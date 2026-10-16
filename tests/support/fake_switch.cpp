#include "support/fake_switch.h"

#include <QFile>
#include <QMutexLocker>

#include <chrono>

#if !defined(Q_OS_WIN)
#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace portside::test {

FakeSwitch::FakeSwitch(QString port, int promptDelayMs)
    : m_port(std::move(port))
    , m_promptDelayMs(promptDelayMs)
{
}

FakeSwitch::~FakeSwitch()
{
    stop();
}

bool FakeSwitch::start()
{
#if defined(Q_OS_WIN)
    return false;
#else
    m_fd = ::open(QFile::encodeName(m_port).constData(), O_RDWR | O_NOCTTY);
    if (m_fd < 0)
        return false;
    termios attributes{};
    ::tcgetattr(m_fd, &attributes);
    ::cfmakeraw(&attributes);
    ::tcsetattr(m_fd, TCSANOW, &attributes);
    m_stop = false;
    m_thread = std::thread([this] { run(); });
    return true;
#endif
}

void FakeSwitch::stop()
{
    m_stop = true;
    if (m_thread.joinable())
        m_thread.join();
#if !defined(Q_OS_WIN)
    if (m_fd >= 0)
        ::close(m_fd);
#endif
    m_fd = -1;
}

QStringList FakeSwitch::configLines() const
{
    QMutexLocker lock(&m_mutex);
    return m_config;
}

QStringList FakeSwitch::commands() const
{
    QMutexLocker lock(&m_mutex);
    return m_commands;
}

QString FakeSwitch::prompt() const
{
    if (m_mode == QLatin1String("user"))
        return m_hostname + QLatin1Char('>');
    if (m_mode == QLatin1String("enable"))
        return m_hostname + QLatin1Char('#');
    return m_hostname + QStringLiteral("(config)#");
}

void FakeSwitch::run()
{
#if !defined(Q_OS_WIN)
    char chunk[256];
    while (!m_stop) {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(m_fd, &ready);
        timeval timeout{0, 50'000};
        const int result = ::select(m_fd + 1, &ready, nullptr, nullptr, &timeout);
        if (result <= 0)
            continue;
        const ssize_t got = ::read(m_fd, chunk, sizeof(chunk));
        if (got <= 0)
            return;
        QByteArray bytes(chunk, qsizetype(got));
        // A break arrives as a NUL (or a framing error the driver reports as
        // one); either way the device drops to its boot monitor.
        if (bytes.contains('\0')) {
            ++m_breaks;
            {
                QMutexLocker lock(&m_mutex);
                m_mode = QStringLiteral("rommon");
            }
            write(QStringLiteral("\r\n*** break detected ***\r\nrommon 1 > "));
            bytes.replace(QByteArray(1, '\0'), QByteArray());
        }
        for (const char byte : bytes)
            feedByte(byte);
    }
#endif
}

void FakeSwitch::feedByte(char byte)
{
    if (byte == '\r' || byte == '\n') {
        write(QStringLiteral("\r\n"));
        QString line;
        {
            QMutexLocker lock(&m_mutex);
            line = QString::fromUtf8(m_buffer);
            m_buffer.clear();
        }
        handleLine(line.trimmed());
        return;
    }
    if (byte == '\x08' || byte == '\x7f') {
        bool erased = false;
        {
            QMutexLocker lock(&m_mutex);
            if (!m_buffer.isEmpty()) {
                m_buffer.chop(1);
                erased = true;
            }
        }
        if (erased)
            write(QStringLiteral("\b \b"));
        return;
    }
    {
        QMutexLocker lock(&m_mutex);
        m_buffer += byte;
    }
    write(QString(QLatin1Char(byte)));
}

void FakeSwitch::handleLine(const QString &line)
{
    if (m_promptDelayMs > 0) {
        // Real gear parses before it prompts; pacing has to cope with that.
        std::this_thread::sleep_for(std::chrono::milliseconds(m_promptDelayMs));
    }
    QString reply;
    {
        QMutexLocker lock(&m_mutex);
        if (!line.isEmpty())
            m_commands << line;
        const QString lowered = line.toLower();
        if (m_mode == QLatin1String("rommon")) {
            if (lowered == QLatin1String("boot") || lowered == QLatin1String("reset")) {
                m_mode = QStringLiteral("user");
                reply = QStringLiteral("\r\nbooting...\r\n") + prompt();
            } else {
                reply = QStringLiteral("\r\nrommon 1 > ");
            }
        } else if (lowered == QLatin1String("enable")) {
            m_mode = QStringLiteral("password");
            reply = QStringLiteral("\r\nPassword: ");
        } else if (m_mode == QLatin1String("password")) {
            m_mode = QStringLiteral("enable");
            reply = QStringLiteral("\r\n") + prompt();
        } else if (lowered == QLatin1String("configure terminal") || lowered == QLatin1String("conf t")) {
            m_mode = QStringLiteral("config");
            reply = QStringLiteral("\r\nEnter configuration commands, one per line.  End with CNTL/Z.\r\n")
                    + prompt();
        } else if ((lowered == QLatin1String("end") || lowered == QLatin1String("exit"))
                   && m_mode == QLatin1String("config")) {
            m_mode = QStringLiteral("enable");
            reply = QStringLiteral("\r\n") + prompt();
        } else {
            if (m_mode == QLatin1String("config") && !line.isEmpty()) {
                m_config << line;
                if (line.startsWith(QStringLiteral("hostname ")))
                    m_hostname = line.section(QLatin1Char(' '), 1);
            }
            reply = QStringLiteral("\r\n") + prompt();
        }
    }
    write(reply);
}

void FakeSwitch::write(const QString &text)
{
#if !defined(Q_OS_WIN)
    const QByteArray bytes = text.toUtf8();
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(m_fd, bytes.constData() + offset, size_t(bytes.size() - offset));
        if (written <= 0)
            return;
        offset += written;
    }
#else
    Q_UNUSED(text);
#endif
}

} // namespace portside::test
