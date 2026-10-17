#pragma once

// `portside` command line front end (PRD F23).
//
// Commands:
//
// * portside --list            — every port with the details F1 asks for,
//                                optionally as JSON for scripts.
// * portside connect <port>    — console ↔ port pass-through, with break,
//                                logging, profiles and auto-reconnect.
// * portside send <port> <files> / portside receive <port>
//                              — XMODEM, YMODEM, ZMODEM, Kermit and raw file
//                                transfers, for scripts and CI.
// * portside profiles [...]    — list, show, export, import and delete
//                                saved profiles (F20).
// * portside gui               — open the desktop application.

#include "core/breaks.h"
#include "core/errors.h"
#include "core/ports.h"
#include "core/profiles.h"
#include "core/session_log.h"
#include "core/transfer/transfer.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace portside::cli {

// Exit codes are part of the CLI contract (DECISIONS D8).
enum ExitCode : int {
    ExitOk = 0,
    ExitFailure = 1,
    ExitUsage = 2,
    ExitPermission = 3,
    ExitBusy = 4,
    ExitNotFound = 5,
};

// Map a typed error to the exit code scripts can branch on.
int exitCodeFor(const PortsideError &error);

// Render the port list as an aligned table (F1).
QString formatPortTable(const QList<PortInfo> &ports, bool verbose);
// The port list as a JSON array.
QByteArray portsJson(const QList<PortInfo> &ports);

// Parse a baud rate the way people type one: 115200, 115_200 or 0x1C200.
std::optional<int> parseBaud(const QString &text);

// Everything `connect` accepts.  An option the user did not type stays empty,
// so a profile can supply it — explicit flags beat a profile (D26).
struct ConnectOptions
{
    QString port;
    std::optional<int> baud;
    std::optional<int> dataBits;
    std::optional<Parity> parity;
    std::optional<StopBits> stopBits;
    std::optional<FlowControl> flow;
    bool reconnect = false;
    QString profile;
    bool raw = false;
    bool localEcho = false;
    std::optional<LineEnding> sendEnding;
    std::optional<Backspace> backspace;
    std::optional<BreakMode> breakMode;
    std::optional<double> breakDuration;
    QString log;
    LogFormat logFormat = LogFormat::Text;
    std::optional<TimestampMode> timestamps;
    double drain = 0.25;
    bool quiet = false;
};

// Parse `connect`'s arguments (everything after the word "connect").
// Returns the options, or sets `error` to a usage message.
std::optional<ConnectOptions> parseConnectArguments(const QStringList &arguments, QString *error);

// Everything `send` and `receive` accept (F17–F19, F23).
struct TransferCommand
{
    TransferDirection direction = TransferDirection::Send;
    // The port, line settings, profile and --quiet.
    ConnectOptions line;
    QStringList files;
    std::optional<Protocol> protocol;
    std::optional<double> timeout;
    // receive
    QString directory;
    QString outputName;
    std::optional<OverwritePolicy> overwrite;
    bool textMode = false;
    bool checksum = false;
    bool resume = false;
    // raw send
    int chunkSize = 256;
    double chunkDelay = 0.0;
};

std::optional<TransferCommand> parseTransferArguments(TransferDirection direction,
                                                      const QStringList &arguments, QString *error);
// Transfer options from the profile's [transfer] table, overridden by flags.
TransferOptions transferOptionsFor(const TransferCommand &command,
                                   const std::optional<Profile> &profile);

// Line settings from the profile, overridden by explicit options (F3).
SerialSettings settingsFor(const ConnectOptions &options, const std::optional<Profile> &profile);
// Line discipline from the profile, overridden by explicit options (F9).
TextPolicy textPolicyFor(const ConnectOptions &options, const std::optional<Profile> &profile);
// Log settings when logging was asked for, by --log or by the profile.
std::optional<LogSettings> logSettingsFor(const ConnectOptions &options,
                                          const std::optional<Profile> &profile);

// Run the CLI.  `arguments` excludes the program name.  Returns the exit code.
int run(const QStringList &arguments);

} // namespace portside::cli
