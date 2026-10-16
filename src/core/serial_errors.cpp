#include "core/serial_errors.h"

#include "core/platform.h"

#include <QStringList>

namespace portside {

namespace {

// OS wording, lowercased.  Checked before the error code because Qt folds
// "busy" and "permission denied" into the same code on Unix.
const QStringList kPermissionText = {
    QStringLiteral("permission denied"),
    QStringLiteral("access is denied"),
    QStringLiteral("access denied"),
};
const QStringList kBusyText = {
    QStringLiteral("resource busy"),
    QStringLiteral("device or resource busy"),
    QStringLiteral("resource temporarily unavailable"),
    QStringLiteral("sharing violation"),
    QStringLiteral("being used by another process"),
    QStringLiteral("in use by another"),
    QStringLiteral("already open"),
    // Qt's own lock-file check on Unix: another process holds the port.
    QStringLiteral("locking the device"),
};
const QStringList kNotFoundText = {
    QStringLiteral("no such file or directory"),
    QStringLiteral("could not find"),
    QStringLiteral("cannot find the file"),
    QStringLiteral("file not found"),
    QStringLiteral("cannot find the path"),
    QStringLiteral("port not found"),
};
const QStringList kVanishedText = {
    QStringLiteral("device not configured"),
    QStringLiteral("no such device"),
    QStringLiteral("device disconnected"),
    QStringLiteral("input/output error"),
    QStringLiteral("the device does not recognize the command"),
    QStringLiteral("the handle is invalid"),
};

bool containsAny(const QString &text, const QStringList &tokens)
{
    for (const QString &token : tokens) {
        if (text.contains(token))
            return true;
    }
    return false;
}

} // namespace

PortsideError mapSerialError(QSerialPort::SerialPortError code, const QString &text,
                             const QString &port, const Platform &platform, bool whileOpen)
{
    const auto permission = [&] {
        return PortsideError(ErrorKind::PermissionDenied,
                             QStringLiteral("permission denied opening %1").arg(port), port,
                             platform.permissionRemedy(port));
    };
    const auto busy = [&] {
        return PortsideError(ErrorKind::PortBusy, QStringLiteral("%1 is busy").arg(port), port,
                             platform.busyRemedy(port));
    };
    const auto missing = [&] {
        return PortsideError(ErrorKind::PortNotFound, QStringLiteral("no such port: %1").arg(port),
                             port, platform.notFoundRemedy(port));
    };
    const auto vanished = [&] {
        return PortsideError(ErrorKind::DeviceVanished, QStringLiteral("%1 disappeared").arg(port),
                             port, platform.vanishedRemedy(port));
    };

    const QString lowered = text.toLower();
    if (containsAny(lowered, kBusyText))
        return busy();
    if (containsAny(lowered, kPermissionText))
        return whileOpen ? busy() : permission();
    if (containsAny(lowered, kVanishedText))
        return vanished();
    if (containsAny(lowered, kNotFoundText))
        return whileOpen ? vanished() : missing();

    switch (code) {
    case QSerialPort::PermissionError:
        return whileOpen ? busy() : permission();
    case QSerialPort::OpenError:
        return busy();
    case QSerialPort::DeviceNotFoundError:
        return whileOpen ? vanished() : missing();
    case QSerialPort::ResourceError:
        return vanished();
    default:
        break;
    }
    const QString detail = text.isEmpty() ? QStringLiteral("serial port error %1").arg(int(code))
                                          : text;
    return PortsideError(ErrorKind::Transport, detail, port);
}

} // namespace portside
