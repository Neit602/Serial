// Driver errors become typed errors carrying the platform's fix (F6).

#include "core/errors.h"
#include "core/platform.h"
#include "core/serial_errors.h"

#include <QTest>

using namespace portside;

Q_DECLARE_METATYPE(portside::ErrorKind)
Q_DECLARE_METATYPE(QSerialPort::SerialPortError)

class TestErrors : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void displayIncludesTheRemedy()
    {
        const PortsideError error(ErrorKind::PortBusy, QStringLiteral("COM3 is busy"), QStringLiteral("COM3"),
                                  QStringLiteral("Close PuTTY."));
        QCOMPARE(error.text(), QStringLiteral("COM3: COM3 is busy"));
        QCOMPARE(error.display(), QStringLiteral("COM3: COM3 is busy\n\nClose PuTTY."));
        QCOMPARE(QString::fromUtf8(error.what()), error.text());
        QVERIFY(error.isTransportError());
        QVERIFY(!invalidSettings(QStringLiteral("x")).isTransportError());
    }

    void mapping_data()
    {
        QTest::addColumn<QSerialPort::SerialPortError>("code");
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("whileOpen");
        QTest::addColumn<ErrorKind>("expected");

        QTest::newRow("EACCES") << QSerialPort::PermissionError << QStringLiteral("Permission denied") << false
                                << ErrorKind::PermissionDenied;
        QTest::newRow("EBUSY") << QSerialPort::PermissionError << QStringLiteral("Device or resource busy")
                               << false << ErrorKind::PortBusy;
        QTest::newRow("lock file") << QSerialPort::PermissionError
                                   << QStringLiteral("Permission error while locking the device") << false
                                   << ErrorKind::PortBusy;
        QTest::newRow("windows 5") << QSerialPort::PermissionError << QStringLiteral("Access is denied.") << false
                                   << ErrorKind::PermissionDenied;
        QTest::newRow("windows 32") << QSerialPort::PermissionError
                                    << QStringLiteral("The process cannot access the file because it is being used by another process.")
                                    << false << ErrorKind::PortBusy;
        QTest::newRow("ENOENT") << QSerialPort::DeviceNotFoundError << QStringLiteral("No such file or directory")
                                << false << ErrorKind::PortNotFound;
        QTest::newRow("ENOENT while open") << QSerialPort::DeviceNotFoundError
                                           << QStringLiteral("No such file or directory") << true
                                           << ErrorKind::DeviceVanished;
        QTest::newRow("windows 2") << QSerialPort::DeviceNotFoundError
                                   << QStringLiteral("The system cannot find the file specified.") << false
                                   << ErrorKind::PortNotFound;
        QTest::newRow("EIO") << QSerialPort::ResourceError << QStringLiteral("Input/output error") << true
                             << ErrorKind::DeviceVanished;
        QTest::newRow("unplugged, no text") << QSerialPort::ResourceError << QString() << true
                                            << ErrorKind::DeviceVanished;
        QTest::newRow("permission while open") << QSerialPort::PermissionError << QString() << true
                                               << ErrorKind::PortBusy;
        QTest::newRow("something else") << QSerialPort::UnknownError << QStringLiteral("gremlins") << false
                                        << ErrorKind::Transport;
    }

    void mapping()
    {
        QFETCH(QSerialPort::SerialPortError, code);
        QFETCH(QString, text);
        QFETCH(bool, whileOpen);
        QFETCH(ErrorKind, expected);
        const LinuxPlatform platform;
        const PortsideError error = mapSerialError(code, text, QStringLiteral("/dev/ttyUSB0"), platform, whileOpen);
        QCOMPARE(error.kind(), expected);
        QCOMPARE(error.port(), QStringLiteral("/dev/ttyUSB0"));
        if (expected != ErrorKind::Transport)
            QVERIFY(!error.remedy().isEmpty());
    }

    void theRemedyComesFromThePlatform()
    {
        const WindowsPlatform windows;
        const PortsideError error = mapSerialError(QSerialPort::PermissionError, QStringLiteral("Access is denied."),
                                                   QStringLiteral("COM3"), windows, false);
        QVERIFY(error.remedy().contains(QStringLiteral("Device Manager")));
        const LinuxPlatform linux;
        const PortsideError denied = mapSerialError(QSerialPort::PermissionError, QStringLiteral("Permission denied"),
                                                    QStringLiteral("/dev/ttyUSB0"), linux, false);
        QVERIFY(denied.remedy().contains(QStringLiteral("sudo usermod -aG")));
    }
};

QTEST_GUILESS_MAIN(TestErrors)
#include "tst_errors.moc"
