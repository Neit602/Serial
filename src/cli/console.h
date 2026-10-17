#pragma once

// Raw terminal I/O for `portside connect`.
//
// The POSIX and Windows implementations sit behind one interface, so the rest
// of the CLI contains no platform branches.  The console always restores the
// terminal, including when the session ends with an error.

#include <QByteArray>
#include <QObject>
#include <QString>

#include <atomic>
#include <thread>

namespace portside::cli {

class Console
{
public:
    Console();
    ~Console();
    Console(const Console &) = delete;
    Console &operator=(const Console &) = delete;

    // True if stdin / stdout are interactive terminals.
    static bool stdinIsTty();
    static bool stdoutIsTty();
    static bool stderrIsTty();

    // Switch stdin to raw mode (no-op when it is not a terminal).
    void enterRaw();
    // Restore the mode it was in.  Idempotent.
    void restore();

    // Device bytes go to stdout, untranslated.
    static void writeOut(const QByteArray &data);
    // Status messages go to stderr, so piped session data stays clean
    // (DECISIONS D9).
    static void writeErr(const QString &text);

private:
    struct Saved;
    Saved *m_saved = nullptr;
};

// Reads stdin on a thread of its own and delivers what it read as signals on
// the owner's thread.  `ended` fires at end of input (Ctrl-D, closed pipe).
class ConsoleReader : public QObject
{
    Q_OBJECT

public:
    explicit ConsoleReader(QObject *parent = nullptr);
    ~ConsoleReader() override;

    void start();
    void stop();

Q_SIGNALS:
    void received(const QByteArray &data);
    void ended();

private:
    void run();

    std::thread m_thread;
    std::atomic<bool> m_stop{false};
};

} // namespace portside::cli
