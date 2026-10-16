// Windows platform backend.
//
// Qt reports COM ports by name ("COM3") with a "\\.\COM3" system location;
// users type the short name, which QSerialPort also accepts above COM9.  What
// Windows users do need is a sharp explanation of error 5 (access denied,
// almost always another program) and of missing vendor drivers.

#include "core/platform.h"

namespace portside {

QString WindowsPlatform::deviceName(const QString &portName, const QString &systemLocation) const
{
    return portName.isEmpty() ? systemLocation : portName;
}

QString WindowsPlatform::permissionRemedy(const QString &port) const
{
    return QStringLiteral(
               "Windows denied access to %1 (error 5).\n"
               "On Windows this almost always means another program already has the port "
               "open — PuTTY, Tera Term, Arduino IDE, a vendor tool, or another Portside "
               "session.  Close it and try again.\n"
               "If nothing holds the port, open Device Manager and check the adapter under "
               "'Ports (COM & LPT)' for a warning triangle; a driver marked with one needs "
               "reinstalling (Windows Update supplies FTDI, CP210x and PL2303; CH34x "
               "usually needs the vendor driver).")
        .arg(port);
}

QString WindowsPlatform::busyRemedy(const QString &port) const
{
    return QStringLiteral(
               "%1 is already open in another program.\n"
               "Close the other terminal, IDE serial monitor or vendor tool, then try again.\n"
               "If the port was left behind by a crashed program, unplug and re-plug the "
               "adapter to force the driver to release it.")
        .arg(port);
}

QString WindowsPlatform::notFoundRemedy(const QString &port) const
{
    return QStringLiteral(
               "No serial port named '%1' was found.\n"
               "Run 'portside --list' to see the current ports, and check Device Manager "
               "under 'Ports (COM & LPT)'.  Note that ports above COM9 must be written as "
               "COM10, not \\\\.\\COM10, on the Portside command line.")
        .arg(port);
}

} // namespace portside
