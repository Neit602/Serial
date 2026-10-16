#include "cli/pass_through.h"

#include "cli/console.h"
#include "core/session.h"

#include <QEventLoop>
#include <QFutureWatcher>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>

namespace portside::cli {

namespace {

std::atomic<bool> g_interrupted{false};

extern "C" void onInterrupt(int)
{
    g_interrupted = true;
}

} // namespace

PassThrough::PassThrough(Session *session, PassThroughOptions options, QObject *parent)
    : QObject(parent)
    , m_session(session)
    , m_options(options)
{
    if (!m_options.output)
        m_options.output = [](const QByteArray &bytes) { Console::writeOut(bytes); };
    if (!m_options.statusSink)
        m_options.statusSink = [](const QString &text) { Console::writeErr(text); };
    connect(session, &Session::dataReceived, this,
            [this](const QByteArray &bytes) { m_options.output(bytes); });
    connect(session, &Session::stateChanged, this,
            [this](SessionState, SessionState state, const QString &) {
                if (state == SessionState::Disconnected || state == SessionState::Closed)
                    finish(Outcome::SessionEnded);
            });
}

PassThrough::~PassThrough()
{
    if (m_reader != nullptr)
        m_reader->stop();
}

PassThrough::Outcome PassThrough::run()
{
    if (m_finished)
        return m_outcome;
    m_reader = new ConsoleReader(this);
    connect(m_reader, &ConsoleReader::received, this, &PassThrough::handleInput);
    connect(m_reader, &ConsoleReader::ended, this, &PassThrough::handleEndOfInput);

    // Ctrl-C only raises a signal when the terminal is not raw (piped input);
    // either way it ends the session cleanly.
    g_interrupted = false;
    const auto previous = std::signal(SIGINT, onInterrupt);
    m_interruptPoll = new QTimer(this);
    m_interruptPoll->setInterval(100);
    connect(m_interruptPoll, &QTimer::timeout, this, [this] {
        if (g_interrupted)
            finish(Outcome::Quit);
    });
    m_interruptPoll->start();

    QEventLoop loop;
    connect(this, &PassThrough::finished, &loop, &QEventLoop::quit);
    m_reader->start();
    if (!m_finished)
        loop.exec();
    m_reader->stop();
    m_interruptPoll->stop();
    std::signal(SIGINT, previous);
    return m_outcome;
}

void PassThrough::handleInput(const QByteArray &data)
{
    if (m_stopping || m_finished || !m_session)
        return;
    QByteArray pending;
    QByteArray echo;
    for (const char byte : data) {
        if (m_commandPending) {
            m_commandPending = false;
            sendNow(pending, echo);
            handleCommand(byte);
            if (m_stopping)
                return;
            continue;
        }
        if (m_options.interactive && byte == kEscapeByte) {
            sendNow(pending, echo);
            m_commandPending = true;
            status(QStringLiteral("[portside] q quit · b break · d DTR · r RTS · ] send Ctrl-] · "
                                  "any other key resumes"));
            continue;
        }
        if (!m_options.translate) {
            pending += byte;
            continue;
        }
        if (byte == '\r') {
            const QByteArray ending = endingBytes(m_options.text.sendEnding);
            pending += ending.isEmpty() ? QByteArrayLiteral("\r") : ending;
            echo += "\r\n";
        } else if (byte == '\x7f' || byte == '\x08') {
            pending += backspaceByte(m_options.text.backspace);
            echo += "\b \b";
        } else {
            pending += byte;
            echo += byte;
        }
    }
    sendNow(pending, echo);
}

void PassThrough::sendNow(QByteArray &pending, QByteArray &echo)
{
    if (!pending.isEmpty() && m_session && m_session->isConnected())
        m_session->send(pending);
    if (m_options.translate && m_options.text.localEcho && !echo.isEmpty())
        m_options.output(echo);
    pending.clear();
    echo.clear();
}

void PassThrough::handleCommand(char key)
{
    switch (key) {
    case 'q':
    case 'Q':
    case 'x':
    case '.':
    case kEscapeByte: // Ctrl-] twice
        stopAfterDrain(Outcome::Quit);
        return;
    case ']':
        if (m_session)
            m_session->send(QByteArray(1, kEscapeByte));
        return;
    case 'b':
    case 'B': {
        if (!m_session)
            return;
        auto *watcher = new QFutureWatcher<BreakResult>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
            watcher->deleteLater();
            try {
                status(QStringLiteral("[portside] %1").arg(watcher->result().describe()));
            } catch (const PortsideError &error) {
                status(QStringLiteral("[portside] break failed: %1").arg(error.display()));
            }
        });
        watcher->setFuture(m_session->sendBreak(m_options.breakDuration));
        return;
    }
    case 'd':
    case 'D':
    case 'r':
    case 'R': {
        if (!m_session)
            return;
        const bool dtr = key == 'd' || key == 'D';
        bool &line = dtr ? m_dtr : m_rts;
        line = !line;
        const QString name = dtr ? QStringLiteral("DTR") : QStringLiteral("RTS");
        const bool value = line;
        auto *watcher = new QFutureWatcher<bool>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, name, value] {
            watcher->deleteLater();
            try {
                status(watcher->result()
                           ? QStringLiteral("[portside] %1 %2").arg(name, value ? QStringLiteral("on") : QStringLiteral("off"))
                           : QStringLiteral("[portside] this driver has no %1 line").arg(name));
            } catch (const PortsideError &error) {
                status(QStringLiteral("[portside] %1 failed: %2").arg(name, error.text()));
            }
        });
        watcher->setFuture(dtr ? m_session->setDtr(value) : m_session->setRts(value));
        return;
    }
    default:
        status(QStringLiteral("[portside] resumed"));
        return;
    }
}

void PassThrough::handleEndOfInput()
{
    stopAfterDrain(Outcome::EndOfInput);
}

void PassThrough::stopAfterDrain(Outcome outcome)
{
    if (m_stopping || m_finished)
        return;
    m_stopping = true;
    // Give the device's last bytes time to arrive before the port closes.
    const int millis = int(std::lround(std::max(0.0, m_options.drain) * 1000.0));
    QTimer::singleShot(millis, this, [this, outcome] { finish(outcome); });
}

void PassThrough::finish(Outcome outcome)
{
    if (m_finished)
        return;
    m_finished = true;
    m_outcome = outcome;
    Q_EMIT finished();
}

void PassThrough::status(const QString &text) const
{
    if (!m_options.quiet)
        m_options.statusSink(text);
}

} // namespace portside::cli
