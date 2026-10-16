// Per-line timestamps (F16).

#include "core/timestamps.h"

#include <QRegularExpression>
#include <QTest>

using namespace portside;

class TestTimestamps : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void intervalsAreHoursMinutesSecondsMillis()
    {
        QCOMPARE(formatInterval(0), QStringLiteral("00:00:00.000"));
        QCOMPARE(formatInterval(3723.25), QStringLiteral("01:02:03.250"));
        QCOMPARE(formatInterval(59.9996), QStringLiteral("00:01:00.000"));
        QCOMPARE(formatInterval(-1), QStringLiteral("00:00:00.000"));
    }

    void absoluteStampsCarryTheUtcOffset()
    {
        const QRegularExpression shape(QStringLiteral("^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d\\.\\d{3}[+-]\\d\\d:\\d\\d$"));
        QVERIFY(shape.match(formatAbsolute(1'700'000'000.123)).hasMatch());
    }

    void offPrefixesNothing()
    {
        Timestamper stamper;
        QCOMPARE(stamper.prefix(5), QString());
        QCOMPARE(stamper.formatLine(QStringLiteral("x"), 5), QStringLiteral("x"));
    }

    void elapsedCountsFromTheStartAndCanBeReset()
    {
        Timestamper stamper(TimestampMode::Elapsed, 100.0);
        QCOMPARE(stamper.prefix(101.5), QStringLiteral("[00:00:01.500] "));
        stamper.reset(200.0);
        QCOMPARE(stamper.prefix(200.25), QStringLiteral("[00:00:00.250] "));
    }

    void deltaMeasuresTheGapBetweenLines()
    {
        Timestamper stamper(TimestampMode::Delta);
        QCOMPARE(stamper.prefix(10.0), QStringLiteral("[+00:00:00.000] "));
        QCOMPARE(stamper.prefix(10.75), QStringLiteral("[+00:00:00.750] "));
        QCOMPARE(stamper.prefix(12.0), QStringLiteral("[+00:00:01.250] "));
    }

    void modesHaveTheirProfileNames()
    {
        QCOMPARE(enumNameList<TimestampMode>(),
                 (QStringList{QStringLiteral("off"), QStringLiteral("absolute"), QStringLiteral("elapsed"),
                              QStringLiteral("delta")}));
    }
};

QTEST_GUILESS_MAIN(TestTimestamps)
#include "tst_timestamps.moc"
