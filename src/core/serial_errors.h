#pragma once

// Translate driver errors into Portside's typed errors (PRD F6).
//
// QSerialPort reports failures as an error code plus the OS's own wording,
// and it folds several distinct OS errors together (EBUSY and EACCES both
// arrive as PermissionError on Unix).  This module is the single place that
// understands that wording, so everything above it only ever sees
// PermissionDenied, PortBusy, PortNotFound or DeviceVanished — each already
// carrying the platform's fix.

#include "core/errors.h"

#include <QSerialPort>
#include <QString>

namespace portside {

class Platform;

// Map one driver failure to a typed error.
//
// `whileOpen` is true when the failure happened during I/O on an already open
// port, where "no such device" means the device was unplugged rather than
// never present.
PortsideError mapSerialError(QSerialPort::SerialPortError code, const QString &text,
                             const QString &port, const Platform &platform, bool whileOpen);

} // namespace portside
