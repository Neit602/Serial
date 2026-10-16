#include "core/textio.h"

namespace portside {

QByteArray endingBytes(LineEnding ending)
{
    switch (ending) {
    case LineEnding::Cr:
        return QByteArrayLiteral("\r");
    case LineEnding::Lf:
        return QByteArrayLiteral("\n");
    case LineEnding::CrLf:
        return QByteArrayLiteral("\r\n");
    case LineEnding::None:
        return {};
    }
    return {};
}

char backspaceByte(Backspace backspace)
{
    return backspace == Backspace::Bs ? '\x08' : '\x7f';
}

namespace {

// Collapse CRLF/CR/LF to one ending, without merging separate breaks.
QByteArray normalise(const QByteArray &data, const QByteArray &ending)
{
    QByteArray out;
    out.reserve(data.size());
    for (qsizetype i = 0; i < data.size(); ++i) {
        const char byte = data.at(i);
        if (byte == '\r') {
            out += ending;
            // A CR followed by LF is one break, not two.
            if (i + 1 < data.size() && data.at(i + 1) == '\n')
                ++i;
        } else if (byte == '\n') {
            out += ending;
        } else {
            out += byte;
        }
    }
    return out;
}

} // namespace

QByteArray TextPolicy::encodeLine(const QString &line) const
{
    return line.toUtf8() + endingBytes(sendEnding);
}

QByteArray TextPolicy::translateOutput(const QByteArray &data) const
{
    if (sendEnding == LineEnding::None)
        return data;
    return normalise(data, endingBytes(sendEnding));
}

QByteArray TextPolicy::translateInput(const QByteArray &data) const
{
    if (receiveEnding == LineEnding::None)
        return data;
    return normalise(data, endingBytes(receiveEnding));
}

QByteArray TextPolicy::echoFor(const QByteArray &data) const
{
    return localEcho ? data : QByteArray();
}

XonXoffGate::XonXoffGate(bool enabled, double maxPause)
    : m_enabled(enabled)
    , m_maxPause(maxPause)
{
}

void XonXoffGate::observe(const QByteArray &data, double now)
{
    if (!m_enabled)
        return;
    for (const char byte : data) {
        if (byte == kXoff)
            m_pausedAt = now;
        else if (byte == kXon)
            m_pausedAt.reset();
    }
}

bool XonXoffGate::isPaused(double now)
{
    if (!m_enabled || !m_pausedAt)
        return false;
    if (now - *m_pausedAt >= m_maxPause) {
        // Treat a lost XON as "resume": a stuck terminal is worse than a
        // possible overrun, and the count makes the event visible.
        m_pausedAt.reset();
        ++m_timeouts;
        return false;
    }
    return true;
}

void XonXoffGate::resume()
{
    m_pausedAt.reset();
}

} // namespace portside
