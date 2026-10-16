// The session core: lifecycle, I/O thread, backpressure, reconnect (F3-F5).

#include "core/session.h"
#include "support/support.h"

#include <QSignalSpy>
#include <QTest>
#include <QThread>

using namespace portside;
using namespace portside::test;

class TestSession : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        qRegisterMetaType<SessionState>();
        qRegisterMetaType<PortsideError>();
    }

    // -- lifecycle ---------------------------------------------------------

    void openAndCloseMoveThroughTheExpectedStates()
    {
        // Declared before the session, which still signals as it is destroyed.
        QList<SessionState> states;
        auto made = makeLoopbackSession();
        connect(made.session.get(), &Session::stateChanged, made.session.get(),
                [&](SessionState, SessionState state, const QString &) { states << state; });
        QCOMPARE(made.session->state(), SessionState::Disconnected);
        made.session->open();
        QVERIFY(made.session->isConnected());
        made.session->close();
        QCOMPARE(states, (QList<SessionState>{SessionState::Connecting, SessionState::Connected,
                                               SessionState::Closed}));
    }

    void closeIsIdempotentAndSafeWithoutOpen()
    {
        auto made = makeLoopbackSession();
        made.session->close();
        made.session->close();
        QCOMPARE(made.session->state(), SessionState::Closed);
    }

    void openingTwiceIsAnError()
    {
        auto made = makeLoopbackSession();
        made.session->open();
        QVERIFY_THROWS_EXCEPTION(PortsideError, made.session->open());
    }

    void aClosedSessionCannotBeReopened()
    {
        auto made = makeLoopbackSession();
        made.session->open();
        made.session->close();
        try {
            made.session->open();
            QFAIL("reopened");
        } catch (const PortsideError &error) {
            QCOMPARE(error.kind(), ErrorKind::Session);
        }
    }

    void anOpenFailureIsTypedAndLeavesTheSessionDisconnected()
    {
        LoopbackOptions options;
        options.failOpenWith = PortsideError(ErrorKind::PortBusy, QStringLiteral("busy"), QStringLiteral("x"),
                                             QStringLiteral("close it"));
        auto made = makeLoopbackSession(options);
        try {
            made.session->open();
            QFAIL("opened");
        } catch (const PortsideError &error) {
            QCOMPARE(error.kind(), ErrorKind::PortBusy);
            QCOMPARE(error.remedy(), QStringLiteral("close it"));
        }
        QCOMPARE(made.session->state(), SessionState::Disconnected);
    }

    void settingsAreAppliedOnOpen()
    {
        SessionOptions options;
        SerialSettings settings;
        settings.baudrate = 9600;
        options.settings = settings;
        auto made = makeLoopbackSession({}, options);
        made.session->open();
        QCOMPARE(made.transport->settings().baudrate, 9600);
        QCOMPARE(made.session->statusLine(),
                 QStringLiteral("loopback:// 9600 8N1 none · in 0 B · out 0 B · connected"));
    }

    // -- data --------------------------------------------------------------

    void bytesRoundTripThroughTheLoopback()
    {
        auto made = makeLoopbackSession();
        made.session->open();
        Collector rx(made.session.get());
        QCOMPARE(settle(made.session->send("hello")).result(), 5);
        QVERIFY(waitFor([&] { return rx.data == "hello"; }));
        QCOMPARE(made.session->bytesIn(), 5);
        QVERIFY(waitFor([&] { return made.session->bytesOut() == 5; }));
    }

    void sendingNothingIsANoOp()
    {
        auto made = makeLoopbackSession();
        made.session->open();
        QCOMPARE(made.session->send({}).result(), 0);
    }

    void sendingWhileDisconnectedFails()
    {
        auto made = makeLoopbackSession();
        QVERIFY_THROWS_EXCEPTION(PortsideError, made.session->send("x").result());
    }

    void concurrentWritesKeepTheirOrder()
    {
        LoopbackOptions options;
        options.echo = false;
        auto made = makeLoopbackSession(options);
        made.session->open();
        QByteArray expected;
        QFuture<qint64> last;
        for (int i = 0; i < 200; ++i) {
            const QByteArray chunk = QByteArray::number(i) + ',';
            expected += chunk;
            last = made.session->send(chunk);
        }
        settle(last);
        QCOMPARE(made.transport->written(), expected);
    }

    void aBulkTransferLosesNothing()
    {
        auto made = makeLoopbackSession();
        made.session->open();
        Collector rx(made.session.get());
        QByteArray payload;
        for (int i = 0; i < 1 << 20; ++i)
            payload += char(i * 7);
        made.session->send(payload);
        QVERIFY(waitFor([&] { return rx.data.size() >= payload.size(); }, 20000));
        QCOMPARE(rx.data, payload);
        QCOMPARE(made.session->rxDropped(), 0);
    }

    void trafficIsAnnounced()
    {
        auto made = makeLoopbackSession();
        made.session->open();
        QSignalSpy sent(made.session.get(), &Session::bytesSent);
        QSignalSpy received(made.session.get(), &Session::dataReceived);
        made.session->send("abc");
        QVERIFY(waitFor([&] { return !sent.isEmpty() && !received.isEmpty(); }));
        QCOMPARE(sent.first().first().toLongLong(), 3);
    }

    void aSlowConsumerDropsOldDataInsteadOfGrowingForever()
    {
        LoopbackOptions options;
        options.echo = false;
        SessionOptions sessionOptions;
        sessionOptions.rxLimit = 1000;
        auto made = makeLoopbackSession(options, sessionOptions);
        made.session->open();
        Collector rx(made.session.get());
        // The GUI thread is "busy": no events are processed while the device
        // talks, so the receive buffer must bound itself.
        for (int i = 0; i < 50; ++i) {
            made.transport->feed(QByteArray(100, char('a' + i % 26)));
            QThread::msleep(2);
        }
        QThread::msleep(200);
        QVERIFY(waitFor([&] { return made.session->rxDropped() > 0 && !rx.data.isEmpty(); }));
        QVERIFY(rx.data.size() <= 1100);
        QCOMPARE(rx.data.size() + made.session->rxDropped(), qint64(5000));
        // The newest data survives.
        QVERIFY(rx.data.endsWith(QByteArray(100, char('a' + 49 % 26))));
    }

    // -- threads -----------------------------------------------------------

    void ioHappensOnItsOwnThreadAndNoThreadIsLeaked()
    {
        auto made = makeLoopbackSession();
        made.session->open();
        QThread *thread = made.session->ioThread();
        QVERIFY(thread->isRunning());
        QVERIFY(thread != QThread::currentThread());
        QCOMPARE(made.transport->thread(), thread);
        made.session->close();
        QVERIFY(thread->isFinished());
    }

    // -- failure and reconnect (F5) ------------------------------------------

    void aVanishedDeviceEndsTheSessionWhenReconnectIsOff()
    {
        auto made = makeLoopbackSession();
        made.session->open();
        QSignalSpy errors(made.session.get(), &Session::errorRaised);
        made.transport->setVanishAfter(1);
        made.session->send("x");
        QVERIFY(waitFor([&] { return made.session->state() == SessionState::Disconnected; }));
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().at(0).value<PortsideError>().kind(), ErrorKind::DeviceVanished);
        QVERIFY(errors.first().at(1).toBool());
    }

    void autoReconnectReopensTheDevice()
    {
        LoopbackTransport *fresh = nullptr;
        int attempts = 0;
        SessionOptions options;
        options.reconnect.enabled = true;
        options.reconnect.initialDelay = 0.01;
        options.reopen = [&]() -> std::unique_ptr<Transport> {
            // Missing for the first attempt, then back under a new name.
            if (++attempts == 1)
                throw PortsideError(ErrorKind::PortNotFound, QStringLiteral("not yet"));
            LoopbackOptions renamed;
            renamed.name = QStringLiteral("loopback-2://");
            auto transport = std::make_unique<LoopbackTransport>(renamed);
            fresh = transport.get();
            return transport;
        };
        // Declared before the session, which still signals as it is destroyed.
        QList<SessionState> states;
        auto made = makeLoopbackSession({}, options);
        made.session->open();
        QSignalSpy attemptsSpy(made.session.get(), &Session::reconnectAttempt);
        connect(made.session.get(), &Session::stateChanged, made.session.get(),
                [&](SessionState, SessionState state, const QString &) { states << state; });
        made.transport->setVanishAfter(1);
        made.session->send("x");
        QVERIFY(waitFor([&] { return made.session->isConnected() && fresh != nullptr; }));
        QCOMPARE(states, (QList<SessionState>{SessionState::Reconnecting, SessionState::Connected}));
        QCOMPARE(attemptsSpy.size(), 2);
        QCOMPARE(made.session->port(), QStringLiteral("loopback-2://"));
        QCOMPARE(made.session->transport(), fresh);
        // And the new transport carries traffic.
        Collector rx(made.session.get());
        made.session->send("back");
        QVERIFY(waitFor([&] { return rx.data == "back"; }));
    }

    void reconnectGivesUpAfterMaxAttempts()
    {
        SessionOptions options;
        options.reconnect.enabled = true;
        options.reconnect.initialDelay = 0.01;
        options.reconnect.maxAttempts = 2;
        options.reopen = []() -> std::unique_ptr<Transport> {
            throw PortsideError(ErrorKind::PortNotFound, QStringLiteral("gone"));
        };
        auto made = makeLoopbackSession({}, options);
        made.session->open();
        QSignalSpy errors(made.session.get(), &Session::errorRaised);
        made.transport->setVanishAfter(1);
        made.session->send("x");
        QVERIFY(waitFor([&] { return made.session->state() == SessionState::Disconnected; }));
        const PortsideError last = errors.last().at(0).value<PortsideError>();
        QVERIFY(last.message().contains(QStringLiteral("gave up reconnecting")));
        QVERIFY(errors.last().at(1).toBool());
    }

    void closingDuringAReconnectStopsCleanly()
    {
        SessionOptions options;
        options.reconnect.enabled = true;
        options.reconnect.initialDelay = 10.0;
        options.reopen = [] { return std::make_unique<LoopbackTransport>(); };
        auto made = makeLoopbackSession({}, options);
        made.session->open();
        made.transport->setVanishAfter(1);
        made.session->send("x");
        QVERIFY(waitFor([&] { return made.session->state() == SessionState::Reconnecting; }));
        made.session->close();
        QCOMPARE(made.session->state(), SessionState::Closed);
    }

    void backoffIsExponentialAndCapped()
    {
        const ReconnectPolicy policy;
        QCOMPARE(policy.delayFor(1), 0.5);
        QCOMPARE(policy.delayFor(2), 1.0);
        QCOMPARE(policy.delayFor(3), 2.0);
        QCOMPARE(policy.delayFor(10), 5.0);
    }

    // -- control lines (F4) --------------------------------------------------

    void controlLinesGoThroughTheIoThread()
    {
        auto made = makeLoopbackSession();
        made.session->open();
        QVERIFY(settle(made.session->setDtr(false)).result());
        QVERIFY(settle(made.session->setRts(true)).result());
        QCOMPARE(made.transport->dtr(), std::optional<bool>(false));
        QCOMPARE(made.transport->rts(), std::optional<bool>(true));
        const ModemLines lines = settle(made.session->modemLines()).result();
        QCOMPARE(lines.describe(), QStringLiteral("CTS DSR dcd ri"));
    }

    void controlLinesMissingFromTheDriverAreReportedNotRaised()
    {
        LoopbackOptions options;
        options.supportsControlLines = false;
        options.modemLines = ModemLines{};
        auto made = makeLoopbackSession(options);
        made.session->open();
        QVERIFY(!settle(made.session->setDtr(true)).result());
        QCOMPARE(settle(made.session->modemLines()).result().describe(), QStringLiteral("-- -- -- --"));
    }

    void breakRunsOnTheIoThread()
    {
        auto made = makeLoopbackSession();
        made.session->open();
        const BreakResult result = settle(made.session->sendBreak(0.25)).result();
        QVERIFY(!result.emulated);
        QCOMPARE(made.transport->breaks(), QList<double>{0.25});
    }

    void settingsCanChangeWhileConnected()
    {
        auto made = makeLoopbackSession();
        made.session->open();
        SerialSettings settings;
        settings.baudrate = 921600;
        settle(made.session->applySettings(settings)).waitForFinished();
        QCOMPARE(made.transport->settings().baudrate, 921600);
        QCOMPARE(made.session->settings().baudrate, 921600);
        settings.baudrate = 7;
        QVERIFY_THROWS_EXCEPTION(PortsideError, made.session->applySettings(settings));
    }

    void dataPendingAtCloseStillReachesSubscribers()
    {
        auto made = makeLoopbackSession();
        made.session->open();
        Collector rx(made.session.get());
        made.transport->feed("last words");
        QThread::msleep(50);
        made.session->close();
        QCOMPARE(rx.data, QByteArray("last words"));
    }
};

QTEST_GUILESS_MAIN(TestSession)
#include "tst_session.moc"
