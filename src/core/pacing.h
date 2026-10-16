#pragma once

// Paced paste (PRD F13).
//
// Pasting a config into a console at full speed is the classic way to lose
// half of it: network gear echoes, parses and sometimes drops characters, and
// has no flow control worth trusting.  Portside sends it the way a careful
// human would — a delay between characters, a longer one between lines, and
// optionally waiting for the device's prompt before each next line.

#include "core/decoder.h"
#include "core/errors.h"
#include "core/textio.h"

#include <QElapsedTimer>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QRegularExpression>
#include <QStringList>

#include <optional>

class QTimer;

namespace portside {

class Session;

// How fast to feed text to the device (F13).
struct PacePolicy
{
    // Seconds between characters (0 sends each line in one write).
    double charDelay = 0.0;
    // Seconds after each line ending.
    double lineDelay = 0.05;
    // Regex the device must emit before the next line goes out.  Far more
    // reliable than a fixed delay on gear whose parse time varies.
    QString waitForPrompt;
    // How long to wait for that prompt, in seconds.
    double promptTimeout = 5.0;
    // Stop the paste when the prompt never arrives, rather than pressing on
    // into a device that is not listening.
    bool abortOnTimeout = true;

    // Throws PortsideError for negative delays or an invalid prompt pattern.
    void validate() const;

    bool operator==(const PacePolicy &) const = default;
};

// Emitted after each line so a UI can show a progress bar (F13, F22).
struct PasteProgress
{
    int line = 0;
    int totalLines = 0;
    qint64 bytesSent = 0;
    bool waitedForPrompt = false;

    double fraction() const { return totalLines ? double(line) / totalLines : 1.0; }
};

// Outcome of a paste, however it ended.
struct PasteResult
{
    int linesSent = 0;
    int totalLines = 0;
    qint64 bytesSent = 0;
    double elapsed = 0.0;
    bool aborted = false;
    QList<int> timedOutLines;
    // Set when the paste stopped because something failed.
    std::optional<PortsideError> error;

    // True when every line reached the device.
    bool complete() const { return !aborted && linesSent == totalLines; }
    // One line for the status bar or the log.
    QString describe() const;
};

// Split text into lines the way a paste sees them: CRLF, CR and LF (and the
// rarer Unicode separators) each end a line; a trailing break adds no line.
QStringList splitPasteLines(const QString &text);

// Sends one paste.  Lives on the session's thread; driven by timers, so it
// never blocks.  Cancelling stops it between characters and leaves the session
// healthy.
class PasteJob : public QObject
{
    Q_OBJECT

public:
    PasteJob(Session *session, const QString &text, PacePolicy pace = {},
             TextPolicy textPolicy = {}, QObject *parent = nullptr);

    void start();
    void cancel();
    bool isRunning() const { return m_running; }
    const PasteResult &result() const { return m_result; }

Q_SIGNALS:
    void progress(const portside::PasteProgress &progress);
    void finished(const portside::PasteResult &result);

private:
    void sendNextLine();
    void sendNextChar();
    void lineSent();
    void promptResolved(bool seen);
    void advance(bool waited);
    void onData(const QByteArray &data);
    void finish(bool aborted, std::optional<PortsideError> error = std::nullopt);
    bool ensureConnected();
    void report(bool waited);

    QPointer<Session> m_session;
    QStringList m_lines;
    PacePolicy m_pace;
    TextPolicy m_text;
    QRegularExpression m_prompt;
    bool m_usePrompt = false;

    QTimer *m_timer = nullptr;
    QElapsedTimer m_clock;
    int m_index = 0;        // 0-based line being sent
    QByteArray m_payload;   // current line, when sending char by char
    qsizetype m_charIndex = 0;
    bool m_running = false;
    bool m_waitingForPrompt = false;
    bool m_promptSeen = false;
    QString m_promptBuffer;
    IncrementalTextDecoder m_decoder;
    PasteResult m_result;
};

} // namespace portside

Q_DECLARE_METATYPE(portside::PasteProgress)
Q_DECLARE_METATYPE(portside::PasteResult)
