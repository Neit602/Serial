#include "cli/cli.h"

#include "cli/console.h"
#include "cli/pass_through.h"
#include "core/discovery.h"
#include "core/factory.h"
#include "core/session.h"
#include "core/transfer/job.h"
#include "core/version.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <limits>
#include <memory>

namespace portside::cli {

namespace {

const char *const kUsage =
    "usage: portside [--version] [--list [--json] [-v]] <command> [options]\n"
    "\n"
    "Portside — cross-platform serial terminal.\n"
    "\n"
    "commands:\n"
    "  list                 list the serial ports on this machine\n"
    "  connect <port>       open a port: your terminal talks to the device\n"
    "  send <port> <files>  send files with XMODEM, YMODEM, ZMODEM, Kermit or raw\n"
    "  receive <port>       receive files with XMODEM, YMODEM, ZMODEM or Kermit\n"
    "  profiles [action]    list, show, export, import and delete profiles\n"
    "  gui                  open the desktop application\n"
    "\n"
    "options:\n"
    "  -h, --help           show this help and exit\n"
    "  --version            show the version and exit\n"
    "  --list               list the serial ports on this machine and exit\n"
    "  --json               machine-readable output for --list\n"
    "  -v, --verbose        show stable paths and driver details\n"
    "\n"
    "Run 'portside connect --help' for the connection options.\n";

void printOut(const QString &text)
{
    const QByteArray bytes = text.toUtf8();
    std::fwrite(bytes.constData(), 1, std::size_t(bytes.size()), stdout);
    std::fflush(stdout);
}

void printErr(const QString &text)
{
    const QByteArray bytes = text.toUtf8();
    std::fwrite(bytes.constData(), 1, std::size_t(bytes.size()), stderr);
    std::fflush(stderr);
}

int usageError(const QString &message, const QString &usage = QString::fromUtf8(kUsage))
{
    printErr(usage.section(QLatin1Char('\n'), 0, 0) + QLatin1Char('\n'));
    printErr(QStringLiteral("portside: error: %1\n").arg(message));
    return ExitUsage;
}

template <typename E>
std::optional<E> choiceOption(const QCommandLineParser &parser, const QString &name,
                              QString *error, bool upper = false)
{
    if (!parser.isSet(name))
        return std::nullopt;
    const QString value = upper ? parser.value(name).toUpper() : parser.value(name);
    const auto parsed = enumFromName<E>(value);
    if (!parsed) {
        *error = QStringLiteral("argument --%1: invalid choice '%2' (choose from %3)")
                     .arg(name, parser.value(name), enumNameList<E>().join(QStringLiteral(", ")));
    }
    return parsed;
}

QString connectHelp()
{
    return QStringLiteral(
        "usage: portside connect <port> [options]\n"
        "\n"
        "Open a port (COM3, /dev/ttyUSB0, /dev/serial/by-id/..., or loop:// for a test\n"
        "port that echoes).  Device output goes to stdout, Portside's own messages to\n"
        "stderr.  On a terminal, press Ctrl-] for the command menu (q quits, b sends a\n"
        "break, z receives a file the device sends with sz); piped input is forwarded\n"
        "byte-exact and ends the session at EOF.\n"
        "\n"
        "line settings:\n"
        "  -b, --baud RATE          baud rate, 50-4000000 (default 115200)\n"
        "  --data-bits {5,6,7,8}    data bits (default 8)\n"
        "  --parity {N,E,O,M,S}     parity (default N)\n"
        "  --stop-bits {1,1.5,2}    stop bits (default 1)\n"
        "  --flow {none,rtscts,dsrdtr,xonxoff}\n"
        "                           flow control (default none)\n"
        "  --reconnect              reconnect when the device disappears and returns\n"
        "  -p, --profile NAME       apply a saved profile; explicit options still win\n"
        "\n"
        "terminal:\n"
        "  --raw                    send keys byte-exact, without the profile's line discipline\n"
        "  --local-echo             echo typed characters locally\n"
        "  --send-ending {cr,lf,crlf,none}\n"
        "                           what Enter sends (default cr)\n"
        "  --backspace {bs,del}     what the backspace key sends (default del)\n"
        "  --break-mode {auto,native,emulated}\n"
        "                           how a break is produced (default auto)\n"
        "  --break-duration SECONDS break length (default 0.25)\n"
        "\n"
        "logging:\n"
        "  --log PATH               log this session to PATH\n"
        "  --log-format {text,raw}  decoded and timestamped, or byte-exact (default text)\n"
        "  --timestamps {off,absolute,elapsed,delta}\n"
        "                           per-line timestamps in the log (default off)\n"
        "\n"
        "  --drain SECONDS          keep reading this long before exiting (default 0.25)\n"
        "  -q, --quiet              suppress status messages\n"
        "  -h, --help               show this help and exit\n");
}

QString transferHelp(TransferDirection direction)
{
    const QString lineSettings = QStringLiteral(
        "line settings:\n"
        "  -b, --baud RATE          baud rate, 50-4000000 (default 115200)\n"
        "  --data-bits {5,6,7,8}    data bits (default 8)\n"
        "  --parity {N,E,O,M,S}     parity (default N)\n"
        "  --stop-bits {1,1.5,2}    stop bits (default 1)\n"
        "  --flow {none,rtscts,dsrdtr,xonxoff}\n"
        "                           flow control (default none)\n"
        "  -p, --profile NAME       apply a saved profile; explicit options still win\n"
        "\n");
    const QString common = QStringLiteral(
        "  --timeout SECONDS        how long to wait for the device to start (default 60)\n"
        "  -q, --quiet              no progress, only errors\n"
        "  -h, --help               show this help and exit\n"
        "\n"
        "Ctrl-C cancels cleanly: the device is told, with the protocol's own cancel.\n");
    if (direction == TransferDirection::Send) {
        return QStringLiteral(
                   "usage: portside send <port> <file>... [options]\n"
                   "\n"
                   "Send files to the device.  Start the receiver on the device first (rz, rb, rx,\n"
                   "kermit -r, U-Boot's loady/loadx...); ZMODEM starts rz itself by sending \"rz\".\n"
                   "\n")
               + lineSettings
               + QStringLiteral(
                   "transfer:\n"
                   "  -P, --protocol {xmodem,xmodem-1k,ymodem,ymodem-g,zmodem,kermit,raw}\n"
                   "                           protocol (default zmodem, or the profile's)\n"
                   "  --chunk-size BYTES       raw: bytes per write (default 256)\n"
                   "  --chunk-delay SECONDS    raw: pause between writes (default 0)\n")
               + common;
    }
    return QStringLiteral(
               "usage: portside receive <port> [options]\n"
               "\n"
               "Receive files from the device.  Start this first, then the sender on the device\n"
               "(sz, sb, sx, kermit -s...).  Files are written as NAME.part and renamed when\n"
               "complete; an existing file is never overwritten unless you say so.\n"
               "\n")
           + lineSettings
           + QStringLiteral(
               "transfer:\n"
               "  -P, --protocol {xmodem,xmodem-1k,ymodem,ymodem-g,zmodem,kermit}\n"
               "                           protocol (default zmodem, or the profile's)\n"
               "  -d, --into DIRECTORY     where files go (default the current directory)\n"
               "  -o, --output NAME        XMODEM: the file name (XMODEM sends none)\n"
               "  --overwrite {refuse,rename,overwrite}\n"
               "                           when a file exists (default refuse)\n"
               "  --text                   XMODEM: strip the end-of-file padding (text files)\n"
               "  --checksum               X/YMODEM: ask for the old 8-bit checksum, not CRC-16\n"
               "  --resume                 ZMODEM: continue an interrupted NAME.part download\n")
           + common;
}

const char *const kProfilesUsage =
    "usage: portside profiles [list|show|export|import|delete] [name] [--file PATH]\n";

} // namespace

int exitCodeFor(const PortsideError &error)
{
    switch (error.kind()) {
    case ErrorKind::Profile:
    case ErrorKind::InvalidSettings:
        return ExitUsage;
    case ErrorKind::PermissionDenied:
        return ExitPermission;
    case ErrorKind::PortBusy:
        return ExitBusy;
    case ErrorKind::PortNotFound:
    case ErrorKind::DeviceVanished:
        return ExitNotFound;
    default:
        return ExitFailure;
    }
}

QString formatPortTable(const QList<PortInfo> &ports, bool verbose)
{
    if (ports.isEmpty()) {
        return QStringLiteral(
            "No serial ports found.\n"
            "Plug in an adapter and try again; if it is plugged in, check that its driver is "
            "installed and that you have permission to open the port.");
    }
    QStringList headers = {QStringLiteral("PORT"), QStringLiteral("DESCRIPTION"),
                           QStringLiteral("VID:PID"), QStringLiteral("CHIP"),
                           QStringLiteral("SERIAL")};
    if (verbose)
        headers << QStringLiteral("DRIVER") << QStringLiteral("STABLE PATH");
    const auto orDash = [](const QString &value) {
        return value.isEmpty() ? QStringLiteral("-") : value;
    };
    QList<QStringList> rows;
    for (const PortInfo &port : ports) {
        QStringList row = {port.device, orDash(port.description), orDash(port.vidPid()),
                           orDash(port.chipFamily), orDash(port.serialNumber)};
        if (verbose)
            row << orDash(port.driver) << orDash(port.stablePath);
        rows << row;
    }
    QList<qsizetype> widths;
    for (qsizetype i = 0; i < headers.size(); ++i) {
        qsizetype width = headers.at(i).size();
        for (const QStringList &row : std::as_const(rows))
            width = std::max(width, row.at(i).size());
        widths << width;
    }
    const auto render = [&](const QStringList &cells) {
        QStringList padded;
        for (qsizetype i = 0; i < cells.size(); ++i)
            padded << cells.at(i).leftJustified(widths.at(i));
        QString line = padded.join(QStringLiteral("  "));
        while (line.endsWith(QLatin1Char(' ')))
            line.chop(1);
        return line;
    };
    QStringList lines = {render(headers)};
    for (const QStringList &row : std::as_const(rows))
        lines << render(row);
    return lines.join(QLatin1Char('\n'));
}

QByteArray portsJson(const QList<PortInfo> &ports)
{
    QJsonArray array;
    for (const PortInfo &port : ports)
        array.append(port.toJson());
    return QJsonDocument(array).toJson(QJsonDocument::Indented);
}

std::optional<int> parseBaud(const QString &text)
{
    QString digits = text.trimmed();
    digits.remove(QLatin1Char('_'));
    int base = 10;
    if (digits.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        base = 16;
        digits = digits.mid(2);
    } else if (digits.startsWith(QLatin1String("0o"), Qt::CaseInsensitive)) {
        base = 8;
        digits = digits.mid(2);
    } else if (digits.startsWith(QLatin1String("0b"), Qt::CaseInsensitive)) {
        base = 2;
        digits = digits.mid(2);
    }
    bool ok = false;
    const qlonglong value = digits.toLongLong(&ok, base);
    if (!ok || value < 0 || value > std::numeric_limits<int>::max())
        return std::nullopt;
    return int(value);
}

namespace {

QCommandLineOption valued(const QStringList &names, const QString &value)
{
    return QCommandLineOption(names, QString(), value);
}

// The line settings and profile options that connect, send and receive share.
void addLineOptions(QCommandLineParser &parser)
{
    parser.addOptions({
        valued({QStringLiteral("b"), QStringLiteral("baud")}, QStringLiteral("rate")),
        valued({QStringLiteral("data-bits")}, QStringLiteral("bits")),
        valued({QStringLiteral("parity")}, QStringLiteral("parity")),
        valued({QStringLiteral("stop-bits")}, QStringLiteral("bits")),
        valued({QStringLiteral("flow")}, QStringLiteral("mode")),
        valued({QStringLiteral("p"), QStringLiteral("profile")}, QStringLiteral("name")),
        QCommandLineOption(QStringList{QStringLiteral("q"), QStringLiteral("quiet")}),
        QCommandLineOption(QStringList{QStringLiteral("h"), QStringLiteral("help")}),
    });
}

bool readLineOptions(const QCommandLineParser &parser, ConnectOptions &options, QString *error)
{
    if (parser.isSet(QStringLiteral("baud"))) {
        const QString text = parser.value(QStringLiteral("baud"));
        const auto baud = parseBaud(text);
        if (!baud) {
            *error = QStringLiteral("argument --baud: '%1' is not an integer baud rate").arg(text);
            return false;
        }
        if (*baud < kBaudMin || *baud > kBaudMax) {
            *error = QStringLiteral("argument --baud: baud rate must be between %1 and %2, got %3")
                         .arg(kBaudMin)
                         .arg(kBaudMax)
                         .arg(*baud);
            return false;
        }
        options.baud = baud;
    }
    if (parser.isSet(QStringLiteral("data-bits"))) {
        bool ok = false;
        const int bits = parser.value(QStringLiteral("data-bits")).toInt(&ok);
        if (!ok || std::find(kDataBitsChoices.begin(), kDataBitsChoices.end(), bits)
                       == kDataBitsChoices.end()) {
            *error = QStringLiteral("argument --data-bits: invalid choice '%1' (choose from 5, 6, 7, 8)")
                         .arg(parser.value(QStringLiteral("data-bits")));
            return false;
        }
        options.dataBits = bits;
    }
    options.parity = choiceOption<Parity>(parser, QStringLiteral("parity"), error, true);
    options.stopBits = choiceOption<StopBits>(parser, QStringLiteral("stop-bits"), error);
    options.flow = choiceOption<FlowControl>(parser, QStringLiteral("flow"), error);
    options.profile = parser.value(QStringLiteral("profile"));
    options.quiet = parser.isSet(QStringLiteral("quiet"));
    return error->isEmpty();
}

std::optional<double> secondsOption(const QCommandLineParser &parser, const QString &name,
                                    bool allowZero, QString *error)
{
    bool ok = false;
    const double value = parser.value(name).toDouble(&ok);
    if (!ok || value < 0 || (!allowZero && value == 0)) {
        *error = QStringLiteral("argument --%1: '%2' is not a valid number of seconds")
                     .arg(name, parser.value(name));
        return std::nullopt;
    }
    return value;
}

} // namespace

std::optional<ConnectOptions> parseConnectArguments(const QStringList &arguments, QString *error)
{
    QCommandLineParser parser;
    parser.addPositionalArgument(QStringLiteral("port"), QStringLiteral("port name or loop://"));
    addLineOptions(parser);
    parser.addOptions({
        QCommandLineOption(QStringLiteral("reconnect")),
        QCommandLineOption(QStringLiteral("raw")),
        QCommandLineOption(QStringLiteral("local-echo")),
        valued({QStringLiteral("send-ending")}, QStringLiteral("ending")),
        valued({QStringLiteral("backspace")}, QStringLiteral("key")),
        valued({QStringLiteral("break-mode")}, QStringLiteral("mode")),
        valued({QStringLiteral("break-duration")}, QStringLiteral("seconds")),
        valued({QStringLiteral("log")}, QStringLiteral("path")),
        valued({QStringLiteral("log-format")}, QStringLiteral("format")),
        valued({QStringLiteral("timestamps")}, QStringLiteral("mode")),
        valued({QStringLiteral("drain")}, QStringLiteral("seconds")),
    });
    if (!parser.parse(QStringList{QStringLiteral("portside connect")} + arguments)) {
        *error = parser.errorText();
        return std::nullopt;
    }
    if (parser.isSet(QStringLiteral("help"))) {
        error->clear();
        return std::nullopt;
    }

    ConnectOptions options;
    const QStringList positional = parser.positionalArguments();
    if (positional.isEmpty()) {
        *error = QStringLiteral("the following arguments are required: port");
        return std::nullopt;
    }
    if (positional.size() > 1) {
        *error = QStringLiteral("unrecognized arguments: %1").arg(positional.mid(1).join(QLatin1Char(' ')));
        return std::nullopt;
    }
    options.port = positional.first();
    if (!readLineOptions(parser, options, error))
        return std::nullopt;

    options.sendEnding = choiceOption<LineEnding>(parser, QStringLiteral("send-ending"), error);
    options.backspace = choiceOption<Backspace>(parser, QStringLiteral("backspace"), error);
    options.breakMode = choiceOption<BreakMode>(parser, QStringLiteral("break-mode"), error);
    options.timestamps = choiceOption<TimestampMode>(parser, QStringLiteral("timestamps"), error);
    if (const auto format = choiceOption<LogFormat>(parser, QStringLiteral("log-format"), error))
        options.logFormat = *format;
    if (!error->isEmpty())
        return std::nullopt;

    if (parser.isSet(QStringLiteral("break-duration"))) {
        options.breakDuration =
            secondsOption(parser, QStringLiteral("break-duration"), false, error);
        if (!options.breakDuration)
            return std::nullopt;
    }
    if (parser.isSet(QStringLiteral("drain"))) {
        const auto drain = secondsOption(parser, QStringLiteral("drain"), true, error);
        if (!drain)
            return std::nullopt;
        options.drain = *drain;
    }

    options.reconnect = parser.isSet(QStringLiteral("reconnect"));
    options.raw = parser.isSet(QStringLiteral("raw"));
    options.localEcho = parser.isSet(QStringLiteral("local-echo"));
    options.log = parser.value(QStringLiteral("log"));
    return options;
}

std::optional<TransferCommand> parseTransferArguments(TransferDirection direction,
                                                      const QStringList &arguments, QString *error)
{
    const bool sending = direction == TransferDirection::Send;
    QCommandLineParser parser;
    parser.addPositionalArgument(QStringLiteral("port"), QStringLiteral("port name"));
    if (sending)
        parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("files to send"));
    addLineOptions(parser);
    parser.addOptions({
        valued({QStringLiteral("P"), QStringLiteral("protocol")}, QStringLiteral("protocol")),
        valued({QStringLiteral("timeout")}, QStringLiteral("seconds")),
    });
    if (sending) {
        parser.addOptions({
            valued({QStringLiteral("chunk-size")}, QStringLiteral("bytes")),
            valued({QStringLiteral("chunk-delay")}, QStringLiteral("seconds")),
        });
    } else {
        parser.addOptions({
            valued({QStringLiteral("d"), QStringLiteral("into")}, QStringLiteral("directory")),
            valued({QStringLiteral("o"), QStringLiteral("output")}, QStringLiteral("name")),
            valued({QStringLiteral("overwrite")}, QStringLiteral("policy")),
            QCommandLineOption(QStringLiteral("text")),
            QCommandLineOption(QStringLiteral("checksum")),
            QCommandLineOption(QStringLiteral("resume")),
        });
    }
    const QString command = sending ? QStringLiteral("portside send") : QStringLiteral("portside receive");
    if (!parser.parse(QStringList{command} + arguments)) {
        *error = parser.errorText();
        return std::nullopt;
    }
    if (parser.isSet(QStringLiteral("help"))) {
        error->clear();
        return std::nullopt;
    }

    TransferCommand result;
    result.direction = direction;
    const QStringList positional = parser.positionalArguments();
    if (positional.isEmpty()) {
        *error = sending ? QStringLiteral("the following arguments are required: port, files")
                         : QStringLiteral("the following arguments are required: port");
        return std::nullopt;
    }
    if (sending && positional.size() < 2) {
        *error = QStringLiteral("the following arguments are required: files");
        return std::nullopt;
    }
    if (!sending && positional.size() > 1) {
        *error = QStringLiteral("unrecognized arguments: %1").arg(positional.mid(1).join(QLatin1Char(' ')));
        return std::nullopt;
    }
    result.line.port = positional.first();
    result.files = positional.mid(1);
    if (!readLineOptions(parser, result.line, error))
        return std::nullopt;

    result.protocol = choiceOption<Protocol>(parser, QStringLiteral("protocol"), error);
    if (!sending)
        result.overwrite = choiceOption<OverwritePolicy>(parser, QStringLiteral("overwrite"), error);
    if (!error->isEmpty())
        return std::nullopt;
    if (!sending && result.protocol == Protocol::Raw) {
        *error = QStringLiteral("argument --protocol: raw can only send; use a raw session log "
                                "(portside connect --log FILE --log-format raw) to capture bytes");
        return std::nullopt;
    }
    if (parser.isSet(QStringLiteral("timeout"))) {
        result.timeout = secondsOption(parser, QStringLiteral("timeout"), false, error);
        if (!result.timeout)
            return std::nullopt;
    }
    if (sending) {
        if (parser.isSet(QStringLiteral("chunk-size"))) {
            bool ok = false;
            result.chunkSize = parser.value(QStringLiteral("chunk-size")).toInt(&ok);
            if (!ok || result.chunkSize < 1) {
                *error = QStringLiteral("argument --chunk-size: '%1' is not a positive number of bytes")
                             .arg(parser.value(QStringLiteral("chunk-size")));
                return std::nullopt;
            }
        }
        if (parser.isSet(QStringLiteral("chunk-delay"))) {
            const auto delay = secondsOption(parser, QStringLiteral("chunk-delay"), true, error);
            if (!delay)
                return std::nullopt;
            result.chunkDelay = *delay;
        }
    } else {
        result.directory = parser.value(QStringLiteral("into"));
        result.outputName = parser.value(QStringLiteral("output"));
        result.textMode = parser.isSet(QStringLiteral("text"));
        result.checksum = parser.isSet(QStringLiteral("checksum"));
        result.resume = parser.isSet(QStringLiteral("resume"));
    }
    return result;
}

TransferOptions transferOptionsFor(const TransferCommand &command,
                                   const std::optional<Profile> &profile)
{
    TransferOptions options;
    const TransferDefaults defaults = profile ? profile->transfer : TransferDefaults{};
    options.protocol = command.protocol.value_or(defaults.protocol);
    // Scripts expect the current directory; the GUI uses Downloads.
    options.directory = !command.directory.isEmpty() ? command.directory
                        : !defaults.directory.isEmpty() ? defaults.directory
                                                        : QDir::currentPath();
    options.overwrite = command.overwrite.value_or(defaults.overwrite);
    options.outputName = command.outputName;
    options.textMode = command.textMode;
    options.checksum = command.checksum;
    options.resume = command.resume;
    options.rawChunkSize = command.chunkSize;
    options.rawChunkDelay = command.chunkDelay;
    if (command.timeout)
        options.startTimeout = *command.timeout;
    return options;
}

SerialSettings settingsFor(const ConnectOptions &options, const std::optional<Profile> &profile)
{
    SerialSettings settings = profile ? profile->serial : SerialSettings{};
    if (options.baud)
        settings.baudrate = *options.baud;
    if (options.dataBits)
        settings.dataBits = *options.dataBits;
    if (options.parity)
        settings.parity = *options.parity;
    if (options.stopBits)
        settings.stopBits = *options.stopBits;
    if (options.flow)
        settings.flowControl = *options.flow;
    return settings;
}

TextPolicy textPolicyFor(const ConnectOptions &options, const std::optional<Profile> &profile)
{
    TextPolicy policy = profile ? profile->text : TextPolicy{};
    if (options.sendEnding)
        policy.sendEnding = *options.sendEnding;
    if (options.backspace)
        policy.backspace = *options.backspace;
    policy.localEcho = options.localEcho || policy.localEcho;
    return policy;
}

std::optional<LogSettings> logSettingsFor(const ConnectOptions &options,
                                          const std::optional<Profile> &profile)
{
    const TimestampMode stamps = options.timestamps.value_or(
        profile ? profile->logging.timestamps : TimestampMode::Off);
    if (!options.log.isEmpty()) {
        const QFileInfo target(options.log);
        LogSettings settings;
        settings.directory = target.absolutePath();
        settings.filenameTemplate = target.fileName();
        settings.format = options.logFormat;
        settings.timestamps = stamps;
        settings.autoStart = true;
        return settings;
    }
    if (profile && profile->logging.autoStart) {
        LogSettings settings = profile->logging;
        settings.timestamps = stamps;
        return settings;
    }
    return std::nullopt;
}

namespace {

int cmdList(bool json, bool verbose)
{
    const QList<PortInfo> ports = listPorts();
    if (json)
        printOut(QString::fromUtf8(portsJson(ports)));
    else
        printOut(formatPortTable(ports, verbose) + QLatin1Char('\n'));
    return ExitOk;
}

int cmdConnect(const QStringList &arguments)
{
    QString error;
    const auto parsed = parseConnectArguments(arguments, &error);
    if (!parsed) {
        if (error.isEmpty()) {
            printOut(connectHelp());
            return ExitOk;
        }
        return usageError(error, connectHelp());
    }
    const ConnectOptions &options = *parsed;
    const auto status = [&](const QString &text) {
        if (!options.quiet)
            Console::writeErr(text);
    };

    std::optional<Profile> profile;
    if (!options.profile.isEmpty())
        profile = ProfileStore().load(options.profile);
    const SerialSettings settings = settingsFor(options, profile);
    settings.validate();

    const auto [device, identity] = resolvePort(options.port);
    SessionOptions sessionOptions;
    sessionOptions.settings = settings;
    sessionOptions.identity = identity;
    sessionOptions.breakMode = options.breakMode.value_or(profile ? profile->breakMode : BreakMode::Auto);
    if (options.reconnect && identity) {
        sessionOptions.reconnect.enabled = true;
        sessionOptions.reopen = identityReopener(*identity, settings);
    } else if (options.reconnect) {
        status(QStringLiteral("[portside] --reconnect needs a port that discovery lists (URLs and "
                              "unlisted ports have no device identity); continuing without it."));
    }

    Session session(createTransport(device, settings), sessionOptions);
    std::optional<PortsideError> fatal;
    QObject::connect(&session, &Session::stateChanged, &session,
                     [&](SessionState, SessionState state, const QString &detail) {
                         if (state == SessionState::Reconnecting)
                             status(QStringLiteral("[portside] device lost: %1").arg(detail.isEmpty() ? QStringLiteral("disconnected") : detail));
                         else if (state == SessionState::Connected && !detail.isEmpty())
                             status(QStringLiteral("[portside] %1").arg(detail));
                     });
    QObject::connect(&session, &Session::reconnectAttempt, &session,
                     [&](int attempt, double delay, const QString &) {
                         status(QStringLiteral("[portside] reconnect attempt %1 in %2s")
                                    .arg(attempt)
                                    .arg(delay, 0, 'f', 1));
                     });
    QObject::connect(&session, &Session::errorRaised, &session,
                     [&](const PortsideError &raised, bool isFatal) {
                         if (!isFatal)
                             return;
                         fatal = raised;
                         status(QStringLiteral("[portside] error: %1").arg(raised.display()));
                     });

    session.open();

    std::unique_ptr<SessionLogger> logger;
    if (const auto logSettings = logSettingsFor(options, profile)) {
        logger = std::make_unique<SessionLogger>(*logSettings, device,
                                                 profile ? profile->name : QString());
        logger->attach(&session);
        const QString path = logger->start();
        status(QStringLiteral("[portside] logging to %1").arg(QDir::toNativeSeparators(path)));
    }

    const bool interactive = Console::stdinIsTty();
    status(QStringLiteral("[portside] connected to %1 at %2").arg(device, session.settings().describe()));
    if (interactive)
        status(QStringLiteral("[portside] press Ctrl-] then q to quit, Ctrl-] then b to send a break"));

    PassThroughOptions pumpOptions;
    pumpOptions.interactive = interactive;
    pumpOptions.translate = interactive && !options.raw;
    pumpOptions.text = textPolicyFor(options, profile);
    pumpOptions.drain = options.drain;
    pumpOptions.breakDuration = options.breakDuration.value_or(profile ? profile->breakDuration : kDefaultBreakDuration);
    pumpOptions.quiet = options.quiet;
    if (profile) {
        pumpOptions.downloadDirectory = profile->transfer.directory;
        pumpOptions.overwrite = profile->transfer.overwrite;
    }

    PassThrough::Outcome outcome;
    {
        Console console;
        if (interactive)
            console.enterRaw();
        PassThrough pump(&session, pumpOptions);
        outcome = pump.run();
        console.restore();
    }
    session.close();
    if (logger)
        logger->detach();
    status(QStringLiteral("[portside] closed %1 · in %2 B · out %3 B")
               .arg(device)
               .arg(session.bytesIn())
               .arg(session.bytesOut()));
    if (outcome == PassThrough::Outcome::SessionEnded && fatal)
        return exitCodeFor(*fatal);
    return ExitOk;
}

// Ctrl-C during a transfer cancels it with the protocol's own sequence
// rather than killing the process mid-block and leaving the device waiting.
std::atomic<bool> g_transferInterrupted{false};

extern "C" void onTransferInterrupt(int)
{
    g_transferInterrupted = true;
}

class ProgressPrinter
{
public:
    explicit ProgressPrinter(bool quiet)
        : m_quiet(quiet)
        , m_live(Console::stderrIsTty())
    {
    }

    void update(const TransferProgress &progress)
    {
        if (m_quiet || progress.file.isEmpty())
            return;
        const bool newFile = progress.fileIndex != m_fileIndex;
        m_fileIndex = progress.fileIndex;
        if (!m_live) {
            // A log or a CI job: one line per file, not a progress bar.
            if (newFile)
                Console::writeErr(QStringLiteral("[portside] %1").arg(progress.file));
            return;
        }
        QString line = QStringLiteral("[portside] %1  ").arg(progress.file);
        if (progress.fileSize > 0) {
            const int percent = int(100 * progress.fileBytes / progress.fileSize);
            line += QStringLiteral("%1%  %2 of %3")
                        .arg(percent, 3)
                        .arg(formatBytes(progress.fileBytes), formatBytes(progress.fileSize));
        } else {
            line += formatBytes(progress.fileBytes);
        }
        if (progress.bytesPerSecond() > 0)
            line += QStringLiteral("  %1/s").arg(formatBytes(qint64(progress.bytesPerSecond())));
        if (progress.retries > 0)
            line += QStringLiteral("  %1 retries").arg(progress.retries);
        const QByteArray bytes = (QStringLiteral("\r\x1b[K") + line).toUtf8();
        std::fwrite(bytes.constData(), 1, std::size_t(bytes.size()), stderr);
        std::fflush(stderr);
        m_open = true;
    }

    void finish()
    {
        if (m_open)
            std::fputs("\n", stderr);
        m_open = false;
    }

private:
    bool m_quiet;
    bool m_live;
    bool m_open = false;
    int m_fileIndex = -1;
};

int cmdTransfer(TransferDirection direction, const QStringList &arguments)
{
    QString error;
    const auto parsed = parseTransferArguments(direction, arguments, &error);
    if (!parsed) {
        if (error.isEmpty()) {
            printOut(transferHelp(direction));
            return ExitOk;
        }
        return usageError(error, transferHelp(direction));
    }
    const TransferCommand &command = *parsed;
    const bool quiet = command.line.quiet;
    const auto status = [&](const QString &text) {
        if (!quiet)
            Console::writeErr(text);
    };

    std::optional<Profile> profile;
    if (!command.line.profile.isEmpty())
        profile = ProfileStore().load(command.line.profile);
    const SerialSettings settings = settingsFor(command.line, profile);
    settings.validate();
    const TransferOptions options = transferOptionsFor(command, profile);
    if (direction == TransferDirection::Receive && !protocolCanReceive(options.protocol))
        return usageError(QStringLiteral("the profile's protocol, raw, can only send"),
                          transferHelp(direction));
    if (direction == TransferDirection::Send && !protocolIsBatch(options.protocol)
        && command.files.size() > 1)
        return usageError(QStringLiteral("%1 sends one file at a time; use ymodem, zmodem or kermit")
                              .arg(protocolTitle(options.protocol)),
                          transferHelp(direction));

    const auto [device, identity] = resolvePort(command.line.port);
    SessionOptions sessionOptions;
    sessionOptions.settings = settings;
    sessionOptions.identity = identity;
    Session session(createTransport(device, settings), sessionOptions);
    session.open();
    status(QStringLiteral("[portside] connected to %1 at %2").arg(device, settings.describe()));
    if (direction == TransferDirection::Send)
        status(QStringLiteral("[portside] %1: waiting for the device to receive...")
                   .arg(protocolTitle(options.protocol)));
    else
        status(QStringLiteral("[portside] %1: waiting for the device to send into %2...")
                   .arg(protocolTitle(options.protocol), QDir::toNativeSeparators(options.directory)));

    TransferJob job(&session, direction, command.files, options);
    ProgressPrinter printer(quiet);
    QEventLoop loop;
    std::optional<TransferResult> result;
    std::optional<PortsideError> failure;
    QObject::connect(&job, &TransferJob::progressed, &loop,
                     [&](const TransferProgress &progress) { printer.update(progress); });
    QObject::connect(&job, &TransferJob::finished, &loop, [&](const TransferResult &done) {
        result = done;
        loop.quit();
    });
    QObject::connect(&job, &TransferJob::failed, &loop, [&](const PortsideError &raised) {
        failure = raised;
        loop.quit();
    });
    g_transferInterrupted = false;
    const auto previous = std::signal(SIGINT, onTransferInterrupt);
    QTimer interrupts;
    QObject::connect(&interrupts, &QTimer::timeout, &loop, [&] {
        if (g_transferInterrupted)
            job.cancel();
    });
    interrupts.start(50);
    job.start();
    loop.exec();
    interrupts.stop();
    std::signal(SIGINT, previous);
    printer.finish();
    session.close();

    if (failure) {
        printErr(QStringLiteral("portside: %1\n").arg(failure->display()));
        return exitCodeFor(*failure);
    }
    for (const TransferredFile &file : result->files) {
        if (file.skipped)
            printErr(QStringLiteral("portside: skipped %1: %2\n").arg(file.name, file.note));
        else if (direction == TransferDirection::Receive)
            status(QStringLiteral("[portside] saved %1").arg(QDir::toNativeSeparators(file.path)));
    }
    status(QStringLiteral("[portside] %1").arg(result->summary()));
    return result->skippedCount() > 0 ? ExitFailure : ExitOk;
}

int cmdProfiles(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.addPositionalArgument(QStringLiteral("action"), QString());
    parser.addPositionalArgument(QStringLiteral("name"), QString());
    parser.addOption(QCommandLineOption(QStringLiteral("file"), QString(), QStringLiteral("path")));
    parser.addOption(QCommandLineOption(QStringList{QStringLiteral("h"), QStringLiteral("help")}));
    if (!parser.parse(QStringList{QStringLiteral("portside profiles")} + arguments))
        return usageError(parser.errorText(), QString::fromUtf8(kProfilesUsage));
    if (parser.isSet(QStringLiteral("help"))) {
        printOut(QString::fromUtf8(kProfilesUsage));
        return ExitOk;
    }
    const QStringList positional = parser.positionalArguments();
    if (positional.size() > 2)
        return usageError(QStringLiteral("unrecognized arguments: %1").arg(positional.mid(2).join(QLatin1Char(' '))),
                          QString::fromUtf8(kProfilesUsage));
    const QString action = positional.value(0, QStringLiteral("list"));
    const QString name = positional.value(1);
    const QStringList actions = {QStringLiteral("list"), QStringLiteral("show"),
                                 QStringLiteral("export"), QStringLiteral("import"),
                                 QStringLiteral("delete")};
    if (!actions.contains(action)) {
        return usageError(QStringLiteral("argument action: invalid choice '%1' (choose from %2)")
                              .arg(action, actions.join(QStringLiteral(", "))),
                          QString::fromUtf8(kProfilesUsage));
    }

    const ProfileStore store;
    if (action == QLatin1String("list")) {
        const QList<Profile> profiles = store.all();
        if (store.names().isEmpty()) {
            printOut(QStringLiteral("No profiles yet. They will be stored in %1\n")
                         .arg(QDir::toNativeSeparators(store.directory())));
            return ExitOk;
        }
        for (const Profile &profile : profiles) {
            const QString devices = profile.deviceKeys.isEmpty() ? QStringLiteral("-")
                                                                 : profile.deviceKeys.join(QStringLiteral(", "));
            printOut(QStringLiteral("%1\t%2\t%3\n").arg(profile.name, profile.serial.describe(), devices));
        }
        return ExitOk;
    }

    const QString file = parser.value(QStringLiteral("file"));
    if (name.isEmpty() && !(action == QLatin1String("import") && !file.isEmpty())) {
        printErr(QStringLiteral("portside: 'profiles %1' needs a profile name\n").arg(action));
        return ExitUsage;
    }
    if (action == QLatin1String("show")) {
        printOut(store.load(name).toToml());
        return ExitOk;
    }
    if (action == QLatin1String("export")) {
        const QString target = file.isEmpty() ? name + QStringLiteral(".toml") : file;
        printOut(QDir::toNativeSeparators(store.exportTo(store.load(name), target)) + QLatin1Char('\n'));
        return ExitOk;
    }
    if (action == QLatin1String("import")) {
        const Profile imported = store.importFile(file.isEmpty() ? name : file);
        printOut(QDir::toNativeSeparators(store.save(imported)) + QLatin1Char('\n'));
        return ExitOk;
    }
    store.remove(name);
    printOut(QStringLiteral("deleted %1\n").arg(name));
    return ExitOk;
}

int cmdGui(const QStringList &arguments)
{
    const QDir here(QCoreApplication::applicationDirPath());
#if defined(Q_OS_WIN)
    const QString program = here.filePath(QStringLiteral("portside-gui.exe"));
#elif defined(Q_OS_MACOS)
    // Built next to the CLI, the app is a bundle.
    QString program = here.filePath(QStringLiteral("portside-gui.app/Contents/MacOS/portside-gui"));
    if (!QFileInfo(program).isExecutable())
        program = here.filePath(QStringLiteral("portside-gui"));
#else
    const QString program = here.filePath(QStringLiteral("portside-gui"));
#endif
    if (!QFileInfo(program).isExecutable()) {
        printErr(QStringLiteral(
                     "portside: the desktop application (portside-gui) is not installed next to "
                     "this program.\nThe command line works without it:\n\n"
                     "    portside --list\n    portside connect <port> --baud 115200\n"));
        return ExitFailure;
    }
    if (!QProcess::startDetached(program, arguments)) {
        printErr(QStringLiteral("portside: could not start %1\n").arg(QDir::toNativeSeparators(program)));
        return ExitFailure;
    }
    return ExitOk;
}

} // namespace

int run(const QStringList &arguments)
{
    QStringList rest = arguments;
    bool listFlag = false;
    bool json = false;
    bool verbose = false;
    while (!rest.isEmpty() && rest.first().startsWith(QLatin1Char('-'))) {
        const QString option = rest.takeFirst();
        if (option == QLatin1String("-h") || option == QLatin1String("--help")) {
            printOut(QString::fromUtf8(kUsage));
            return ExitOk;
        }
        if (option == QLatin1String("--version")) {
            printOut(QStringLiteral("portside %1\n").arg(QString::fromLatin1(kVersion)));
            return ExitOk;
        }
        if (option == QLatin1String("--list"))
            listFlag = true;
        else if (option == QLatin1String("--json"))
            json = true;
        else if (option == QLatin1String("-v") || option == QLatin1String("--verbose"))
            verbose = true;
        else
            return usageError(QStringLiteral("unrecognized arguments: %1").arg(option));
    }

    const QString command = rest.isEmpty() ? QString() : rest.takeFirst();
    try {
        if (listFlag || command == QLatin1String("list")) {
            if (listFlag && !command.isEmpty() && command != QLatin1String("list"))
                rest.prepend(command);
            for (const QString &option : std::as_const(rest)) {
                if (option == QLatin1String("--json"))
                    json = true;
                else if (option == QLatin1String("-v") || option == QLatin1String("--verbose"))
                    verbose = true;
                else
                    return usageError(QStringLiteral("unrecognized arguments: %1").arg(option));
            }
            return cmdList(json, verbose);
        }
        if (command == QLatin1String("connect"))
            return cmdConnect(rest);
        if (command == QLatin1String("send"))
            return cmdTransfer(TransferDirection::Send, rest);
        if (command == QLatin1String("receive"))
            return cmdTransfer(TransferDirection::Receive, rest);
        if (command == QLatin1String("profiles"))
            return cmdProfiles(rest);
        if (command == QLatin1String("gui"))
            return cmdGui(rest);
        if (command.isEmpty()) {
            printErr(QString::fromUtf8(kUsage));
            return ExitUsage;
        }
        return usageError(QStringLiteral("invalid choice: '%1' (choose from list, connect, send, receive, profiles, gui)")
                              .arg(command));
    } catch (const PortsideError &raised) {
        printErr(QStringLiteral("portside: %1\n").arg(raised.display()));
        return exitCodeFor(raised);
    }
}

} // namespace portside::cli
