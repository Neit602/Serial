#include "core/session_log.h"

#include "core/errors.h"
#include "core/platform.h"
#include "core/session.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

#include <algorithm>
#include <optional>

namespace portside {

QString sanitiseFileComponent(const QString &value)
{
    static const QRegularExpression unsafe(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]"));
    QString cleaned = value;
    cleaned.replace(unsafe, QStringLiteral("_"));
    // Strip leading and trailing dots and spaces, which Windows mangles.
    qsizetype start = 0;
    qsizetype end = cleaned.size();
    const auto strip = [](QChar c) { return c == QLatin1Char('.') || c == QLatin1Char(' '); };
    while (start < end && strip(cleaned.at(start)))
        ++start;
    while (end > start && strip(cleaned.at(end - 1)))
        --end;
    cleaned = cleaned.mid(start, end - start);
    return cleaned.isEmpty() ? QStringLiteral("unnamed") : cleaned;
}

namespace {

std::optional<QString> expand(const QString &pattern, const QHash<QString, QString> &values)
{
    QString out;
    for (qsizetype i = 0; i < pattern.size(); ++i) {
        const QChar c = pattern.at(i);
        if (c == QLatin1Char('{')) {
            if (i + 1 < pattern.size() && pattern.at(i + 1) == QLatin1Char('{')) {
                out += QLatin1Char('{');
                ++i;
                continue;
            }
            const qsizetype close = pattern.indexOf(QLatin1Char('}'), i + 1);
            if (close < 0)
                return std::nullopt;
            const QString key = pattern.mid(i + 1, close - i - 1);
            const auto it = values.constFind(key);
            if (it == values.constEnd())
                return std::nullopt;
            out += *it;
            i = close;
        } else if (c == QLatin1Char('}')) {
            if (i + 1 < pattern.size() && pattern.at(i + 1) == QLatin1Char('}')) {
                out += QLatin1Char('}');
                ++i;
                continue;
            }
            return std::nullopt;
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace

QString resolveTemplate(const QString &pattern, const QString &port, const QString &profile,
                        const QDateTime &when, int index)
{
    const QDateTime moment = when.isValid() ? when : QDateTime::currentDateTime();
    const QString safePort = sanitiseFileComponent(port);
    const QHash<QString, QString> values = {
        {QStringLiteral("port"), safePort},
        {QStringLiteral("device"), safePort},
        {QStringLiteral("date"), moment.toString(QStringLiteral("yyyyMMdd"))},
        {QStringLiteral("time"), moment.toString(QStringLiteral("HHmmss"))},
        {QStringLiteral("profile"),
         profile.isEmpty() ? QStringLiteral("default") : sanitiseFileComponent(profile)},
        {QStringLiteral("n"), QString::number(index)},
    };
    if (auto expanded = expand(pattern, values))
        return *expanded;
    return *expand(kDefaultLogTemplate, values);
}

SessionLogger::SessionLogger(LogSettings settings, QString port, QString profile, Clock now)
    : m_settings(std::move(settings))
    , m_port(std::move(port))
    , m_profile(std::move(profile))
    , m_now(now ? std::move(now) : Clock(wallClock))
{
}

SessionLogger::~SessionLogger()
{
    detach();
}

QString SessionLogger::start(const QString &path)
{
    if (m_file)
        return m_path;
    const QString target = path.isEmpty() ? defaultPath() : path;
    QDir().mkpath(QFileInfo(target).absolutePath());
    auto file = std::make_unique<QFile>(target);
    // Append: a reconnect continues the same session's log rather than
    // truncating what was already captured.
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append)) {
        throw PortsideError(ErrorKind::Generic,
                            QStringLiteral("cannot open log file %1: %2").arg(target, file->errorString()));
    }
    m_written = file->size();
    m_file = std::move(file);
    m_path = target;
    m_lineOpen = false;
    m_pendingCr = false;
    m_timestamper = Timestamper(m_settings.timestamps, m_now());
    m_decoder = IncrementalTextDecoder(m_settings.charset);
    m_lastFlush = m_now();
    return target;
}

void SessionLogger::stop()
{
    if (!m_file)
        return;
    if (m_lineOpen) {
        m_buffer += '\n';
        m_lineOpen = false;
    }
    flushBuffer();
    m_file->close();
    m_file.reset();
}

void SessionLogger::attach(Session *session)
{
    detach();
    m_connections << QObject::connect(session, &Session::dataReceived, session,
                                      [this](const QByteArray &data) { write(data); });
    // A file transfer's bytes belong in a raw log, which stays byte-exact; a
    // text log gets a note from the front end instead of a megabyte of
    // mojibake (DECISIONS D56).
    m_connections << QObject::connect(session, &Session::claimedDataReceived, session,
                                      [this](const QByteArray &data) {
                                          if (m_settings.format == LogFormat::Raw)
                                              write(data);
                                      });
    m_connections << QObject::connect(
        session, &Session::stateChanged, session,
        [this](SessionState, SessionState state, const QString &) {
            if (m_settings.autoStart && state == SessionState::Connected) {
                try {
                    start();
                } catch (const PortsideError &) {
                    // The session matters more than its log; the UI reports
                    // failures from explicit starts.
                }
            } else if (state == SessionState::Closed || state == SessionState::Disconnected) {
                stop();
            }
        });
    m_connections << QObject::connect(session, &Session::errorRaised, session,
                                      [this](const PortsideError &error, bool) {
                                          if (isLogging())
                                              writeNote(QStringLiteral("error: %1").arg(error.text()));
                                      });
}

void SessionLogger::detach()
{
    for (const auto &connection : std::as_const(m_connections))
        QObject::disconnect(connection);
    m_connections.clear();
    stop();
}

void SessionLogger::write(const QByteArray &data)
{
    if (!m_file || data.isEmpty())
        return;
    m_buffer += m_settings.format == LogFormat::Raw ? data : asText(data);
    maybeFlush();
}

void SessionLogger::writeNote(const QString &note)
{
    if (!m_file || m_settings.format == LogFormat::Raw)
        return;
    if (m_lineOpen) {
        m_buffer += '\n';
        m_lineOpen = false;
    }
    m_buffer += (m_timestamper.prefix(m_now()) + QStringLiteral("[portside] ") + note
                 + QLatin1Char('\n'))
                    .toUtf8();
    maybeFlush();
}

QByteArray SessionLogger::asText(const QByteArray &data)
{
    const std::u32string text = m_decoder.decode(data);
    if (text.empty())
        return {};
    QString out;
    out.reserve(qsizetype(text.size()) + 32);
    for (const char32_t raw : text) {
        char32_t c = raw;
        // Internally LF, on every platform: CR, LF and CRLF all become LF,
        // including a CRLF split across two reads.
        if (c == U'\n' && m_pendingCr) {
            m_pendingCr = false;
            continue;
        }
        m_pendingCr = c == U'\r';
        if (c == U'\r')
            c = U'\n';
        if (!m_lineOpen) {
            // A line is stamped when its first character arrives, not when it
            // is completed — otherwise a slow device skews every stamp.
            out += m_timestamper.prefix(m_now());
            m_lineOpen = true;
        }
        out += QString::fromUcs4(&c, 1);
        if (c == U'\n')
            m_lineOpen = false;
    }
    return out.toUtf8();
}

void SessionLogger::maybeFlush()
{
    const double moment = m_now();
    const bool due = moment - m_lastFlush >= m_settings.flushInterval;
    if (due || rotationDue()) {
        flushBuffer();
        m_lastFlush = moment;
        rotateIfNeeded();
    }
}

bool SessionLogger::rotationDue() const
{
    return m_settings.maxBytes > 0 && bytesWritten() >= m_settings.maxBytes;
}

void SessionLogger::flushBuffer()
{
    if (!m_file || m_buffer.isEmpty())
        return;
    m_file->write(m_buffer);
    m_file->flush();
    m_written += m_buffer.size();
    m_buffer.clear();
}

void SessionLogger::flush()
{
    flushBuffer();
}

namespace {

// "name.3.log" style rotated siblings of `path`, with their numbers.
QList<std::pair<int, QString>> rotatedSiblings(const QString &path)
{
    const QFileInfo info(path);
    const QString suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    const QRegularExpression pattern(QStringLiteral("^%1\\.(\\d+)%2$")
                                         .arg(QRegularExpression::escape(info.completeBaseName()),
                                              QRegularExpression::escape(suffix)));
    QList<std::pair<int, QString>> found;
    const QDir dir = info.absoluteDir();
    for (const QString &name : dir.entryList(QDir::Files)) {
        const auto match = pattern.match(name);
        if (match.hasMatch())
            found.append({match.captured(1).toInt(), dir.filePath(name)});
    }
    std::sort(found.begin(), found.end());
    return found;
}

} // namespace

void SessionLogger::rotateIfNeeded()
{
    if (!rotationDue() || !m_file)
        return;
    flushBuffer();
    m_file->close();
    m_file.reset();
    ++m_rotations;
    // Number past any rotated file already there, so an earlier session's
    // rotations are never overwritten.
    const auto existing = rotatedSiblings(m_path);
    const int next = existing.isEmpty() ? 1 : existing.last().first + 1;
    const QFileInfo info(m_path);
    const QString suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    const QString rotated =
        info.absoluteDir().filePath(QStringLiteral("%1.%2%3").arg(info.completeBaseName()).arg(next).arg(suffix));
    QFile::rename(m_path, rotated);
    prune();
    auto file = std::make_unique<QFile>(m_path);
    if (file->open(QIODevice::WriteOnly | QIODevice::Append))
        m_file = std::move(file);
    m_written = 0;
}

void SessionLogger::prune()
{
    if (m_settings.keep <= 0)
        return;
    const auto rotated = rotatedSiblings(m_path);
    const qsizetype excess = rotated.size() - m_settings.keep;
    for (qsizetype i = 0; i < excess; ++i)
        QFile::remove(rotated.at(i).second);
}

QString SessionLogger::defaultPath() const
{
    const QString directory = m_settings.directory.isEmpty() ? Platform::logDir()
                                                             : m_settings.directory;
    const QDateTime when =
        QDateTime::fromMSecsSinceEpoch(qint64(m_now() * 1000.0));
    return QDir(directory).filePath(resolveTemplate(m_settings.filenameTemplate, m_port, m_profile, when));
}

void SessionLogger::resetElapsed()
{
    m_timestamper.reset(m_now());
}

void SessionLogger::restamp(TimestampMode mode)
{
    m_settings.timestamps = mode;
    m_timestamper.setMode(mode);
}

} // namespace portside
