// Serial settings and their validation (F3).

#include "core/errors.h"
#include "core/settings.h"

#include <QTest>

using namespace portside;

class TestSettings : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void defaultsAre115200_8N1_noFlow()
    {
        const SerialSettings settings;
        QCOMPARE(settings.baudrate, 115200);
        QCOMPARE(settings.dataBits, 8);
        QCOMPARE(settings.parity, Parity::None);
        QCOMPARE(settings.stopBits, StopBits::One);
        QCOMPARE(settings.flowControl, FlowControl::None);
        QCOMPARE(settings.describe(), QStringLiteral("115200 8N1 none"));
    }

    void describeSpellsOneAndAHalf()
    {
        SerialSettings settings;
        settings.baudrate = 9600;
        settings.dataBits = 7;
        settings.parity = Parity::Even;
        settings.stopBits = StopBits::OnePointFive;
        settings.flowControl = FlowControl::RtsCts;
        QCOMPARE(settings.describe(), QStringLiteral("9600 7E1.5 rtscts"));
    }

    void theWholeBaudRangeIsAccepted_data()
    {
        QTest::addColumn<int>("baud");
        QTest::newRow("floor") << 50;
        QTest::newRow("console") << 9600;
        QTest::newRow("custom") << 250000;
        QTest::newRow("ceiling") << 4'000'000;
    }

    void theWholeBaudRangeIsAccepted()
    {
        QFETCH(int, baud);
        SerialSettings settings;
        settings.baudrate = baud;
        settings.validate();
    }

    void outOfRangeValuesAreTypedErrors_data()
    {
        QTest::addColumn<int>("baud");
        QTest::addColumn<int>("bits");
        QTest::newRow("too slow") << 49 << 8;
        QTest::newRow("too fast") << 4'000'001 << 8;
        QTest::newRow("four data bits") << 9600 << 4;
        QTest::newRow("nine data bits") << 9600 << 9;
    }

    void outOfRangeValuesAreTypedErrors()
    {
        QFETCH(int, baud);
        QFETCH(int, bits);
        SerialSettings settings;
        settings.baudrate = baud;
        settings.dataBits = bits;
        try {
            settings.validate();
            QFAIL("validate() accepted an invalid setting");
        } catch (const PortsideError &error) {
            QCOMPARE(error.kind(), ErrorKind::InvalidSettings);
        }
    }

    void mapRoundTrip()
    {
        SerialSettings settings;
        settings.baudrate = 57600;
        settings.parity = Parity::Mark;
        settings.stopBits = StopBits::Two;
        settings.flowControl = FlowControl::XonXoff;
        QCOMPARE(SerialSettings::fromMap(settings.toMap()), settings);
    }

    void missingKeysTakeDefaults()
    {
        const SerialSettings settings = SerialSettings::fromMap({{QStringLiteral("baudrate"), 9600}});
        QCOMPARE(settings.baudrate, 9600);
        QCOMPARE(settings.dataBits, 8);
    }

    void unknownValuesAreRejected_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QVariant>("value");
        QTest::newRow("parity") << QStringLiteral("parity") << QVariant(QStringLiteral("X"));
        QTest::newRow("stop bits") << QStringLiteral("stop_bits") << QVariant(3.0);
        QTest::newRow("flow") << QStringLiteral("flow_control") << QVariant(QStringLiteral("magic"));
        QTest::newRow("baud as text") << QStringLiteral("baudrate") << QVariant(QStringLiteral("fast"));
        QTest::newRow("baud as bool") << QStringLiteral("baudrate") << QVariant(true);
    }

    void unknownValuesAreRejected()
    {
        QFETCH(QString, key);
        QFETCH(QVariant, value);
        QVERIFY_THROWS_EXCEPTION(PortsideError, SerialSettings::fromMap({{key, value}}));
    }
};

QTEST_GUILESS_MAIN(TestSettings)
#include "tst_settings.moc"
