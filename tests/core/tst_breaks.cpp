// Break, native and emulated (F12).

#include "core/breaks.h"
#include "core/errors.h"
#include "core/loopback_transport.h"

#include <QTest>

using namespace portside;

namespace {

void noSleep(double) {}

std::unique_ptr<LoopbackTransport> openTransport(bool supportsBreak, int baud = 9600)
{
    LoopbackOptions options;
    options.echo = false;
    options.supportsBreak = supportsBreak;
    SerialSettings settings;
    settings.baudrate = baud;
    auto transport = std::make_unique<LoopbackTransport>(options, settings);
    transport->open();
    return transport;
}

} // namespace

class TestBreaks : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void baudForBreakIsTheFastestThatStillCovers()
    {
        QCOMPARE(baudForBreak(0.001), 9000);
        QCOMPARE(baudForBreak(0.25), kBaudMin); // 36 baud would be ideal; 50 is the floor
        QCOMPARE(baudForBreak(0), kBaudMin);
    }

    void zeroBytesRepeatWhenFiftyBaudIsNotSlowEnough()
    {
        // One zero byte at 50 baud lasts 180 ms (DECISIONS D18).
        QCOMPARE(zeroBytesForBreak(0.18, 50), 1);
        QCOMPARE(zeroBytesForBreak(0.25, 50), 2);
        QCOMPARE(zeroBytesForBreak(1.0, 50), 6);
    }

    void aNativeBreakIsUsedWhenTheDriverHasOne()
    {
        auto transport = openTransport(true);
        const BreakResult result = sendBreak(*transport, 0.25, BreakMode::Auto, noSleep);
        QVERIFY(!result.emulated);
        QCOMPARE(transport->breaks(), QList<double>{0.25});
        QCOMPARE(result.describe(), QStringLiteral("native break, 250 ms"));
    }

    void autoFallsBackToTheEmulatedBreakAndRestoresTheBaudRate()
    {
        auto transport = openTransport(false, 115200);
        double slept = 0;
        const BreakResult result = sendBreak(*transport, 0.25, BreakMode::Auto, [&](double s) { slept = s; });
        QVERIFY(result.emulated);
        QCOMPARE(result.baudrate, 50);
        QCOMPARE(result.bytesSent, 2);
        QCOMPARE(transport->written(), QByteArray(2, '\0'));
        const QList<SerialSettings> applied = transport->appliedSettings();
        QCOMPARE(applied.size(), 2);
        QCOMPARE(applied.at(0).baudrate, 50);
        QCOMPARE(applied.at(1).baudrate, 115200);
        QCOMPARE(transport->settings().baudrate, 115200);
        QVERIFY(qAbs(slept - 2 * 9.0 / 50) < 1e-9);
        QCOMPARE(result.describe(), QStringLiteral("emulated break, 250 ms at 50 baud (2 zero bytes)"));
    }

    void emulatedCanBeForced()
    {
        auto transport = openTransport(true);
        const BreakResult result = sendBreak(*transport, 0.1, BreakMode::Emulated, noSleep);
        QVERIFY(result.emulated);
        QVERIFY(transport->breaks().isEmpty());
        QCOMPARE(result.baudrate, 90);
        QCOMPARE(result.bytesSent, 1);
    }

    void nativeOnlyExplainsAMissingBreak()
    {
        auto transport = openTransport(false);
        try {
            sendBreak(*transport, 0.25, BreakMode::Native, noSleep);
            QFAIL("no error");
        } catch (const PortsideError &error) {
            QCOMPARE(error.kind(), ErrorKind::NotSupported);
            QVERIFY(error.remedy().contains(QStringLiteral("emulated")));
        }
    }

    void theSettingsComeBackEvenWhenTheWriteFails()
    {
        auto transport = openTransport(false, 19200);
        transport->setVanishAfter(1);
        QVERIFY_THROWS_EXCEPTION(PortsideError, sendBreak(*transport, 0.25, BreakMode::Emulated, noSleep));
        QCOMPARE(transport->settings().baudrate, 19200);
    }

    void aNonPositiveDurationIsRefused()
    {
        auto transport = openTransport(true);
        QVERIFY_THROWS_EXCEPTION(PortsideError, sendBreak(*transport, 0, BreakMode::Auto, noSleep));
    }
};

QTEST_GUILESS_MAIN(TestBreaks)
#include "tst_breaks.moc"
