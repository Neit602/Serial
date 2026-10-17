// The `portside` command line (F23): arguments, profiles, formatting, exit
// codes, the pass-through pump, and the real binary end to end.

#include "cli/cli.h"
#include "cli/pass_through.h"
#include "core/session.h"
#include "support/support.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace portside;
using namespace portside::cli;
using namespace portside::test;

namespace {

ConnectOptions parse(const QStringList &arguments)
{
    QString error;
    const auto options = parseConnectArguments(arguments, &error);
    if (!options)
        qFatal("parse failed: %s", qPrintable(error));
    return *options;
}

QString parseError(const QStringList &arguments)
{
    QString error;
    const auto options = parseConnectArguments(arguments, &error);
    return options ? QString() : error;
}

struct Run
{
    int code = -1;
    QByteArray out;
    QByteArray err;
};

Run runCli(const QStringList &arguments, const QByteArray &input = {}, const QProcessEnvironment &env = QProcessEnvironment::systemEnvironment())
{
    QProcess process;
    process.setProcessEnvironment(env);
    process.start(qEnvironmentVariable("PORTSIDE_CLI"), arguments);
    process.waitForStarted();
    if (!input.isEmpty())
        process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(30000))
        process.kill();
    return {process.exitCode(), process.readAllStandardOutput(), process.readAllStandardError()};
}

PortInfo port(const QString &device, const QString &description = {})
{
    PortInfo info;
    info.device = device;
    info.description = description;
    return info;
}

} // namespace

class TestCli : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY2(!qEnvironmentVariable("PORTSIDE_CLI").isEmpty(), "run through ctest, which sets PORTSIDE_CLI");
    }

    // -- arguments -----------------------------------------------------------

    void defaultsAre115200_8N1()
    {
        const ConnectOptions options = parse({QStringLiteral("/dev/ttyUSB0")});
        QCOMPARE(settingsFor(options, std::nullopt).describe(), QStringLiteral("115200 8N1 none"));
        QCOMPARE(options.drain, 0.25);
        QVERIFY(!options.baud);
    }

    void everyLineSettingCanBeGiven()
    {
        const ConnectOptions options =
            parse({QStringLiteral("COM3"), QStringLiteral("--baud"), QStringLiteral("9600"), QStringLiteral("--parity"),
                   QStringLiteral("e"), QStringLiteral("--data-bits"), QStringLiteral("7"),
                   QStringLiteral("--stop-bits=2"), QStringLiteral("--flow"), QStringLiteral("rtscts")});
        QCOMPARE(settingsFor(options, std::nullopt).describe(), QStringLiteral("9600 7E2 rtscts"));
    }

    void oneAndAHalfStopBitsIsAvailable()
    {
        const ConnectOptions options = parse({QStringLiteral("x"), QStringLiteral("--stop-bits"), QStringLiteral("1.5")});
        QCOMPARE(*options.stopBits, StopBits::OnePointFive);
    }

    void baudAcceptsShortAndHexForms()
    {
        QCOMPARE(parseBaud(QStringLiteral("115_200")), std::optional(115200));
        QCOMPARE(parseBaud(QStringLiteral("0x1C200")), std::optional(115200));
        QCOMPARE(*parse({QStringLiteral("x"), QStringLiteral("-b"), QStringLiteral("57600")}).baud, 57600);
        QVERIFY(!parseBaud(QStringLiteral("fast")));
    }

    void invalidArgumentsAreUsageErrors_data()
    {
        QTest::addColumn<QStringList>("arguments");
        QTest::addColumn<QString>("message");
        QTest::newRow("no port") << QStringList{} << QStringLiteral("the following arguments are required: port");
        QTest::newRow("slow baud") << QStringList{QStringLiteral("x"), QStringLiteral("--baud"), QStringLiteral("12")}
                                   << QStringLiteral("argument --baud: baud rate must be between 50 and 4000000, got 12");
        QTest::newRow("word baud") << QStringList{QStringLiteral("x"), QStringLiteral("--baud"), QStringLiteral("fast")}
                                   << QStringLiteral("argument --baud: 'fast' is not an integer baud rate");
        QTest::newRow("parity") << QStringList{QStringLiteral("x"), QStringLiteral("--parity"), QStringLiteral("Q")}
                                << QStringLiteral("argument --parity: invalid choice 'Q' (choose from N, E, O, M, S)");
        QTest::newRow("data bits") << QStringList{QStringLiteral("x"), QStringLiteral("--data-bits"), QStringLiteral("9")}
                                   << QStringLiteral("argument --data-bits: invalid choice '9' (choose from 5, 6, 7, 8)");
        QTest::newRow("break") << QStringList{QStringLiteral("x"), QStringLiteral("--break-duration"), QStringLiteral("0")}
                               << QStringLiteral("argument --break-duration: '0' is not a valid number of seconds");
        QTest::newRow("unknown") << QStringList{QStringLiteral("x"), QStringLiteral("--frobnicate")}
                                 << QStringLiteral("Unknown option 'frobnicate'.");
        QTest::newRow("two ports") << QStringList{QStringLiteral("a"), QStringLiteral("b")}
                                   << QStringLiteral("unrecognized arguments: b");
    }

    void invalidArgumentsAreUsageErrors()
    {
        QFETCH(QStringList, arguments);
        QFETCH(QString, message);
        QCOMPARE(parseError(arguments), message);
    }

    // -- profiles and explicit options (D26) ---------------------------------------

    void aProfileSuppliesTheSettingsAndExplicitFlagsWin()
    {
        Profile profile;
        profile.name = QStringLiteral("lab");
        profile.serial.baudrate = 9600;
        profile.serial.parity = Parity::Even;
        profile.text.sendEnding = LineEnding::CrLf;
        profile.text.backspace = Backspace::Bs;
        const ConnectOptions fromProfile = parse({QStringLiteral("x")});
        QCOMPARE(settingsFor(fromProfile, profile).describe(), QStringLiteral("9600 8E1 none"));
        const ConnectOptions overridden = parse({QStringLiteral("x"), QStringLiteral("--baud"), QStringLiteral("115200")});
        QCOMPARE(settingsFor(overridden, profile).describe(), QStringLiteral("115200 8E1 none"));
        QCOMPARE(textPolicyFor(fromProfile, profile).sendEnding, LineEnding::CrLf);
        const ConnectOptions discipline = parse({QStringLiteral("x"), QStringLiteral("--send-ending"),
                                                 QStringLiteral("lf"), QStringLiteral("--backspace"), QStringLiteral("del"),
                                                 QStringLiteral("--local-echo")});
        const TextPolicy policy = textPolicyFor(discipline, profile);
        QCOMPARE(policy.sendEnding, LineEnding::Lf);
        QCOMPARE(policy.backspace, Backspace::Del);
        QVERIFY(policy.localEcho);
    }

    void loggingIsAskedForByFlagOrProfile()
    {
        QVERIFY(!logSettingsFor(parse({QStringLiteral("x")}), std::nullopt));
        QTemporaryDir dir;
        const auto settings = logSettingsFor(parse({QStringLiteral("x"), QStringLiteral("--log"),
                                                    dir.filePath(QStringLiteral("capture.log")), QStringLiteral("--log-format"),
                                                    QStringLiteral("raw"), QStringLiteral("--timestamps"), QStringLiteral("elapsed")}),
                                             std::nullopt);
        QVERIFY(settings);
        QCOMPARE(settings->directory, QDir(dir.path()).absolutePath());
        QCOMPARE(settings->filenameTemplate, QStringLiteral("capture.log"));
        QCOMPARE(settings->format, LogFormat::Raw);
        QCOMPARE(settings->timestamps, TimestampMode::Elapsed);
        Profile profile;
        profile.logging.autoStart = true;
        profile.logging.timestamps = TimestampMode::Delta;
        QCOMPARE(logSettingsFor(parse({QStringLiteral("x")}), profile)->timestamps, TimestampMode::Delta);
    }

    // -- formatting ------------------------------------------------------------

    void theTableLinesUp()
    {
        PortInfo usb = port(QStringLiteral("/dev/ttyUSB0"), QStringLiteral("FT232R USB UART"));
        usb.vid = 0x0403;
        usb.pid = 0x6001;
        usb.chipFamily = QStringLiteral("FTDI FT232R");
        usb.serialNumber = QStringLiteral("A50285BI");
        usb.driver = QStringLiteral("ftdi_sio");
        const QString table = formatPortTable({usb, port(QStringLiteral("/dev/ttyS0"))}, true);
        const QStringList lines = table.split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 3);
        QVERIFY(lines.at(0).startsWith(QStringLiteral("PORT          DESCRIPTION      VID:PID")));
        QCOMPARE(lines.at(1).indexOf(QStringLiteral("FT232R USB")), lines.at(0).indexOf(QStringLiteral("DESCRIPTION")));
        QCOMPARE(lines.at(2).indexOf(QLatin1Char('-')), lines.at(0).indexOf(QStringLiteral("DESCRIPTION")));
        QVERIFY(lines.at(0).contains(QStringLiteral("STABLE PATH")));
        QVERIFY(!formatPortTable({usb}, false).contains(QStringLiteral("DRIVER")));
    }

    void noPortsIsNotAnErrorAndSaysWhatToCheck()
    {
        QVERIFY(formatPortTable({}, false).startsWith(QStringLiteral("No serial ports found.")));
        QCOMPARE(QJsonDocument::fromJson(portsJson({})).array().size(), 0);
    }

    void exitCodesArePartOfTheContract()
    {
        QCOMPARE(exitCodeFor(PortsideError(ErrorKind::PermissionDenied, {})), 3);
        QCOMPARE(exitCodeFor(PortsideError(ErrorKind::PortBusy, {})), 4);
        QCOMPARE(exitCodeFor(PortsideError(ErrorKind::PortNotFound, {})), 5);
        QCOMPARE(exitCodeFor(PortsideError(ErrorKind::DeviceVanished, {})), 5);
        QCOMPARE(exitCodeFor(PortsideError(ErrorKind::Profile, {})), 2);
        QCOMPARE(exitCodeFor(PortsideError(ErrorKind::InvalidSettings, {})), 2);
        QCOMPARE(exitCodeFor(PortsideError(ErrorKind::Transport, {})), 1);
    }

    // -- the pass-through pump --------------------------------------------------

    void pipedInputIsForwardedByteExactIncludingTheEscapeByte()
    {
        LoopbackOptions options;
        options.echo = false;
        auto made = makeLoopbackSession(options);
        made.session->open();
        PassThroughOptions pumpOptions;
        pumpOptions.output = [](const QByteArray &) {};
        PassThrough pump(made.session.get(), pumpOptions);
        pump.handleInput(QByteArray("a\x1d" "b\r\x7f", 5));
        QVERIFY(waitFor([&] { return made.transport->written().size() == 5; }));
        QCOMPARE(made.transport->written(), QByteArray("a\x1d" "b\r\x7f", 5));
        QVERIFY(!pump.isFinished());
    }

    void ctrlBracketThenQQuitsAndSendsWhatCameBefore()
    {
        LoopbackOptions options;
        options.echo = false;
        auto made = makeLoopbackSession(options);
        made.session->open();
        QStringList status;
        PassThroughOptions pumpOptions;
        pumpOptions.interactive = true;
        pumpOptions.drain = 0;
        pumpOptions.output = [](const QByteArray &) {};
        pumpOptions.statusSink = [&](const QString &text) { status << text; };
        PassThrough pump(made.session.get(), pumpOptions);
        pump.handleInput("before\x1dqafter");
        QVERIFY(waitFor([&] { return pump.isFinished(); }));
        QCOMPARE(pump.outcome(), PassThrough::Outcome::Quit);
        QVERIFY(waitFor([&] { return made.transport->written() == "before"; }));
        QVERIFY(status.first().contains(QStringLiteral("q quit")));
    }

    void theCommandKeySendsABreakAndTogglesTheLines()
    {
        auto made = makeLoopbackSession();
        made.session->open();
        QStringList status;
        PassThroughOptions pumpOptions;
        pumpOptions.interactive = true;
        pumpOptions.breakDuration = 0.3;
        pumpOptions.output = [](const QByteArray &) {};
        pumpOptions.statusSink = [&](const QString &text) { status << text; };
        PassThrough pump(made.session.get(), pumpOptions);
        pump.handleInput("\x1d" "b");
        QVERIFY(waitFor([&] { return made.transport->breaks() == QList<double>{0.3}; }));
        QVERIFY(waitFor([&] { return status.last().contains(QStringLiteral("native break, 300 ms")); }));
        pump.handleInput("\x1d" "d");
        QVERIFY(waitFor([&] { return made.transport->dtr() == std::optional<bool>(false); }));
        pump.handleInput("\x1d]");
        QVERIFY(waitFor([&] { return made.transport->written() == "\x1d"; }));
    }

    void theProfilesLineDisciplineAppliesToTypedKeys()
    {
        LoopbackOptions options;
        options.echo = false;
        auto made = makeLoopbackSession(options);
        made.session->open();
        QByteArray shown;
        PassThroughOptions pumpOptions;
        pumpOptions.interactive = true;
        pumpOptions.translate = true;
        pumpOptions.text.sendEnding = LineEnding::CrLf;
        pumpOptions.text.backspace = Backspace::Bs;
        pumpOptions.text.localEcho = true;
        pumpOptions.output = [&](const QByteArray &bytes) { shown += bytes; };
        PassThrough pump(made.session.get(), pumpOptions);
        pump.handleInput("ab\x7f\r");
        QVERIFY(waitFor([&] { return made.transport->written() == "ab\x08\r\n"; }));
        QCOMPARE(shown, QByteArray("ab\b \b\r\n"));
    }

    void theDeviceRunningSzIsOfferedAndCtrlBracketZReceives()
    {
        LoopbackOptions options;
        options.echo = false;
        auto made = makeLoopbackSession(options);
        made.session->open();
        QTemporaryDir dir;
        QStringList status;
        PassThroughOptions pumpOptions;
        pumpOptions.interactive = true;
        pumpOptions.downloadDirectory = dir.path();
        pumpOptions.output = [](const QByteArray &) {};
        pumpOptions.statusSink = [&](const QString &text) { status << text; };
        PassThrough pump(made.session.get(), pumpOptions);
        made.transport->feed("rz\r**\x18" "B00000000000000\r\x8a\x11");
        QVERIFY(waitFor([&] { return status.join(QLatin1Char('\n')).contains(QStringLiteral("Ctrl-] then z")); }));
        pump.handleInput("\x1d" "z");
        QVERIFY(made.session->isStreamClaimed());
        QVERIFY(status.last().contains(QStringLiteral("receiving with ZMODEM")));
        // Typing does not reach the device; Ctrl-] cancels.
        pump.handleInput("abc");
        pump.handleInput("\x1d");
        QVERIFY(waitFor([&] { return !made.session->isStreamClaimed(); }, 5000));
        QVERIFY(waitFor([&] { return status.last().contains(QStringLiteral("cancelled")); }));
        QVERIFY(!made.transport->written().contains("abc"));
        QVERIFY(made.transport->written().contains(QByteArray(5, '\x18')));
        // And the terminal has the port back.
        pump.handleInput("show ver\r");
        QVERIFY(waitFor([&] { return made.transport->written().endsWith("show ver\r"); }));
    }

    void theSessionEndingEndsThePump()
    {
        auto made = makeLoopbackSession();
        made.session->open();
        PassThroughOptions pumpOptions;
        pumpOptions.output = [](const QByteArray &) {};
        PassThrough pump(made.session.get(), pumpOptions);
        made.transport->setVanishAfter(1);
        made.session->send("x");
        QVERIFY(waitFor([&] { return pump.isFinished(); }));
        QCOMPARE(pump.outcome(), PassThrough::Outcome::SessionEnded);
    }

    // -- the real binary -----------------------------------------------------------

    void versionAndHelp()
    {
        const Run version = runCli({QStringLiteral("--version")});
        QCOMPARE(version.code, 0);
        QVERIFY(version.out.startsWith("portside "));
        const Run bare = runCli({});
        QCOMPARE(bare.code, 2);
        QVERIFY(bare.err.contains("usage: portside"));
        const Run help = runCli({QStringLiteral("connect"), QStringLiteral("--help")});
        QCOMPARE(help.code, 0);
        QVERIFY(help.out.contains("--break-mode"));
    }

    void listRunsAndItsJsonParses()
    {
        const Run table = runCli({QStringLiteral("--list")});
        QCOMPARE(table.code, 0);
        const Run json = runCli({QStringLiteral("--list"), QStringLiteral("--json")});
        QCOMPARE(json.code, 0);
        QJsonParseError error{};
        QVERIFY(QJsonDocument::fromJson(json.out, &error).isArray());
        QCOMPARE(runCli({QStringLiteral("list"), QStringLiteral("--json"), QStringLiteral("-v")}).code, 0);
        QCOMPARE(runCli({QStringLiteral("frobnicate")}).code, 2);
    }

    void connectOverTheLoopUrlPipesBytesThrough()
    {
        const Run run = runCli({QStringLiteral("connect"), QStringLiteral("loop://"), QStringLiteral("--baud"),
                                QStringLiteral("115200"), QStringLiteral("--quiet"), QStringLiteral("--drain"),
                                QStringLiteral("0.5")},
                               "hello\r");
        QCOMPARE(run.code, 0);
        QCOMPARE(run.out, QByteArray("hello\r"));
        QVERIFY(run.err.isEmpty());
    }

    void statusGoesToStderrNotStdout()
    {
        const Run run = runCli({QStringLiteral("connect"), QStringLiteral("loop://"), QStringLiteral("--reconnect")}, "x");
        QCOMPARE(run.out, QByteArray("x"));
        QVERIFY(run.err.contains("[portside] connected to loop:// at 115200 8N1 none"));
        QVERIFY(run.err.contains("--reconnect needs a port"));
        QVERIFY(run.err.contains("[portside] closed loop:// · in 1 B · out 1 B"));
    }

    void connectingToAMissingPortExitsWithTheNotFoundCode()
    {
        const Run run = runCli({QStringLiteral("connect"), QStringLiteral("/dev/portside-no-such-port")}, "x");
        QCOMPARE(run.code, 5);
        QVERIFY(run.err.contains("no such port"));
        QVERIFY(run.err.contains("portside --list"));
    }

    void anUnknownProfileIsAUsageError()
    {
        QTemporaryDir dir;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("PORTSIDE_CONFIG_DIR"), dir.path());
        const Run run = runCli({QStringLiteral("connect"), QStringLiteral("loop://"), QStringLiteral("-p"), QStringLiteral("ghost")}, "x", env);
        QCOMPARE(run.code, 2);
        QVERIFY(run.err.contains("no profile named 'ghost'"));
    }

    void profilesImportShowExportAndDelete()
    {
        QTemporaryDir dir;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("PORTSIDE_CONFIG_DIR"), dir.filePath(QStringLiteral("config")));
        QVERIFY(runCli({QStringLiteral("profiles")}, {}, env).out.startsWith("No profiles yet."));

        writeFile(dir.filePath(QStringLiteral("lab.toml")),
                  "name = \"lab\"\ndevice_keys = [\"usb_0403_6001_X\"]\n[serial]\nbaudrate = 9600\n");
        const Run imported = runCli({QStringLiteral("profiles"), QStringLiteral("import"), dir.filePath(QStringLiteral("lab.toml"))}, {}, env);
        QCOMPARE(imported.code, 0);
        const Run listed = runCli({QStringLiteral("profiles"), QStringLiteral("list")}, {}, env);
        QCOMPARE(listed.out, QByteArray("lab\t9600 8N1 none\tusb_0403_6001_X\n"));
        const Run shown = runCli({QStringLiteral("profiles"), QStringLiteral("show"), QStringLiteral("lab")}, {}, env);
        QVERIFY(shown.out.contains("baudrate = 9600"));
        const QString target = dir.filePath(QStringLiteral("out.toml"));
        QCOMPARE(runCli({QStringLiteral("profiles"), QStringLiteral("export"), QStringLiteral("lab"), QStringLiteral("--file"), target}, {}, env).code, 0);
        QVERIFY(readFile(target).contains("name = \"lab\""));
        QCOMPARE(runCli({QStringLiteral("profiles"), QStringLiteral("delete"), QStringLiteral("lab")}, {}, env).out, QByteArray("deleted lab\n"));
        QCOMPARE(runCli({QStringLiteral("profiles"), QStringLiteral("show")}, {}, env).code, 2);
        QCOMPARE(runCli({QStringLiteral("profiles"), QStringLiteral("rename")}, {}, env).code, 2);
    }

    void aProfileDrivesAConnect()
    {
        QTemporaryDir dir;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("PORTSIDE_CONFIG_DIR"), dir.path());
        QDir(dir.path()).mkpath(QStringLiteral("profiles"));
        writeFile(dir.filePath(QStringLiteral("profiles/slow.toml")), "name = \"slow\"\n[serial]\nbaudrate = 2400\nparity = \"E\"\n");
        const Run run = runCli({QStringLiteral("connect"), QStringLiteral("loop://"), QStringLiteral("-p"), QStringLiteral("slow"),
                                QStringLiteral("--data-bits"), QStringLiteral("7")},
                               "x", env);
        QCOMPARE(run.code, 0);
        QVERIFY2(run.err.contains("at 2400 7E1 none"), run.err.constData());
    }

    void aSessionLogIsWrittenFromTheCommandLine()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("capture.log"));
        const Run run = runCli({QStringLiteral("connect"), QStringLiteral("loop://"), QStringLiteral("--quiet"),
                                QStringLiteral("--log"), path},
                               "show version\r\n");
        QCOMPARE(run.code, 0);
        QCOMPARE(readFile(path), QByteArray("show version\n"));
    }
};

QTEST_GUILESS_MAIN(TestCli)
#include "tst_cli.moc"
