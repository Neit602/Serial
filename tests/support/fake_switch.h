#pragma once

// A scripted console that behaves like a network switch (POSIX only).
//
// The PRD's exit criterion is "configure a real Cisco switch", which CI cannot
// do.  This is the closest honest substitute: a device on the far end of a
// real (socat) serial pair that echoes like a console, walks the user >
// enable > configure prompts, records the configuration it is given, and
// notices a break.
//
// It is deliberately *not* a Cisco emulator — it implements only the
// behaviours Portside's features have to cope with: echo, prompts that appear
// after a delay, a password step, and a break that drops to a boot monitor.

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QStringList>

#include <atomic>
#include <thread>

namespace portside::test {

class FakeSwitch
{
public:
    explicit FakeSwitch(QString port, int promptDelayMs = 0);
    ~FakeSwitch();

    bool start();
    void stop();

    QStringList configLines() const;
    QStringList commands() const;
    int breaksSeen() const { return m_breaks.load(); }
    QString prompt() const;

private:
    void run();
    void feedByte(char byte);
    void handleLine(const QString &line);
    void write(const QString &text);

    QString m_port;
    int m_promptDelayMs;
    int m_fd = -1;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_breaks{0};

    mutable QMutex m_mutex;
    QString m_hostname = QStringLiteral("Switch");
    QString m_mode = QStringLiteral("user");
    QByteArray m_buffer;
    QStringList m_config;
    QStringList m_commands;
};

} // namespace portside::test
