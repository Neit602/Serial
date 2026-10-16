// Paced paste (F13).

#include "core/pacing.h"
#include "support/support.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

using namespace portside;
using namespace portside::test;

namespace {

// A quiet device: nothing is echoed back.
LoopbackSession quietSession()
{
    LoopbackOptions options;
    options.echo = false;
    auto made = makeLoopbackSession(options);
    made.session->open();
    return made;
}

PasteResult runPaste(PasteJob &job, int timeoutMs = 10000)
{
    QSignalSpy finished(&job, &PasteJob::finished);
    job.start();
    waitFor([&] { return !finished.isEmpty(); }, timeoutMs);
    return job.result();
}

PacePolicy fast()
{
    PacePolicy pace;
    pace.lineDelay = 0;
    return pace;
}

} // namespace

class TestPacing : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void linesSplitTheWayAPasteSeesThem()
    {
        QCOMPARE(splitPasteLines(QStringLiteral("a\nb")), (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
        QCOMPARE(splitPasteLines(QStringLiteral("a\r\nb\r\n")), (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
        QCOMPARE(splitPasteLines(QStringLiteral("a\rb")), (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
        QCOMPARE(splitPasteLines(QStringLiteral("a\n\nb")).size(), 3);
        QCOMPARE(splitPasteLines(QStringLiteral("\n")), QStringList{QString()});
        QVERIFY(splitPasteLines(QString()).isEmpty());
    }

    void eachLineGetsTheConfiguredEnding()
    {
        auto made = quietSession();
        TextPolicy text;
        text.sendEnding = LineEnding::Cr;
        PasteJob job(made.session.get(), QStringLiteral("hostname core1\r\ninterface Gi1/0/1\n"), fast(), text);
        QSignalSpy progress(&job, &PasteJob::progress);
        const PasteResult result = runPaste(job);
        QVERIFY(result.complete());
        QCOMPARE(result.linesSent, 2);
        QCOMPARE(result.bytesSent, 33);
        QCOMPARE(progress.size(), 2);
        QVERIFY(waitFor([&] { return made.transport->written() == "hostname core1\rinterface Gi1/0/1\r"; }));
        QCOMPARE(result.describe(), QStringLiteral("sent 2/2 lines, 33 B"));
    }

    void anEmptyPasteFinishesAtOnce()
    {
        auto made = quietSession();
        PasteJob job(made.session.get(), QString(), fast());
        const PasteResult result = runPaste(job);
        QVERIFY(result.complete());
        QCOMPARE(result.totalLines, 0);
    }

    void aCharacterDelaySendsByteByByte()
    {
        auto made = quietSession();
        PacePolicy pace = fast();
        pace.charDelay = 0.005;
        QSignalSpy sent(made.session.get(), &Session::bytesSent);
        PasteJob job(made.session.get(), QStringLiteral("abc"), pace);
        QElapsedTimer clock;
        clock.start();
        const PasteResult result = runPaste(job);
        QVERIFY(result.complete());
        QVERIFY(clock.elapsed() >= 15);
        QVERIFY(waitFor([&] { return sent.size() == 4; }));
        QCOMPARE(made.transport->written(), QByteArray("abc\r"));
    }

    void aLineDelayIsWaitedBetweenLinesButNotAfterTheLast()
    {
        auto made = quietSession();
        PacePolicy pace;
        pace.lineDelay = 0.1;
        PasteJob job(made.session.get(), QStringLiteral("1\n2\n3"), pace);
        QElapsedTimer clock;
        clock.start();
        const PasteResult result = runPaste(job);
        QVERIFY(result.complete());
        const qint64 elapsed = clock.elapsed();
        QVERIFY2(elapsed >= 190 && elapsed < 900, qPrintable(QString::number(elapsed)));
    }

    void waitingForThePromptBeforeEachNextLine()
    {
        auto made = quietSession();
        // A "device" that prompts 50 ms after every line it receives.  Its
        // timers hang off `device`, so none can fire after this test returns.
        int lines = 0;
        QObject device;
        QTimer poll(&device);
        connect(&poll, &QTimer::timeout, &device, [&] {
            const int seen = int(made.transport->written().count('\r'));
            if (seen > lines) {
                lines = seen;
                QTimer::singleShot(50, &device, [&] { made.transport->feed("\r\nSwitch(config)# "); });
            }
        });
        poll.start(5);
        PacePolicy pace = fast();
        pace.waitForPrompt = QStringLiteral("[>#]\\s*$");
        PasteJob job(made.session.get(), QStringLiteral("one\ntwo\nthree"), pace);
        QSignalSpy progress(&job, &PasteJob::progress);
        const PasteResult result = runPaste(job);
        QVERIFY(result.complete());
        QVERIFY(result.timedOutLines.isEmpty());
        QVERIFY(progress.at(0).at(0).value<PasteProgress>().waitedForPrompt);
        QVERIFY(!progress.last().at(0).value<PasteProgress>().waitedForPrompt); // no wait after the last line
    }

    void aPromptThatNeverComesAbortsThePaste()
    {
        auto made = quietSession();
        PacePolicy pace = fast();
        pace.waitForPrompt = QStringLiteral("#$");
        pace.promptTimeout = 0.1;
        PasteJob job(made.session.get(), QStringLiteral("one\ntwo\nthree"), pace);
        const PasteResult result = runPaste(job);
        QVERIFY(result.aborted);
        QCOMPARE(result.linesSent, 1);
        QCOMPARE(result.timedOutLines, QList<int>{1});
        QCOMPARE(result.describe(), QStringLiteral("aborted 1/3 lines, 4 B, 1 prompt timeout(s)"));
    }

    void orPressesOnWhenAskedTo()
    {
        auto made = quietSession();
        PacePolicy pace = fast();
        pace.waitForPrompt = QStringLiteral("#$");
        pace.promptTimeout = 0.05;
        pace.abortOnTimeout = false;
        PasteJob job(made.session.get(), QStringLiteral("one\ntwo\nthree"), pace);
        const PasteResult result = runPaste(job);
        QVERIFY(!result.aborted);
        QCOMPARE(result.linesSent, 3);
        QCOMPARE(result.timedOutLines, (QList<int>{1, 2}));
    }

    void cancellingStopsBetweenLinesAndLeavesTheSessionHealthy()
    {
        auto made = quietSession();
        PacePolicy pace;
        pace.lineDelay = 0.2;
        PasteJob job(made.session.get(), QStringLiteral("1\n2\n3\n4"), pace);
        QSignalSpy finished(&job, &PasteJob::finished);
        job.start();
        QVERIFY(waitFor([&] { return job.result().linesSent >= 1; }));
        job.cancel();
        QCOMPARE(finished.size(), 1);
        QVERIFY(job.result().aborted);
        QVERIFY(job.result().linesSent < 4);
        QVERIFY(made.session->isConnected());
        QCOMPARE(settle(made.session->send("ok")).result(), 2);
    }

    void aSessionThatEndsMidPasteIsReported()
    {
        auto made = quietSession();
        PacePolicy pace;
        pace.lineDelay = 0.05;
        PasteJob job(made.session.get(), QStringLiteral("1\n2\n3"), pace);
        QSignalSpy finished(&job, &PasteJob::finished);
        job.start();
        QVERIFY(waitFor([&] { return job.result().linesSent >= 1; }));
        made.session->close();
        QVERIFY(waitFor([&] { return !finished.isEmpty(); }));
        QVERIFY(job.result().aborted);
        QVERIFY(job.result().error.has_value());
    }

    void invalidPoliciesAreRefused()
    {
        PacePolicy pace;
        pace.lineDelay = -1;
        QVERIFY_THROWS_EXCEPTION(PortsideError, pace.validate());
        pace = {};
        pace.promptTimeout = 0;
        QVERIFY_THROWS_EXCEPTION(PortsideError, pace.validate());
        pace = {};
        pace.waitForPrompt = QStringLiteral("([");
        QVERIFY_THROWS_EXCEPTION(PortsideError, pace.validate());
    }
};

QTEST_GUILESS_MAIN(TestPacing)
#include "tst_pacing.moc"
