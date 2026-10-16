#pragma once

// Building transports from a port name or a device identity.
//
// Keeping this out of the session lets the session stay transport-agnostic:
// it is handed a callable and never learns what a COM port is.

#include "core/discovery.h"
#include "core/identity.h"
#include "core/settings.h"
#include "core/transport.h"

#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace portside {

class Platform;

// Called (on the session's I/O thread) to obtain a fresh, unopened transport
// when the device comes back.
using Reopener = std::function<std::unique_ptr<Transport>()>;
using TransportFactory =
    std::function<std::unique_ptr<Transport>(const QString &device, const SerialSettings &)>;

// True for URL-style names such as loop://, which have no device identity.
bool isUrl(const QString &name);

// Resolve a user-supplied port name to a device name and its identity.
//
// Accepts a device name (COM3, /dev/ttyUSB0), a Linux stable path
// (/dev/serial/by-id/...) or a URL (loop://), which passes through untouched.
// Enumeration is advisory, not a gate: ports discovery does not report —
// Bluetooth RFCOMM nodes, PTYs, some on-board UARTs — are handed to the driver
// anyway, which then produces the real (typed) error if the port truly is not
// there.  Only ports found by discovery carry an identity (DECISIONS D5).
std::pair<QString, std::optional<DeviceIdentity>>
resolvePort(const QString &name, const PortLister &lister = {}, const Platform *platform = nullptr);

// Create (but do not open) a transport for `name`.  `loop://` gives an
// echoing in-memory port, handy without hardware.
std::unique_ptr<Transport> createTransport(const QString &name, const SerialSettings &settings = {},
                                           const Platform *platform = nullptr);

// A reopener that finds the device again *by identity*, not by port name, so
// auto-reconnect survives the adapter coming back as /dev/ttyUSB1 or COM9.
Reopener identityReopener(const DeviceIdentity &identity, const SerialSettings &settings,
                          const PortLister &lister = {}, const Platform *platform = nullptr,
                          TransportFactory factory = {});

} // namespace portside
