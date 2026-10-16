// Session logging (F15, F16).

#include "core/session_log.h"
#include "support/support.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

using namespace portside;
using namespace portside::test;

namespace {

// A clock the test moves by hand.
struct FakeClock
{
    double now = 1'000'000.0;
    Clock clock()
    {
        return [this] { return now; };
    }
};

LogSettings settingsIn(const QTemporaryDir &dir, LogFormat format = LogFormat::Text)
{
    LogSettings settings;
    settings.directory = dir.path();
    settings.filenameTemplate = QStringLiteral("session.log");
    settings.format = format;
    settings.flushInterval = 0; // every write reaches the disk, for the asserts
    return settings;
}

} // namespace

class TestSessionLog : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // -- templates ---------------------------------------------------------

    void templatesExpandAndStaySafeOnEveryOs()
    {
        const QDateTime when(QDate(2026, 10, 16), QTime(14, 3, 7));
        QCOMPARE(resolveTemplate(kDefaultLogTemplate, QStringLiteral("/dev/ttyUSB0"), {}, when),
                 QStringLiteral("portside-_dev_ttyUSB0-20261016-140307.log"));
        QCOMPARE(resolveTemplate(QStringLiteral("{profile}-{device}-{n}.txt"), QStringLiteral("COM3:"),
                                 QStringLiteral("lab/core"), when, 4),
                 QStringLiteral("lab_core-COM3_-4.txt"));
        QCOMPARE(resolveTemplate(QStringLiteral("{profile}.log"), QStringLiteral("x"), {}, when),
                 QStringLiteral("default.log"));
        QCOMPARE(resolveTemplate(QStringLiteral("{{literal}}-{port}"), QStringLiteral("x"), {}, when),
                 QStringLiteral("{literal}-x"));
    }

    void anUnknownPlaceholderFallsBackToTheDefault()
    {
        const QDateTime when(QDate(2026, 1, 2), QTime(3, 4, 5));
        QCOMPARE(resolveTemplate(QStringLiteral("{nope}.log"), QStringLiteral("x"), {}, when),
                 QStringLiteral("portside-x-20260102-030405.log"));
        QCOMPARE(resolveTemplate(QStringLiteral("unclosed {port"), QStringLiteral("x"), {}, when),
                 QStringLiteral("portside-x-20260102-030405.log"));
    }

    void sanitisingNeverLeavesAnEmptyName()
    {
        QCOMPARE(sanitiseFileComponent(QStringLiteral(" .. ")), QStringLiteral("unnamed"));
        QCOMPARE(sanitiseFileComponent(QStringLiteral("a<b>c:d")), QStringLiteral("a_b_c_d"));
    }

    // -- text logs -----------------------------------------------------------

    void textLogsUseLfOnEveryPlatform()
    {
        QTemporaryDir dir;
        SessionLogger logger(settingsIn(dir));
        const QString path = logger.start();
        logger.write("one\r\ntwo\rthree\nfour\r");
        logger.write("\nfive");
        logger.stop();
        QCOMPARE(readFile(path), QByteArray("one\ntwo\nthree\nfour\nfive\n"));
    }

    void linesAreStampedWhenTheirFirstByteArrives()
    {
        QTemporaryDir dir;
        FakeClock clock;
        LogSettings settings = settingsIn(dir);
        settings.timestamps = TimestampMode::Elapsed;
        SessionLogger logger(settings, {}, {}, clock.clock());
        const QString path = logger.start();
        clock.now += 1.0;
        logger.write("first ha");
        clock.now += 5.0;
        logger.write("lf\r\nsecond\r\n");
        logger.stop();
        QCOMPARE(readFile(path), QByteArray("[00:00:01.000] first half\n[00:00:06.000] second\n"));
    }

    void notesAreMarkedAndRawLogsIgnoreThem()
    {
        QTemporaryDir dir;
        SessionLogger text(settingsIn(dir));
        const QString textPath = text.start();
        text.write("partial");
        text.writeNote(QStringLiteral("break sent"));
        text.stop();
        QCOMPARE(readFile(textPath), QByteArray("partial\n[portside] break sent\n"));

        LogSettings rawSettings = settingsIn(dir, LogFormat::Raw);
        rawSettings.filenameTemplate = QStringLiteral("raw.bin");
        SessionLogger raw(rawSettings);
        const QString rawPath = raw.start();
        const QByteArray wire("\x00\x01\r\n\xff\x7f", 6);
        raw.write(wire);
        raw.writeNote(QStringLiteral("ignored"));
        raw.stop();
        QCOMPARE(readFile(rawPath), wire);
    }

    void restartingAppendsRatherThanTruncating()
    {
        QTemporaryDir dir;
        SessionLogger logger(settingsIn(dir));
        const QString path = logger.start();
        logger.write("before\n");
        logger.stop();
        logger.start();
        logger.write("after\n");
        logger.stop();
        QCOMPARE(readFile(path), QByteArray("before\nafter\n"));
    }

    void writesAreBufferedUntilTheFlushInterval()
    {
        QTemporaryDir dir;
        FakeClock clock;
        LogSettings settings = settingsIn(dir);
        settings.flushInterval = 1.0;
        SessionLogger logger(settings, {}, {}, clock.clock());
        const QString path = logger.start();
        logger.write("buffered\n");
        QVERIFY(readFile(path).isEmpty());
        QCOMPARE(logger.bytesWritten(), 9);
        clock.now += 2;
        logger.write("x\n");
        QCOMPARE(readFile(path), QByteArray("buffered\nx\n"));
    }

    void anUnwritableLogIsATypedError()
    {
        // A directory "inside" a regular file can be created on no OS.
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("blocker")), "x");
        LogSettings settings;
        settings.directory = dir.filePath(QStringLiteral("blocker/logs"));
        SessionLogger logger(settings);
        QVERIFY_THROWS_EXCEPTION(PortsideError, logger.start());
        QVERIFY(!logger.isLogging());
    }

    // -- rotation ------------------------------------------------------------

    void rotationKeepsTheNewestFiles()
    {
        QTemporaryDir dir;
        LogSettings settings = settingsIn(dir, LogFormat::Raw);
        settings.maxBytes = 10;
        settings.keep = 2;
        SessionLogger logger(settings);
        const QString path = logger.start();
        for (int i = 0; i < 5; ++i)
            logger.write(QByteArray(10, char('a' + i)));
        logger.stop();
        QCOMPARE(logger.rotations(), 5);
        const QStringList files = QDir(dir.path()).entryList(QDir::Files, QDir::Name);
        QCOMPARE(files, (QStringList{QStringLiteral("session.4.log"), QStringLiteral("session.5.log"),
                                     QStringLiteral("session.log")}));
        QCOMPARE(readFile(dir.filePath(QStringLiteral("session.5.log"))), QByteArray(10, 'e'));
        QCOMPARE(readFile(path), QByteArray());
    }

    void rotationNeverOverwritesAnEarlierSessionsFiles()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("session.7.log")), "old");
        LogSettings settings = settingsIn(dir, LogFormat::Raw);
        settings.maxBytes = 4;
        settings.keep = 10;
        SessionLogger logger(settings);
        logger.start();
        logger.write("new!");
        logger.stop();
        QCOMPARE(readFile(dir.filePath(QStringLiteral("session.7.log"))), QByteArray("old"));
        QCOMPARE(readFile(dir.filePath(QStringLiteral("session.8.log"))), QByteArray("new!"));
    }

    // -- following a session ----------------------------------------------------

    void theLoggerFollowsASession()
    {
        QTemporaryDir dir;
        LogSettings settings = settingsIn(dir);
        settings.autoStart = true;
        SessionLogger logger(settings, QStringLiteral("loopback://"));
        auto made = makeLoopbackSession();
        logger.attach(made.session.get());
        made.session->open();
        QVERIFY(logger.isLogging());
        made.session->send("show clock\r\n");
        QVERIFY(waitFor([&] { return made.session->bytesIn() == 12; }));
        made.session->close();
        QVERIFY(!logger.isLogging());
        QCOMPARE(readFile(dir.filePath(QStringLiteral("session.log"))), QByteArray("show clock\n"));
    }

    void errorsBecomeNotes()
    {
        QTemporaryDir dir;
        SessionLogger logger(settingsIn(dir));
        auto made = makeLoopbackSession();
        logger.attach(made.session.get());
        made.session->open();
        logger.start();
        made.transport->setVanishAfter(1);
        made.session->send("x");
        QVERIFY(waitFor([&] { return !logger.isLogging(); }));
        const QByteArray log = readFile(dir.filePath(QStringLiteral("session.log")));
        QVERIFY2(log.contains("[portside] error: loopback://: loopback:// disappeared"), log.constData());
    }
};

QTEST_GUILESS_MAIN(TestSessionLog)
#include "tst_session_log.moc"
