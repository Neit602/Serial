#include "core/transfer/engine.h"

#include "core/session_log.h"

#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace portside::transfer {

// -- Link -------------------------------------------------------------------

namespace {

// The longest a wait goes without looking at the cancel flag.
constexpr int kCancelPollMs = 50;

} // namespace

Link::Link(ByteChannel &channel, const std::atomic<bool> &cancel)
    : m_channel(channel)
    , m_cancel(cancel)
{
}

void Link::checkCancelled() const
{
    if (m_cancel.load())
        throw PortsideError(ErrorKind::Cancelled, QStringLiteral("transfer cancelled"));
}

bool Link::fill(int timeoutMs)
{
    QDeadlineTimer deadline(qMax(0, timeoutMs));
    for (;;) {
        checkCancelled();
        const int slice = int(qMin<qint64>(kCancelPollMs, deadline.remainingTime()));
        QByteArray data = m_channel.read(qMax(0, slice));
        if (!data.isEmpty()) {
            if (m_offset >= m_buffer.size()) {
                m_buffer = std::move(data);
                m_offset = 0;
            } else {
                m_buffer.remove(0, m_offset);
                m_offset = 0;
                m_buffer += data;
            }
            return true;
        }
        if (deadline.hasExpired())
            return false;
    }
}

int Link::getByte(int timeoutMs)
{
    if (m_offset >= m_buffer.size() && !fill(timeoutMs))
        return -1;
    ++m_read;
    return quint8(m_buffer.at(m_offset++));
}

void Link::unget(quint8 byte)
{
    --m_read;
    if (m_offset > 0) {
        m_buffer[--m_offset] = char(byte);
        return;
    }
    m_buffer.prepend(char(byte));
}

bool Link::hasInput()
{
    if (m_offset < m_buffer.size())
        return true;
    return fill(0);
}

void Link::put(const QByteArray &data)
{
    if (data.isEmpty())
        return;
    m_channel.write(data);
    m_written += data.size();
}

void Link::put(char byte)
{
    put(QByteArray(1, byte));
}

void Link::purge(int quietMs, int maxMs)
{
    m_buffer.clear();
    m_offset = 0;
    if (quietMs <= 0)
        return;
    QDeadlineTimer overall(maxMs);
    while (!overall.hasExpired()) {
        checkCancelled();
        if (m_channel.read(quietMs).isEmpty())
            return;
    }
}

void fail(const QString &message, const QString &remedy)
{
    throw transferError(message, remedy);
}

void failCancelledByPeer()
{
    fail(QStringLiteral("the other end cancelled the transfer"));
}

// -- Job ----------------------------------------------------------------------

Job::Job(ByteChannel &channel, const TransferOptions &options_, const std::atomic<bool> &cancel,
         const ProgressCallback &callback, TransferDirection direction)
    : link(channel, cancel)
    , options(options_)
    , m_callback(callback)
{
    result.protocol = options.protocol;
    result.direction = direction;
    m_clock.start();
}

void Job::report(bool force)
{
    if (!m_callback)
        return;
    const qint64 now = m_clock.elapsed();
    if (!force && now - m_lastReport < 100)
        return;
    m_lastReport = now;
    m_progress.seconds = double(now) / 1000.0;
    m_callback(m_progress);
}

void Job::beginFile(const QString &name, qint64 size, int index, int count)
{
    m_progress.file = name;
    m_progress.fileSize = size;
    m_progress.fileBytes = 0;
    m_progress.fileIndex = index;
    m_progress.fileCount = count;
    report(true);
}

void Job::advance(qint64 bytes)
{
    m_progress.fileBytes += bytes;
    m_progress.totalBytes += bytes;
    report(false);
}

void Job::setFileBytes(qint64 bytes)
{
    m_progress.totalBytes += bytes - m_progress.fileBytes;
    m_progress.fileBytes = bytes;
    report(false);
}

void Job::retry()
{
    ++m_progress.retries;
    ++result.retries;
    report(false);
}

void Job::endFile(const QString &path, qint64 bytes, bool skipped, const QString &note)
{
    TransferredFile file;
    file.name = m_progress.file;
    file.path = path;
    file.bytes = bytes;
    file.skipped = skipped;
    file.note = note;
    result.files << file;
    if (!skipped)
        result.bytes += bytes;
    report(true);
}

TransferResult Job::finish()
{
    result.seconds = double(m_clock.elapsed()) / 1000.0;
    report(true);
    return result;
}

// -- OutgoingFile ---------------------------------------------------------------

OutgoingFile::OutgoingFile(const QString &path)
    : m_path(path)
    , m_file(path)
{
    const QFileInfo info(path);
    if (!info.exists())
        fail(QStringLiteral("%1: no such file").arg(path));
    if (!info.isFile())
        fail(QStringLiteral("%1: not a regular file").arg(path));
    if (!m_file.open(QIODevice::ReadOnly))
        fail(QStringLiteral("%1: cannot read: %2").arg(path, m_file.errorString()));
    m_name = info.fileName();
    m_size = info.size();
    m_modified = info.lastModified();
}

QByteArray OutgoingFile::read(qint64 maxBytes)
{
    QByteArray data = m_file.read(maxBytes);
    if (data.isEmpty() && !m_file.atEnd())
        fail(QStringLiteral("%1: read failed: %2").arg(m_path, m_file.errorString()));
    return data;
}

void OutgoingFile::seek(qint64 position)
{
    if (!m_file.seek(qBound<qint64>(0, position, m_size)))
        fail(QStringLiteral("%1: cannot seek to %2").arg(m_path).arg(position));
}

std::vector<std::unique_ptr<OutgoingFile>> openAll(const QStringList &paths)
{
    if (paths.isEmpty())
        fail(QStringLiteral("no files to send"));
    std::vector<std::unique_ptr<OutgoingFile>> files;
    for (const QString &path : paths)
        files.push_back(std::make_unique<OutgoingFile>(path));
    return files;
}

// -- IncomingFile ---------------------------------------------------------------

IncomingFile::IncomingFile(QString finalPath, QString partPath, qint64 resumeFrom)
    : m_finalPath(std::move(finalPath))
    , m_partPath(std::move(partPath))
    , m_file(m_partPath)
{
    const QIODevice::OpenMode mode =
        resumeFrom > 0 ? QIODevice::WriteOnly | QIODevice::Append
                       : QIODevice::WriteOnly | QIODevice::Truncate;
    if (!m_file.open(mode))
        fail(QStringLiteral("%1: cannot create: %2").arg(m_partPath, m_file.errorString()),
             QStringLiteral("Choose a download folder you can write to."));
    m_written = resumeFrom > 0 ? m_file.size() : 0;
    m_resumedFrom = m_written;
}

IncomingFile::~IncomingFile()
{
    if (!m_done)
        abandon(m_keepPartial);
}

void IncomingFile::write(const QByteArray &data)
{
    if (data.isEmpty())
        return;
    if (m_file.write(data) != data.size())
        fail(QStringLiteral("%1: write failed: %2").arg(m_partPath, m_file.errorString()),
             QStringLiteral("Check that the disk is not full."));
    m_written += data.size();
}

void IncomingFile::truncate(qint64 size)
{
    if (size >= m_written)
        return;
    m_file.flush();
    if (!m_file.resize(size))
        fail(QStringLiteral("%1: cannot truncate: %2").arg(m_partPath, m_file.errorString()));
    m_file.seek(size);
    m_written = size;
}

QString IncomingFile::commit(const QDateTime &modified)
{
    m_file.close();
    if (QFileInfo::exists(m_finalPath))
        QFile::remove(m_finalPath);
    if (!QFile::rename(m_partPath, m_finalPath))
        fail(QStringLiteral("%1: cannot rename the finished download into place").arg(m_finalPath));
    if (modified.isValid()) {
        QFile done(m_finalPath);
        if (done.open(QIODevice::ReadWrite))
            done.setFileTime(modified, QFileDevice::FileModificationTime);
    }
    m_done = true;
    return m_finalPath;
}

void IncomingFile::abandon(bool keepPartial)
{
    if (m_done)
        return;
    m_done = true;
    m_file.close();
    if (!keepPartial || m_written == 0)
        QFile::remove(m_partPath);
}

// -- ReceiveDirectory -------------------------------------------------------------

ReceiveDirectory::ReceiveDirectory(const TransferOptions &options)
    : m_directory(options.directory.isEmpty() ? QDir::currentPath() : options.directory)
    , m_policy(options.overwrite)
{
}

QString ReceiveDirectory::safeName(const QString &peerName)
{
    // Only the last component: a device says "../../.bashrc" or "C:\boot.ini"
    // and gets ".bashrc" turned into "bashrc".
    QString name = peerName;
    const qsizetype slash = std::max(name.lastIndexOf(QLatin1Char('/')),
                                     name.lastIndexOf(QLatin1Char('\\')));
    if (slash >= 0)
        name = name.mid(slash + 1);
    name = sanitiseFileComponent(name.trimmed());
    // Names Windows reserves for devices, whatever the extension.
    static const QRegularExpression reserved(
        QStringLiteral("^(con|prn|aux|nul|com[0-9]|lpt[0-9])(\\..*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(name).hasMatch())
        name.prepend(QLatin1Char('_'));
    return name.left(200);
}

std::unique_ptr<IncomingFile> ReceiveDirectory::open(const QString &peerName, QString *why,
                                                     bool resume)
{
    const QDir dir(m_directory);
    if (!dir.exists() && !QDir().mkpath(m_directory))
        fail(QStringLiteral("%1: cannot create the download folder").arg(m_directory),
             QStringLiteral("Choose a download folder you can write to."));
    const QString name = safeName(peerName);
    QString target = dir.filePath(name);
    if (QFileInfo::exists(target)) {
        switch (m_policy) {
        case OverwritePolicy::Refuse:
            if (why != nullptr)
                *why = QStringLiteral("%1 already exists").arg(target);
            return nullptr;
        case OverwritePolicy::Rename: {
            const QFileInfo info(name);
            const QString base = info.completeBaseName().isEmpty() ? name : info.completeBaseName();
            const QString suffix = info.suffix().isEmpty() || info.completeBaseName().isEmpty()
                                       ? QString()
                                       : QLatin1Char('.') + info.suffix();
            for (int n = 1;; ++n) {
                target = dir.filePath(QStringLiteral("%1.%2%3").arg(base).arg(n).arg(suffix));
                if (!QFileInfo::exists(target))
                    break;
            }
            break;
        }
        case OverwritePolicy::Overwrite:
            break;
        }
    }
    const QString part = target + QStringLiteral(".part");
    const qint64 resumeFrom = resume && QFileInfo::exists(part) ? QFileInfo(part).size() : 0;
    return std::make_unique<IncomingFile>(target, part, resumeFrom);
}

} // namespace portside::transfer
