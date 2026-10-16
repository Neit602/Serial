#include "core/platform.h"

#include <QDir>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <variant>
#include <vector>

namespace portside {

QList<PortInfo> Platform::preparePorts(const QList<PortInfo> &ports) const
{
    QList<PortInfo> prepared;
    for (const PortInfo &port : filterPorts(ports))
        prepared << enrichPort(port);
    return prepared;
}

namespace {

// One natural-order chunk: text sorts before a number at the same position,
// numbers compare by value.
using Chunk = std::variant<QString, qulonglong>;

std::vector<Chunk> naturalChunks(const QString &text)
{
    static const QRegularExpression digits(QStringLiteral("(\\d+)"));
    std::vector<Chunk> chunks;
    qsizetype last = 0;
    auto it = digits.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        if (match.capturedStart() > last)
            chunks.emplace_back(text.mid(last, match.capturedStart() - last));
        chunks.emplace_back(match.captured().toULongLong());
        last = match.capturedEnd();
    }
    if (last < text.size())
        chunks.emplace_back(text.mid(last));
    return chunks;
}

} // namespace

bool Platform::sortLess(const PortInfo &a, const PortInfo &b)
{
    const bool aUsb = !a.vidPid().isEmpty();
    const bool bUsb = !b.vidPid().isEmpty();
    if (aUsb != bUsb)
        return aUsb;
    const auto left = naturalChunks(a.device);
    const auto right = naturalChunks(b.device);
    // std::variant orders by index first (text < number), then by value.
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
}

QString Platform::deviceName(const QString &portName, const QString &systemLocation) const
{
    return systemLocation.isEmpty() ? portName : systemLocation;
}

QString Platform::notFoundRemedy(const QString &port) const
{
    return QStringLiteral(
               "No serial port named '%1' was found.\n"
               "Run 'portside --list' to see the ports that are currently present, and check "
               "that the adapter is plugged in and its driver is loaded.")
        .arg(port);
}

QString Platform::vanishedRemedy(const QString &port) const
{
    return QStringLiteral(
               "The device behind %1 disappeared (unplugged, reset, or the driver dropped it).\n"
               "Re-plug it and connect again, or use --reconnect to have Portside wait for it.")
        .arg(port);
}

QString Platform::configDir()
{
    const QString overridden = qEnvironmentVariable("PORTSIDE_CONFIG_DIR");
    if (!overridden.isEmpty())
        return overridden;
#if defined(Q_OS_WIN)
    // Roaming %APPDATA%, so profiles follow the user between machines.
    const QString appData = qEnvironmentVariable("APPDATA");
    if (!appData.isEmpty())
        return QDir(appData).filePath(QStringLiteral("Portside"));
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
        .filePath(QStringLiteral("Portside"));
#elif defined(Q_OS_MACOS)
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("Portside")); // ~/Library/Application Support/Portside
#else
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
        .filePath(QStringLiteral("Portside")); // $XDG_CONFIG_HOME/Portside
#endif
}

QString Platform::logDir()
{
    const QString overridden = qEnvironmentVariable("PORTSIDE_LOG_DIR");
    if (!overridden.isEmpty())
        return overridden;
#if defined(Q_OS_WIN)
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    const QString base = local.isEmpty()
                             ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                             : local;
    return QDir(base).filePath(QStringLiteral("Portside/Logs"));
#elif defined(Q_OS_MACOS)
    return QDir::home().filePath(QStringLiteral("Library/Logs/Portside"));
#else
    QString state = qEnvironmentVariable("XDG_STATE_HOME");
    if (state.isEmpty())
        state = QDir::home().filePath(QStringLiteral(".local/state"));
    return QDir(state).filePath(QStringLiteral("Portside/log"));
#endif
}

QString currentPlatformName()
{
#if defined(Q_OS_LINUX)
    return QStringLiteral("linux");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows");
#else
    return QStringLiteral("posix");
#endif
}

std::unique_ptr<Platform> platformFor(const QString &name)
{
    if (name == QLatin1String("linux"))
        return std::make_unique<LinuxPlatform>();
    if (name == QLatin1String("macos"))
        return std::make_unique<MacOSPlatform>();
    if (name == QLatin1String("windows"))
        return std::make_unique<WindowsPlatform>();
    if (name == QLatin1String("posix"))
        return std::make_unique<PosixPlatform>();
    return nullptr;
}

const Platform &currentPlatform()
{
    static const std::unique_ptr<Platform> platform = platformFor(currentPlatformName());
    return *platform;
}

} // namespace portside
