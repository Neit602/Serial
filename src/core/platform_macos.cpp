// macOS platform backend.
//
// The important rule here is the PRD's: list the /dev/cu.* callout devices and
// hide the matching /dev/tty.* dial-in devices, which block on open until DCD
// is asserted and so appear to hang.

#include "core/platform.h"

namespace portside {

namespace {
const QString kDialInPrefix = QStringLiteral("/dev/tty.");
} // namespace

QList<PortInfo> MacOSPlatform::filterPorts(const QList<PortInfo> &ports) const
{
    QList<PortInfo> kept;
    for (const PortInfo &port : ports) {
        if (!port.device.startsWith(kDialInPrefix))
            kept << port;
    }
    return kept;
}

QString MacOSPlatform::permissionRemedy(const QString &port) const
{
    return QStringLiteral(
               "macOS refused access to %1.\n"
               "This is unusual on macOS; the likely causes are:\n"
               "  * another program (screen, minicom, an IDE's serial monitor) holds the port;\n"
               "  * the adapter's vendor driver and Apple's built-in driver both claim the "
               "device — remove the vendor kext/driver extension and re-plug;\n"
               "  * the terminal app lacks permission to access a USB accessory: check "
               "System Settings → Privacy & Security.")
        .arg(port);
}

QString MacOSPlatform::busyRemedy(const QString &port) const
{
    return QStringLiteral(
               "%1 is open in another program.\n"
               "Find it with:\n\n"
               "    lsof %1\n\n"
               "Quit that program (or close the other Portside session) and try again.\n"
               "If you opened the matching /dev/tty.* device by mistake, use the /dev/cu.* "
               "name instead — Portside only lists /dev/cu.*.")
        .arg(port);
}

} // namespace portside
