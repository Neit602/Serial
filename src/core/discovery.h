#pragma once

// Serial port discovery (PRD F1, F2).
//
// QSerialPortInfo gives the cross-platform base (device, description, VID,
// PID, serial number); the platform backend then filters the list (macOS
// /dev/cu.* only) and enriches it (Linux stable path and bound driver).

#include "core/identity.h"
#include "core/ports.h"

#include <QList>
#include <QString>

#include <functional>
#include <optional>

namespace portside {

class Platform;

// One entry as the OS reports it, before cleaning.  Injected in tests so the
// discovery logic runs against synthetic port lists on any OS.
struct RawPort
{
    QString device;
    QString description;
    QString manufacturer;
    QString product;
    QString serialNumber;
    QString location;
    std::optional<quint16> vid;
    std::optional<quint16> pid;
};

using PortLister = std::function<QList<RawPort>()>;

// What the OS reports right now (QSerialPortInfo::availablePorts).
QList<RawPort> systemPorts(const Platform &platform);

// Convert one raw entry, dropping "n/a"-style placeholders.
PortInfo portFromRaw(const RawPort &raw);

// All serial ports the user may connect to, sorted for display.
QList<PortInfo> listPorts(const PortLister &lister = {}, const Platform *platform = nullptr);

// Find a port by device name or stable path.
std::optional<PortInfo> findPort(const QString &name, const PortLister &lister = {},
                                 const Platform *platform = nullptr);

// Find a port by device identity — what makes auto-reconnect survive a rename:
// the adapter that comes back as /dev/ttyUSB1 or COM9 is still recognised.
std::optional<PortInfo> findPort(const DeviceIdentity &identity, const PortLister &lister = {},
                                 const Platform *platform = nullptr);

} // namespace portside
