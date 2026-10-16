#include "core/identity.h"

#include <QRegularExpression>

namespace portside {

namespace {

// Reduce `value` to characters that are safe in a TOML bare key.
QString slug(const QString &value)
{
    static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9_.-]+"));
    QString cleaned = value;
    cleaned.replace(unsafe, QStringLiteral("_"));
    while (cleaned.startsWith(QLatin1Char('_')))
        cleaned.remove(0, 1);
    while (cleaned.endsWith(QLatin1Char('_')))
        cleaned.chop(1);
    return cleaned.isEmpty() ? QStringLiteral("unknown") : cleaned;
}

QString hex4(quint16 value)
{
    return QStringLiteral("%1").arg(value, 4, 16, QLatin1Char('0'));
}

} // namespace

DeviceIdentity DeviceIdentity::fromPort(const PortInfo &port)
{
    if (port.vid && port.pid && !port.serialNumber.isEmpty())
        return {port.vid, port.pid, port.serialNumber, {}, false};
    if (port.vid && port.pid)
        return {port.vid, port.pid, {}, port.description, true};
    // Non-USB port: nothing but the port name identifies it.
    return {std::nullopt, std::nullopt, {}, port.device, true};
}

QString DeviceIdentity::key() const
{
    if (!vid || !pid)
        return QStringLiteral("port_") + slug(fallback.isEmpty() ? QStringLiteral("unknown") : fallback);
    const QString vidPid = hex4(*vid) + QLatin1Char('_') + hex4(*pid);
    if (!serialNumber.isEmpty())
        return QStringLiteral("usb_%1_%2").arg(vidPid, slug(serialNumber));
    if (!fallback.isEmpty())
        return QStringLiteral("usb_%1_desc_%2").arg(vidPid, slug(fallback));
    return QStringLiteral("usb_") + vidPid;
}

bool DeviceIdentity::matches(const PortInfo &port) const
{
    return fromPort(port) == *this;
}

QString DeviceIdentity::describe() const
{
    if (!vid || !pid)
        return QStringLiteral("port ") + (fallback.isEmpty() ? QStringLiteral("unknown") : fallback);
    const QString vidPid = hex4(*vid) + QLatin1Char(':') + hex4(*pid);
    if (!serialNumber.isEmpty())
        return QStringLiteral("USB %1 serial %2").arg(vidPid, serialNumber);
    return QStringLiteral("USB %1 (no serial number)").arg(vidPid);
}

} // namespace portside
