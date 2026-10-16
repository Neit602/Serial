#include "core/factory.h"

#include "core/loopback_transport.h"
#include "core/platform.h"
#include "core/serial_transport.h"

namespace portside {

bool isUrl(const QString &name)
{
    return name.contains(QStringLiteral("://"));
}

std::pair<QString, std::optional<DeviceIdentity>>
resolvePort(const QString &name, const PortLister &lister, const Platform *platform)
{
    if (isUrl(name))
        return {name, std::nullopt};
    const auto port = findPort(name, lister, platform);
    if (!port)
        return {name, std::nullopt};
    return {port->device, DeviceIdentity::fromPort(*port)};
}

std::unique_ptr<Transport> createTransport(const QString &name, const SerialSettings &settings,
                                           const Platform *platform)
{
    if (name.startsWith(QStringLiteral("loop://"))) {
        LoopbackOptions options;
        options.name = name;
        return std::make_unique<LoopbackTransport>(options, settings);
    }
    if (isUrl(name)) {
        throw PortsideError(ErrorKind::PortNotFound,
                            QStringLiteral("unsupported URL scheme: %1").arg(name), name,
                            QStringLiteral("Portside understands device names (COM3, "
                                           "/dev/ttyUSB0) and loop:// for testing."));
    }
    return std::make_unique<SerialTransport>(name, settings, platform);
}

Reopener identityReopener(const DeviceIdentity &identity, const SerialSettings &settings,
                          const PortLister &lister, const Platform *platform,
                          TransportFactory factory)
{
    const Platform *support = platform ? platform : &currentPlatform();
    if (!factory) {
        factory = [support](const QString &device, const SerialSettings &cfg) {
            return createTransport(device, cfg, support);
        };
    }
    return [identity, settings, lister, support, factory]() -> std::unique_ptr<Transport> {
        const auto port = findPort(identity, lister, support);
        if (!port) {
            throw PortsideError(
                ErrorKind::PortNotFound,
                QStringLiteral("device %1 is not present").arg(identity.describe()), {},
                QStringLiteral("Re-plug the adapter; Portside is waiting for it to come back."));
        }
        return factory(port->device, settings);
    };
}

} // namespace portside
