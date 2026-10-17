#pragma once

// `portside connect`: console ↔ port, with no emulation.
//
// The user's own terminal does the VT100 work; Portside moves bytes.  On an
// interactive terminal it also applies the profile's line discipline (what
// Enter and Backspace send, local echo) unless --raw is given, and Ctrl-]
// opens a one-key command menu: quit, break, DTR, RTS, and receiving a file
// the device sends with ZMODEM (F18).  Piped input is always byte-exact, so
// scripts get exactly what they send.

#include "core/textio.h"
#include "core/transfer/transfer.h"
#include "core/transfer/zmodem.h"

#include <QByteArray>
#include <QObject>
#include <QPointer>

#include <functional>

class QTimer;

namespace portside {
class Session;
}

namespace portside::cli {

class ConsoleReader;
}

namespace portside {
class TransferJob;
}

namespace portside::cli {

// Ctrl-] — the telnet escape character.
inline constexpr char kEscapeByte = 0x1D;

struct PassThroughOptions
{
    // stdin is a terminal: Ctrl-] is a command key, not data.
    bool interactive = false;
    // Apply `text` to typed bytes (interactive and not --raw).
    bool translate = false;
    TextPolicy text;
    // Keep reading this long before stopping, so the device's last bytes
    // still reach the screen (DECISIONS D7).
    double drain = 0.25;
    double breakDuration = 0.25;
    bool quiet = false;
    // Where Ctrl-] z saves a ZMODEM download; empty means the current
    // directory.
    QString downloadDirectory;
    OverwritePolicy overwrite = OverwritePolicy::Refuse;
    // Where device bytes and status lines go; stdout and stderr by default.
    std::function<void(const QByteArray &)> output;
    std::function<void(const QString &)> statusSink;
};

class PassThrough : public QObject
{
    Q_OBJECT

public:
    enum class Outcome { Quit, EndOfInput, SessionEnded };

    PassThrough(Session *session, PassThroughOptions options, QObject *parent = nullptr);
    ~PassThrough() override;

    // Pump until the user quits, input ends, or the session ends.
    Outcome run();

    // Also called by tests, which drive the pump without a real console.
    void handleInput(const QByteArray &data);
    void handleEndOfInput();
    bool isFinished() const { return m_finished; }
    Outcome outcome() const { return m_outcome; }

Q_SIGNALS:
    void finished();

private:
    void handleCommand(char key);
    void watchForZmodem(const QByteArray &data);
    void receiveZmodem();
    void showProgress(const TransferProgress &progress);
    QString downloadDirectory() const;
    void sendNow(QByteArray &pending, QByteArray &echo);
    void stopAfterDrain(Outcome outcome);
    void finish(Outcome outcome);
    void status(const QString &text) const;

    QPointer<Session> m_session;
    PassThroughOptions m_options;
    ConsoleReader *m_reader = nullptr;
    QTimer *m_interruptPoll = nullptr;
    bool m_commandPending = false;
    bool m_stopping = false;
    bool m_finished = false;
    bool m_dtr = true;
    bool m_rts = true;
    transfer::zmodem::StartDetector m_detector;
    QPointer<TransferJob> m_transfer;
    qint64 m_lastProgress = 0;
    Outcome m_outcome = Outcome::Quit;
};

} // namespace portside::cli
