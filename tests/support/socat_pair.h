#pragma once

// A real pair of virtual serial ports, made by socat (POSIX only).
//
// Both ends are genuine ttys, so everything between the device and the user
// is real: QSerialPort, the session core, pacing and logging.  Tests that
// need one skip cleanly where socat is missing (and on Windows).

#include <QProcess>
#include <QString>
#include <QTemporaryDir>

#include <memory>

namespace portside::test {

class SocatPair
{
public:
    SocatPair();
    ~SocatPair();

    // Whether socat can be run here at all.
    static bool available();

    // Start (or restart) socat; the two ends appear as left() and right().
    bool start();
    // Kill socat: both ends vanish, as if the cable were pulled.
    void stop();

    QString left() const { return m_dir.filePath(QStringLiteral("left")); }
    QString right() const { return m_dir.filePath(QStringLiteral("right")); }

private:
    QTemporaryDir m_dir;
    std::unique_ptr<QProcess> m_process;
};

} // namespace portside::test
