#include "core/discovery.h"

#include "core/chips.h"
#include "core/platform.h"

#include <QSerialPortInfo>

#include <algorithm>

namespace portside {

namespace {

// Drivers report unknown fields as empty or as placeholder words.
QString clean(const QString &value)
{
    const QString text = value.trimmed();
    const QString lowered = text.toLower();
    if (lowered == QLatin1String("n/a") || lowered == QLatin1String("none")
        || lowered == QLatin1String("unknown")) {
        return {};
    }
    return text;
}

} // namespace

QList<RawPort> systemPorts(const Platform &platform)
{
    QList<RawPort> ports;
    const auto available = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : available) {
        RawPort raw;
        raw.device = platform.deviceName(info.portName(), info.systemLocation());
        raw.description = info.description();
        raw.manufacturer = info.manufacturer();
        raw.serialNumber = info.serialNumber();
        if (info.hasVendorIdentifier())
            raw.vid = info.vendorIdentifier();
        if (info.hasProductIdentifier())
            raw.pid = info.productIdentifier();
        ports << raw;
    }
    return ports;
}

PortInfo portFromRaw(const RawPort &raw)
{
    PortInfo port;
    port.device = raw.device;
    port.description = clean(raw.description);
    port.vid = raw.vid;
    port.pid = raw.pid;
    port.serialNumber = clean(raw.serialNumber);
    port.manufacturer = clean(raw.manufacturer);
    port.product = clean(raw.product);
    port.chipFamily = chipFamily(raw.vid, raw.pid);
    const QString location = clean(raw.location);
    if (!location.isEmpty())
        port.extra.insert(QStringLiteral("location"), location);
    return port;
}

QList<PortInfo> listPorts(const PortLister &lister, const Platform *platform)
{
    const Platform &support = platform ? *platform : currentPlatform();
    const QList<RawPort> raw = lister ? lister() : systemPorts(support);
    QList<PortInfo> ports;
    for (const RawPort &entry : raw)
        ports << portFromRaw(entry);
    QList<PortInfo> prepared = support.preparePorts(ports);
    std::stable_sort(prepared.begin(), prepared.end(), &Platform::sortLess);
    return prepared;
}

std::optional<PortInfo> findPort(const QString &name, const PortLister &lister,
                                 const Platform *platform)
{
    for (const PortInfo &port : listPorts(lister, platform)) {
        if (port.device == name || (!port.stablePath.isEmpty() && port.stablePath == name))
            return port;
    }
    return std::nullopt;
}

std::optional<PortInfo> findPort(const DeviceIdentity &identity, const PortLister &lister,
                                 const Platform *platform)
{
    for (const PortInfo &port : listPorts(lister, platform)) {
        if (identity.matches(port))
            return port;
    }
    return std::nullopt;
}

} // namespace portside
