#include "core/pacing.h"

#include "core/session.h"

#include <QTimer>

#include <algorithm>
#include <cmath>

namespace portside {

namespace {

// Keep only enough received text for a prompt to match across chunks.
constexpr qsizetype kPromptContext = 4096;

int toMillis(double seconds)
{
    return int(std::lround(std::max(0.0, seconds) * 1000.0));
}

} // namespace

void PacePolicy::validate() const
{
    if (charDelay < 0 || lineDelay < 0)
        throw PortsideError(ErrorKind::InvalidSettings, QStringLiteral("paste delays cannot be negative"));
    if (promptTimeout <= 0)
        throw PortsideError(ErrorKind::InvalidSettings, QStringLiteral("prompt timeout must be positive"));
    if (!waitForPrompt.isEmpty()) {
        const QRegularExpression pattern(waitForPrompt);
        if (!pattern.isValid()) {
            throw PortsideError(ErrorKind::InvalidSettings,
                                QStringLiteral("invalid prompt pattern: %1").arg(pattern.errorString()));
        }
    }
}

QString PasteResult::describe() const
{
    QString summary = QStringLiteral("%1 %2/%3 lines, %4 B")
                          .arg(aborted ? QStringLiteral("aborted") : QStringLiteral("sent"))
                          .arg(linesSent)
                          .arg(totalLines)
                          .arg(bytesSent);
    if (!timedOutLines.isEmpty())
        summary += QStringLiteral(", %1 prompt timeout(s)").arg(timedOutLines.size());
    return summary;
}

QStringList splitPasteLines(const QString &text)
{
    QStringList lines;
    QString current;
    bool pendingBreak = false; // the last thing seen was a line break
    for (qsizetype i = 0; i < text.size(); ++i) {
        const char16_t c = text.at(i).unicode();
        const bool isBreak = c == u'\n' || c == u'\r' || c == 0x0B || c == 0x0C || c == 0x1C
                             || c == 0x1D || c == 0x1E || c == 0x85 || c == 0x2028 || c == 0x2029;
        if (!isBreak) {
            current += text.at(i);
            pendingBreak = false;
            continue;
        }
        lines << current;
        current.clear();
        pendingBreak = true;
        if (c == u'\r' && i + 1 < text.size() && text.at(i + 1) == u'\n')
            ++i;
    }
    if (!current.isEmpty() || (!pendingBreak && !text.isEmpty()))
        lines << current;
    return lines;
}

PasteJob::PasteJob(Session *session, const QString &text, PacePolicy pace, TextPolicy textPolicy,
                   QObject *parent)
    : QObject(parent)
    , m_session(session)
    , m_lines(splitPasteLines(text))
    , m_pace(std::move(pace))
    , m_text(textPolicy)
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_result.totalLines = int(m_lines.size());
}

void PasteJob::start()
{
    if (m_running)
        return;
    m_pace.validate();
    m_usePrompt = !m_pace.waitForPrompt.isEmpty();
    if (m_usePrompt)
        m_prompt = QRegularExpression(m_pace.waitForPrompt);
    m_running = true;
    m_index = 0;
    m_clock.start();
    if (m_usePrompt && m_session)
        connect(m_session, &Session::dataReceived, this, &PasteJob::onData);
    // Start from the event loop, so callers can connect to finished() even
    // for an empty paste.
    m_timer->disconnect();
    connect(m_timer, &QTimer::timeout, this, &PasteJob::sendNextLine);
    m_timer->start(0);
}

void PasteJob::cancel()
{
    if (m_running)
        finish(true);
}

bool PasteJob::ensureConnected()
{
    if (m_session && m_session->isConnected())
        return true;
    const QString port = m_session ? m_session->port() : QString();
    finish(true, sessionError(QStringLiteral("the session ended during the paste after %1 of %2 lines")
                                  .arg(m_result.linesSent)
                                  .arg(m_result.totalLines),
                              port));
    return false;
}

void PasteJob::sendNextLine()
{
    if (!m_running)
        return;
    if (m_index >= m_lines.size()) {
        finish(false);
        return;
    }
    if (!ensureConnected())
        return;
    if (m_usePrompt) {
        // Forget anything seen before this line was sent.
        m_promptSeen = false;
        m_promptBuffer.clear();
        m_decoder.reset();
    }
    const QByteArray payload = m_text.encodeLine(m_lines.at(m_index));
    if (m_pace.charDelay <= 0) {
        m_session->send(payload);
        m_result.bytesSent += payload.size();
        lineSent();
        return;
    }
    m_payload = payload;
    m_charIndex = 0;
    sendNextChar();
}

void PasteJob::sendNextChar()
{
    if (!m_running || !ensureConnected())
        return;
    if (m_charIndex < m_payload.size()) {
        m_session->send(m_payload.mid(m_charIndex, 1));
        ++m_charIndex;
        ++m_result.bytesSent;
    }
    m_timer->disconnect();
    if (m_charIndex < m_payload.size())
        connect(m_timer, &QTimer::timeout, this, &PasteJob::sendNextChar);
    else
        connect(m_timer, &QTimer::timeout, this, &PasteJob::lineSent);
    m_timer->start(toMillis(m_pace.charDelay));
}

void PasteJob::lineSent()
{
    if (!m_running)
        return;
    m_result.linesSent = m_index + 1;
    // Nothing follows the last line, so waiting for one more prompt only
    // delays the paste — and turns a quiet device into a spurious timeout
    // (DECISIONS D19).
    const bool isLast = m_index == m_lines.size() - 1;
    m_timer->disconnect();
    if (m_usePrompt && !isLast) {
        if (m_promptSeen) {
            promptResolved(true);
            return;
        }
        m_waitingForPrompt = true;
        connect(m_timer, &QTimer::timeout, this, [this] { promptResolved(false); });
        m_timer->start(toMillis(m_pace.promptTimeout));
        return;
    }
    if (m_pace.lineDelay > 0 && !isLast) {
        connect(m_timer, &QTimer::timeout, this, [this] { advance(false); });
        m_timer->start(toMillis(m_pace.lineDelay));
        return;
    }
    connect(m_timer, &QTimer::timeout, this, [this] { advance(false); });
    m_timer->start(0);
}

void PasteJob::onData(const QByteArray &data)
{
    if (!m_running || !m_usePrompt)
        return;
    m_promptBuffer += m_decoder.decodeToString(data);
    if (m_promptBuffer.size() > kPromptContext)
        m_promptBuffer = m_promptBuffer.right(kPromptContext);
    if (!m_promptSeen && m_prompt.match(m_promptBuffer).hasMatch()) {
        m_promptSeen = true;
        if (m_waitingForPrompt) {
            m_timer->stop();
            promptResolved(true);
        }
    }
}

void PasteJob::promptResolved(bool seen)
{
    m_waitingForPrompt = false;
    if (!seen) {
        m_result.timedOutLines << m_index + 1;
        if (m_pace.abortOnTimeout) {
            report(false);
            finish(true);
            return;
        }
    }
    advance(seen);
}

void PasteJob::advance(bool waited)
{
    if (!m_running)
        return;
    report(waited);
    ++m_index;
    m_timer->disconnect();
    connect(m_timer, &QTimer::timeout, this, &PasteJob::sendNextLine);
    m_timer->start(0);
}

void PasteJob::report(bool waited)
{
    Q_EMIT progress({m_index + 1, m_result.totalLines, m_result.bytesSent, waited});
}

void PasteJob::finish(bool aborted, std::optional<PortsideError> error)
{
    if (!m_running)
        return;
    m_running = false;
    m_timer->stop();
    m_timer->disconnect();
    if (m_session)
        disconnect(m_session, &Session::dataReceived, this, &PasteJob::onData);
    m_result.aborted = aborted;
    m_result.elapsed = m_clock.isValid() ? m_clock.elapsed() / 1000.0 : 0.0;
    m_result.error = std::move(error);
    Q_EMIT finished(m_result);
}

} // namespace portside
