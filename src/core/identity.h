#pragma once

// Stable device identity (PRD F2).
//
// A USB adapter shows up as /dev/ttyUSB0 today, /dev/ttyUSB1 after the next
// replug and COM7 on a colleague's laptop.  Profiles must follow the *device*,
// so they are keyed by VID:PID plus serial number.
//
// Adapters without a serial number (most CH340s, many PL2303 clones) get a
// `weak` identity derived from VID:PID and the port description: still useful
// for auto-applying a profile, but not unique if two identical adapters are
// plugged in at once, which is why the flag is exposed rather than hidden.

#include "core/ports.h"

#include <QMetaType>
#include <QString>

#include <optional>

namespace portside {

struct DeviceIdentity
{
    std::optional<quint16> vid;
    std::optional<quint16> pid;
    QString serialNumber;
    QString fallback;
    bool weak = false;

    static DeviceIdentity fromPort(const PortInfo &port);

    // Stable string key, safe to use as a TOML bare key.
    QString key() const;
    // True if `port` is (as far as can be told) this same device.
    bool matches(const PortInfo &port) const;
    // Human readable identity for status lines and logs.
    QString describe() const;

    bool operator==(const DeviceIdentity &) const = default;
};

} // namespace portside

Q_DECLARE_METATYPE(portside::DeviceIdentity)
