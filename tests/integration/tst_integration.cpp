// End to end over a real (socat) serial pair, against a scripted console.
//
// The closest CI can get to the PRD's exit criterion ("configure a real Cisco
// switch").  The switch is scripted, but everything between it and the user
// is real: a pair of genuine ttys, QSerialPort, the session core, pacing,
// break and logging.  The hardware half of the criterion is in
// docs/HARDWARE-CHECK.md.  Skips cleanly where socat is missing.

#include "core/pacing.h"
#include "core/serial_transport.h"
#include "core/session.h"
#include "core/session_log.h"
#include "support/fake_switch.h"
#include "support/socat_pair.h"
#include "support/support.h"

#include <QProcess>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace portside;
using namespace portside::test;

namespace {

std::unique_ptr<Session> sessionFor(const QString &port, SessionOptions options = {})
{
    auto session = std::make_unique<Session>(std::make_unique<SerialTransport>(port), std::move(options));
    session->open();
    return session;
}

TextPolicy cisco()
{
    TextPolicy policy;
    policy.sendEnding = LineEnding::Cr;
    return policy;
}

PasteResult paste(Session *session, const QString &text, const PacePolicy &pace)
{
    PasteJob job(session, text, pace, cisco());
    QSignalSpy finished(&job, &PasteJob::finished);
    job.start();
    waitFor([&] { return !finished.isEmpty(); }, 30000);
    return job.result();
}

} // namespace

class TestIntegration : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        qRegisterMetaType<PortsideError>();
        qRegisterMetaType<SessionState>();
    }

    void init()
    {
        if (!SocatPair::available())
            QSKIP("socat is not installed (or this is Windows)");
        QVERIFY(m_pair.start());
    }

    void cleanup() { m_pair.stop(); }

    // -- the pair itself -------------------------------------------------------

    void aRealPortOpensAndRoundTrips()
    {
        auto left = sessionFor(m_pair.left());
        auto right = sessionFor(m_pair.right());
        Collector rx(right.get());
        left->send("ping\n");
        QVERIFY(waitFor([&] { return rx.data == "ping\n"; }));
        Collector back(left.get());
        right->send("pong\n");
        QVERIFY(waitFor([&] { return back.data == "pong\n"; }));
    }

    void settingsApplyToARealPort()
    {
        auto session = sessionFor(m_pair.left());
        SerialSettings settings;
        settings.baudrate = 9600;
        settings.parity = Parity::Even;
        settle(session->applySettings(settings)).waitForFinished();
        QCOMPARE(session->settings().baudrate, 9600);
    }

    void modemLinesAndControlSignalsDoNotExplode()
    {
        auto session = sessionFor(m_pair.left());
        settle(session->setDtr(false)).waitForFinished();
        settle(session->setRts(false)).waitForFinished();
        settle(session->modemLines()).waitForFinished();
        QVERIFY(session->isConnected());
    }

    void fullDuplexBulkTransferLosesNothing()
    {
        auto left = sessionFor(m_pair.left());
        auto right = sessionFor(m_pair.right());
        Collector atRight(right.get());
        Collector atLeft(left.get());
        QByteArray one;
        QByteArray two;
        for (int i = 0; i < 200'000; ++i) {
            one += char(i % 251);
            two += char(i % 241);
        }
        left->send(one);
        right->send(two);
        QVERIFY(waitFor([&] { return atRight.data.size() >= one.size() && atLeft.data.size() >= two.size(); }, 60000));
        QCOMPARE(atRight.data, one);
        QCOMPARE(atLeft.data, two);
    }

    void unpluggingMidSessionIsReportedNotCrashed()
    {
        auto session = sessionFor(m_pair.left());
        QSignalSpy errors(session.get(), &Session::errorRaised);
        m_pair.stop();
        QVERIFY(waitFor([&] { return session->state() == SessionState::Disconnected; }, 10000));
        QCOMPARE(errors.first().at(0).value<PortsideError>().kind(), ErrorKind::DeviceVanished);
        session->close();
        QCOMPARE(session->state(), SessionState::Closed);
    }

    void autoReconnectWaitsForTheDeviceToReturn()
    {
        SessionOptions options;
        options.reconnect.enabled = true;
        options.reconnect.initialDelay = 0.05;
        options.reconnect.maxDelay = 0.1;
        const QString port = m_pair.left();
        options.reopen = [port] { return std::make_unique<SerialTransport>(port); };
        auto session = sessionFor(port, options);
        m_pair.stop();
        QVERIFY(waitFor([&] { return session->state() == SessionState::Reconnecting; }, 10000));
        QTest::qWait(300);
        QVERIFY(m_pair.start());
        QVERIFY(waitFor([&] { return session->isConnected(); }, 10000));
        FakeSwitch device(m_pair.right());
        QVERIFY(device.start());
        Collector rx(session.get());
        session->send("\r");
        QVERIFY(waitFor([&] { return rx.data.contains("Switch>"); }));
    }

    void openingAMissingPortGivesATypedErrorWithARemedy()
    {
        SerialTransport transport(QStringLiteral("/dev/portside-no-such-port"));
        try {
            transport.open();
            QFAIL("opened");
        } catch (const PortsideError &error) {
            QCOMPARE(error.kind(), ErrorKind::PortNotFound);
            QVERIFY(error.remedy().contains(QStringLiteral("portside --list")));
        }
    }

    // -- against the scripted switch ----------------------------------------------

    void aSessionTalksToTheScriptedConsole()
    {
        FakeSwitch device(m_pair.right());
        QVERIFY(device.start());
        auto session = sessionFor(m_pair.left());
        Collector rx(session.get());
        session->send("\r");
        QVERIFY(waitFor([&] { return rx.data.contains("Switch>"); }));
    }

    void configuringTheSwitchWithPacedPaste()
    {
        FakeSwitch device(m_pair.right(), 50);
        QVERIFY(device.start());
        auto session = sessionFor(m_pair.left());
        Collector rx(session.get());
        session->send("\r");
        QVERIFY(waitFor([&] { return rx.data.contains("Switch>"); }));
        PacePolicy pace;
        pace.lineDelay = 0;
        pace.waitForPrompt = QStringLiteral("(>|#|Password: )\\s*$");
        const PasteResult result = paste(session.get(),
                                         QStringLiteral("enable\ncisco\nconfigure terminal\nhostname lab-sw1\n"
                                                        "interface GigabitEthernet1/0/1\n description uplink to core\n"
                                                        " switchport mode trunk\nend"),
                                         pace);
        QVERIFY2(result.complete(), qPrintable(result.describe()));
        QVERIFY(waitFor([&] { return device.prompt() == QStringLiteral("lab-sw1#"); }));
        QCOMPARE(device.configLines(),
                 (QStringList{QStringLiteral("hostname lab-sw1"), QStringLiteral("interface GigabitEthernet1/0/1"),
                              QStringLiteral("description uplink to core"), QStringLiteral("switchport mode trunk")}));
    }

    void pacingWaitsForASlowDevice()
    {
        FakeSwitch device(m_pair.right(), 300);
        QVERIFY(device.start());
        auto session = sessionFor(m_pair.left());
        Collector rx(session.get());
        session->send("\r");
        QVERIFY(waitFor([&] { return rx.data.contains("Switch>"); }));
        PacePolicy pace;
        pace.lineDelay = 0;
        pace.waitForPrompt = QStringLiteral("[>#]\\s*$");
        const PasteResult result = paste(session.get(), QStringLiteral("show version\nshow clock\nshow inventory"), pace);
        QVERIFY(result.complete());
        // Two waits of at least the device's 300 ms think time.
        QVERIFY2(result.elapsed >= 0.55, qPrintable(QString::number(result.elapsed)));
        QVERIFY(waitFor([&] { return device.commands().size() == 3; }));
        QCOMPARE(device.commands(),
                 (QStringList{QStringLiteral("show version"), QStringLiteral("show clock"), QStringLiteral("show inventory")}));
    }

    void aBreakReachesTheDevice()
    {
        FakeSwitch device(m_pair.right());
        QVERIFY(device.start());
        auto session = sessionFor(m_pair.left());
        Collector rx(session.get());
        // A pty carries no break condition, so the emulated form is what can
        // reach the far end.
        const BreakResult result = settle(session->sendBreak(0.05, BreakMode::Emulated)).result();
        QVERIFY(result.emulated);
        QVERIFY(waitFor([&] { return device.breaksSeen() >= 1; }));
        QVERIFY(waitFor([&] { return rx.data.contains("break detected"); }));
        QCOMPARE(session->settings().baudrate, 115200);
        QCOMPARE(static_cast<SerialTransport *>(session->transport())->settings().baudrate, 115200);
    }

    void theSessionLogCapturesTheWholeExchange()
    {
        FakeSwitch device(m_pair.right());
        QVERIFY(device.start());
        QTemporaryDir dir;
        auto session = sessionFor(m_pair.left());
        LogSettings settings;
        settings.directory = dir.path();
        settings.timestamps = TimestampMode::Elapsed;
        settings.flushInterval = 0;
        SessionLogger logger(settings, m_pair.left());
        logger.attach(session.get());
        const QString path = logger.start();
        Collector rx(session.get());
        session->send("\r");
        QVERIFY(waitFor([&] { return rx.data.contains("Switch>"); }));
        session->send("show version\r");
        QVERIFY(waitFor([&] { return rx.data.contains("show version"); }));
        session->close();
        logger.detach();
        const QString logged = QString::fromUtf8(readFile(path));
        QVERIFY2(logged.contains(QStringLiteral("Switch>")), qPrintable(logged));
        QVERIFY(logged.contains(QStringLiteral("show version")));
        const QRegularExpression stamp(QStringLiteral("^\\[\\d{2}:\\d{2}:\\d{2}\\.\\d{3}\\] "));
        for (const QString &line : logged.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            QVERIFY2(stamp.match(line).hasMatch(), qPrintable(line));
    }

    void theCommandLineTalksToARealVirtualPort()
    {
        FakeSwitch device(m_pair.right());
        QVERIFY(device.start());
        QProcess cli;
        cli.start(qEnvironmentVariable("PORTSIDE_CLI"),
                  {QStringLiteral("connect"), m_pair.left(), QStringLiteral("--quiet"), QStringLiteral("--drain"),
                   QStringLiteral("1.0")});
        QVERIFY(cli.waitForStarted());
        cli.write("\r");
        cli.closeWriteChannel();
        QVERIFY(cli.waitForFinished(30000));
        QCOMPARE(cli.exitCode(), 0);
        QVERIFY2(cli.readAllStandardOutput().contains("Switch>"), "no prompt on stdout");
    }

private:
    SocatPair m_pair;
};

QTEST_GUILESS_MAIN(TestIntegration)
#include "tst_integration.moc"
