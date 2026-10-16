// Discovery, identity, chip names and the platform backends (F1, F2, F6).
//
// Every platform backend runs on every OS here, against synthetic port lists
// and a fake sysfs, so Windows and macOS logic is covered on Linux and vice
// versa.

#include "core/chips.h"
#include "core/discovery.h"
#include "core/factory.h"
#include "core/identity.h"
#include "core/loopback_transport.h"
#include "core/platform.h"

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace portside;

namespace {

RawPort usb(const QString &device, quint16 vid, quint16 pid, const QString &serial = {},
            const QString &description = QStringLiteral("USB Serial"))
{
    RawPort raw;
    raw.device = device;
    raw.vid = vid;
    raw.pid = pid;
    raw.serialNumber = serial;
    raw.description = description;
    return raw;
}

RawPort plain(const QString &device)
{
    RawPort raw;
    raw.device = device;
    return raw;
}

PortLister listerOf(QList<RawPort> ports)
{
    return [ports] { return ports; };
}

} // namespace

class TestDiscovery : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // -- chips -------------------------------------------------------------

    void knownChipsAreNamed()
    {
        QCOMPARE(chipFamily(0x0403, 0x6001), QStringLiteral("FTDI FT232R"));
        QCOMPARE(chipFamily(0x1A86, 0x7523), QStringLiteral("WCH CH340"));
    }

    void aKnownVendorStillGivesSomethingToSearchFor()
    {
        QCOMPARE(chipFamily(0x0403, 0x1234), QStringLiteral("FTDI 1234"));
    }

    void unknownAdaptersAreNotGuessed()
    {
        QVERIFY(chipFamily(0xDEAD, 0xBEEF).isEmpty());
        QVERIFY(chipFamily(std::nullopt, std::nullopt).isEmpty());
    }

    // -- ports and identity ------------------------------------------------

    void placeholdersAreCleaned()
    {
        RawPort raw = plain(QStringLiteral("/dev/ttyS0"));
        raw.description = QStringLiteral("n/a");
        raw.serialNumber = QStringLiteral("  None ");
        const PortInfo port = portFromRaw(raw);
        QVERIFY(port.description.isEmpty());
        QVERIFY(port.serialNumber.isEmpty());
    }

    void portLabelAndJson()
    {
        const PortInfo port = portFromRaw(usb(QStringLiteral("/dev/ttyUSB0"), 0x0403, 0x6001, QStringLiteral("A50285BI")));
        QCOMPARE(port.vidPid(), QStringLiteral("0403:6001"));
        QCOMPARE(port.label(), QStringLiteral("/dev/ttyUSB0 — FTDI FT232R (0403:6001) SN A50285BI"));
        const QJsonObject json = port.toJson();
        QCOMPARE(json.value(QStringLiteral("vid")).toInt(), 0x0403);
        QCOMPARE(json.value(QStringLiteral("chip_family")).toString(), QStringLiteral("FTDI FT232R"));
        QVERIFY(json.value(QStringLiteral("driver")).isNull());
    }

    void aSerialNumberGivesAStrongIdentity()
    {
        const PortInfo port = portFromRaw(usb(QStringLiteral("COM3"), 0x0403, 0x6001, QStringLiteral("A50285BI")));
        const DeviceIdentity identity = DeviceIdentity::fromPort(port);
        QVERIFY(!identity.weak);
        QCOMPARE(identity.key(), QStringLiteral("usb_0403_6001_A50285BI"));
        QCOMPARE(identity.describe(), QStringLiteral("USB 0403:6001 serial A50285BI"));
    }

    void withoutASerialNumberTheIdentityIsWeak()
    {
        const PortInfo port = portFromRaw(usb(QStringLiteral("/dev/ttyUSB0"), 0x1A86, 0x7523, {}, QStringLiteral("USB2.0-Serial")));
        const DeviceIdentity identity = DeviceIdentity::fromPort(port);
        QVERIFY(identity.weak);
        QCOMPARE(identity.key(), QStringLiteral("usb_1a86_7523_desc_USB2.0-Serial"));
        QCOMPARE(identity.describe(), QStringLiteral("USB 1a86:7523 (no serial number)"));
    }

    void aNonUsbPortIsKnownByItsName()
    {
        const DeviceIdentity identity = DeviceIdentity::fromPort(portFromRaw(plain(QStringLiteral("/dev/ttyS0"))));
        QCOMPARE(identity.key(), QStringLiteral("port_dev_ttyS0"));
        QCOMPARE(identity.describe(), QStringLiteral("port /dev/ttyS0"));
    }

    void theSameAdapterMatchesUnderANewName()
    {
        const DeviceIdentity identity =
            DeviceIdentity::fromPort(portFromRaw(usb(QStringLiteral("/dev/ttyUSB0"), 0x0403, 0x6001, QStringLiteral("X1"))));
        QVERIFY(identity.matches(portFromRaw(usb(QStringLiteral("/dev/ttyUSB1"), 0x0403, 0x6001, QStringLiteral("X1")))));
        QVERIFY(!identity.matches(portFromRaw(usb(QStringLiteral("/dev/ttyUSB1"), 0x0403, 0x6001, QStringLiteral("X2")))));
    }

    // -- sorting and filtering ---------------------------------------------

    void usbFirstThenNaturalOrder()
    {
        const PosixPlatform platform;
        const QList<PortInfo> ports = listPorts(
            listerOf({plain(QStringLiteral("COM10")), plain(QStringLiteral("COM9")),
                      usb(QStringLiteral("COM12"), 0x0403, 0x6001), plain(QStringLiteral("COM2"))}),
            &platform);
        QStringList devices;
        for (const PortInfo &port : ports)
            devices << port.device;
        QCOMPARE(devices, (QStringList{QStringLiteral("COM12"), QStringLiteral("COM2"),
                                       QStringLiteral("COM9"), QStringLiteral("COM10")}));
    }

    void macosHidesDialInDevices()
    {
        const MacOSPlatform platform;
        const QList<PortInfo> ports = listPorts(
            listerOf({plain(QStringLiteral("/dev/cu.usbserial-A1")), plain(QStringLiteral("/dev/tty.usbserial-A1"))}),
            &platform);
        QCOMPARE(ports.size(), 1);
        QCOMPARE(ports.first().device, QStringLiteral("/dev/cu.usbserial-A1"));
    }

    void windowsUsesThePortName()
    {
        const WindowsPlatform platform;
        QCOMPARE(platform.deviceName(QStringLiteral("COM12"), QStringLiteral("\\\\.\\COM12")), QStringLiteral("COM12"));
        const LinuxPlatform linux;
        QCOMPARE(linux.deviceName(QStringLiteral("ttyUSB0"), QStringLiteral("/dev/ttyUSB0")), QStringLiteral("/dev/ttyUSB0"));
    }

    void linuxHidesPhantomUartsAndAddsDriverAndStablePath()
    {
#if defined(Q_OS_WIN)
        QSKIP("the fake sysfs needs real symlinks");
#endif
        QTemporaryDir root;
        QDir dir(root.path());
        // A fake sysfs: ttyS0 is a real 16550A, ttyS1 a phantom, ttyUSB0 an FTDI.
        dir.mkpath(QStringLiteral("sys/ttyS0"));
        dir.mkpath(QStringLiteral("sys/ttyS1"));
        dir.mkpath(QStringLiteral("devices/usb/ftdi_sio"));
        dir.mkpath(QStringLiteral("devices/usb/port0"));
        dir.mkpath(QStringLiteral("sys/ttyUSB0"));
        QFile::link(dir.filePath(QStringLiteral("devices/usb/port0")), dir.filePath(QStringLiteral("sys/ttyUSB0/device")));
        QFile::link(dir.filePath(QStringLiteral("devices/usb/ftdi_sio")), dir.filePath(QStringLiteral("devices/usb/port0/driver")));
        for (const auto &[name, type] : {std::pair{"ttyS0", "4\n"}, std::pair{"ttyS1", "0\n"}}) {
            QFile file(dir.filePath(QStringLiteral("sys/%1/type").arg(QLatin1String(name))));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(type);
        }
        // A stable path pointing at a device node.
        dir.mkpath(QStringLiteral("by-id"));
        QFile node(dir.filePath(QStringLiteral("ttyUSB0")));
        QVERIFY(node.open(QIODevice::WriteOnly));
        node.close();
        QVERIFY(QFile::link(node.fileName(), dir.filePath(QStringLiteral("by-id/usb-FTDI_FT232R_A50285BI-if00-port0"))));

        const LinuxPlatform platform(dir.filePath(QStringLiteral("by-id")), dir.filePath(QStringLiteral("sys")));
        const QList<PortInfo> ports = listPorts(
            listerOf({plain(QStringLiteral("/dev/ttyS0")), plain(QStringLiteral("/dev/ttyS1")),
                      usb(node.fileName(), 0x0403, 0x6001, QStringLiteral("A50285BI"))}),
            &platform);
        QCOMPARE(ports.size(), 2);
        QCOMPARE(ports.at(0).device, node.fileName());
        QCOMPARE(ports.at(0).driver, QStringLiteral("ftdi_sio"));
        QCOMPARE(ports.at(0).stablePath, dir.filePath(QStringLiteral("by-id/usb-FTDI_FT232R_A50285BI-if00-port0")));
        QCOMPARE(ports.at(1).device, QStringLiteral("/dev/ttyS0"));
    }

    // -- remedies (F6) -----------------------------------------------------

    void linuxPermissionRemedyNamesTheGroupAndTheCommand()
    {
        const LinuxPlatform platform;
        const QString remedy = platform.permissionRemedy(QStringLiteral("/dev/does-not-exist"));
        QVERIFY(remedy.contains(QStringLiteral("sudo usermod -aG dialout $USER")));
    }

    void everyBackendExplainsBusyAndMissingPorts()
    {
        for (const QString &name : {QStringLiteral("linux"), QStringLiteral("macos"), QStringLiteral("windows"),
                                    QStringLiteral("posix")}) {
            const auto platform = platformFor(name);
            QVERIFY(platform);
            QCOMPARE(platform->name(), name);
            QVERIFY(platform->busyRemedy(QStringLiteral("X")).contains(QStringLiteral("X")));
            QVERIFY(platform->notFoundRemedy(QStringLiteral("X")).contains(QStringLiteral("X")));
            QVERIFY(platform->vanishedRemedy(QStringLiteral("X")).contains(QStringLiteral("X")));
        }
        QVERIFY(!platformFor(QStringLiteral("beos")));
    }

    void pathsCanBeOverridden()
    {
        qputenv("PORTSIDE_CONFIG_DIR", "/tmp/portside-cfg");
        qputenv("PORTSIDE_LOG_DIR", "/tmp/portside-log");
        QCOMPARE(Platform::configDir(), QStringLiteral("/tmp/portside-cfg"));
        QCOMPARE(Platform::logDir(), QStringLiteral("/tmp/portside-log"));
        qunsetenv("PORTSIDE_CONFIG_DIR");
        qunsetenv("PORTSIDE_LOG_DIR");
        QVERIFY(Platform::configDir().endsWith(QStringLiteral("Portside")));
    }

    // -- finding and resolving ----------------------------------------------

    void findByNameStablePathOrIdentity()
    {
        const PosixPlatform platform;
        const auto lister = listerOf({usb(QStringLiteral("/dev/ttyUSB3"), 0x10C4, 0xEA60, QStringLiteral("0001"))});
        QVERIFY(findPort(QStringLiteral("/dev/ttyUSB3"), lister, &platform));
        QVERIFY(!findPort(QStringLiteral("/dev/ttyUSB9"), lister, &platform));
        DeviceIdentity identity;
        identity.vid = 0x10C4;
        identity.pid = 0xEA60;
        identity.serialNumber = QStringLiteral("0001");
        const auto found = findPort(identity, lister, &platform);
        QVERIFY(found);
        QCOMPARE(found->device, QStringLiteral("/dev/ttyUSB3"));
    }

    void resolvePassesUrlsAndUnlistedPortsThrough()
    {
        const PosixPlatform platform;
        const auto lister = listerOf({usb(QStringLiteral("/dev/ttyACM0"), 0x2E8A, 0x0005, QStringLiteral("E6614"))});
        const auto [url, noIdentity] = resolvePort(QStringLiteral("loop://"), lister, &platform);
        QCOMPARE(url, QStringLiteral("loop://"));
        QVERIFY(!noIdentity);
        const auto [unlisted, none] = resolvePort(QStringLiteral("/dev/rfcomm0"), lister, &platform);
        QCOMPARE(unlisted, QStringLiteral("/dev/rfcomm0"));
        QVERIFY(!none);
        const auto [listed, identity] = resolvePort(QStringLiteral("/dev/ttyACM0"), lister, &platform);
        QCOMPARE(listed, QStringLiteral("/dev/ttyACM0"));
        QVERIFY(identity);
        QCOMPARE(identity->serialNumber, QStringLiteral("E6614"));
    }

    void theReopenerFollowsTheDeviceToItsNewName()
    {
        const PosixPlatform platform;
        QList<RawPort> present = {usb(QStringLiteral("/dev/ttyUSB0"), 0x0403, 0x6001, QStringLiteral("A1"))};
        const PortLister lister = [&present] { return present; };
        const DeviceIdentity identity = DeviceIdentity::fromPort(portFromRaw(present.first()));
        QString opened;
        const Reopener reopen = identityReopener(identity, {}, lister, &platform,
                                                 [&opened](const QString &device, const SerialSettings &) {
                                                     opened = device;
                                                     return std::make_unique<LoopbackTransport>();
                                                 });
        present = {usb(QStringLiteral("/dev/ttyUSB1"), 0x0403, 0x6001, QStringLiteral("A1"))};
        QVERIFY(reopen());
        QCOMPARE(opened, QStringLiteral("/dev/ttyUSB1"));
        present.clear();
        try {
            reopen();
            QFAIL("a missing device was reopened");
        } catch (const PortsideError &error) {
            QCOMPARE(error.kind(), ErrorKind::PortNotFound);
        }
    }

    void loopUrlsMakeAnEchoingTransport()
    {
        auto transport = createTransport(QStringLiteral("loop://"));
        QCOMPARE(transport->name(), QStringLiteral("loop://"));
        QVERIFY(qobject_cast<LoopbackTransport *>(transport.get()));
        QVERIFY_THROWS_EXCEPTION(PortsideError, createTransport(QStringLiteral("socket://x:1")));
    }
};

QTEST_GUILESS_MAIN(TestDiscovery)
#include "tst_discovery.moc"
