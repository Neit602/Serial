#pragma once

// The interface every platform backend implements.
//
// One class per operating system, all with the same surface, so the rest of
// Portside never branches on the OS.  Every backend is compiled on every OS so
// the tests can exercise Windows logic on Linux and vice versa; only the few
// calls that need an OS API are guarded inside the backends.

#include "core/ports.h"

#include <QList>
#include <QString>

#include <memory>

namespace portside {

class Platform
{
public:
    virtual ~Platform() = default;

    // Short identifier used in messages and tests ("linux", "macos", ...).
    virtual QString name() const = 0;

    // -- discovery ---------------------------------------------------------

    // Drop ports the user should never be offered.  Default: keep all.
    virtual QList<PortInfo> filterPorts(const QList<PortInfo> &ports) const { return ports; }
    // Add platform-only details (driver in use, stable path).  Default: none.
    virtual PortInfo enrichPort(const PortInfo &port) const { return port; }
    // Filter then enrich, the order discovery always wants.
    QList<PortInfo> preparePorts(const QList<PortInfo> &ports) const;

    // USB adapters first, then the rest; each in natural order, so COM9 sorts
    // before COM10 and ttyS2 before ttyS10.
    static bool sortLess(const PortInfo &a, const PortInfo &b);

    // The name users type for a port Qt enumerated: "COM3" on Windows, the
    // device node elsewhere.
    virtual QString deviceName(const QString &portName, const QString &systemLocation) const;

    // -- diagnostics (F6) --------------------------------------------------

    virtual QString permissionRemedy(const QString &port) const = 0;
    virtual QString busyRemedy(const QString &port) const = 0;
    virtual QString notFoundRemedy(const QString &port) const;
    virtual QString vanishedRemedy(const QString &port) const;

    // -- paths -------------------------------------------------------------

    // OS-correct configuration directory; PORTSIDE_CONFIG_DIR overrides it.
    static QString configDir();
    // OS-correct directory for session logs; PORTSIDE_LOG_DIR overrides it.
    static QString logDir();
};

class LinuxPlatform : public Platform
{
public:
    // Injectable so the tests can run this backend on any OS.
    explicit LinuxPlatform(QString byIdDir = QStringLiteral("/dev/serial/by-id"),
                           QString sysTtyDir = QStringLiteral("/sys/class/tty"));

    QString name() const override { return QStringLiteral("linux"); }
    // Hide the legacy ttyS* nodes the 8250 driver registers for UARTs that are
    // not there (sysfs reports their type as 0, "unknown").
    QList<PortInfo> filterPorts(const QList<PortInfo> &ports) const override;
    PortInfo enrichPort(const PortInfo &port) const override;
    QString permissionRemedy(const QString &port) const override;
    QString busyRemedy(const QString &port) const override;

    // Group that owns the device node, or "dialout" if it can't be read.
    QString deviceGroup(const QString &port) const;

private:
    QString stablePath(const QString &device) const;
    QString driver(const QString &device) const;

    QString m_byId;
    QString m_sysTty;
};

class MacOSPlatform : public Platform
{
public:
    QString name() const override { return QStringLiteral("macos"); }
    // Keep callout devices; drop the dial-in twins (they block on DCD).
    QList<PortInfo> filterPorts(const QList<PortInfo> &ports) const override;
    QString permissionRemedy(const QString &port) const override;
    QString busyRemedy(const QString &port) const override;
};

class WindowsPlatform : public Platform
{
public:
    QString name() const override { return QStringLiteral("windows"); }
    QString deviceName(const QString &portName, const QString &systemLocation) const override;
    QString permissionRemedy(const QString &port) const override;
    QString busyRemedy(const QString &port) const override;
    QString notFoundRemedy(const QString &port) const override;
};

// The BSDs and anything else POSIX: usable without pretending to know their
// sysfs equivalents.
class PosixPlatform : public Platform
{
public:
    QString name() const override { return QStringLiteral("posix"); }
    QString permissionRemedy(const QString &port) const override;
    QString busyRemedy(const QString &port) const override;
};

// "linux", "macos", "windows" or "posix" for the OS Portside was built for.
QString currentPlatformName();
// Construct the backend called `name`; nullptr for an unknown name.
std::unique_ptr<Platform> platformFor(const QString &name);
// The backend for the OS Portside is running on.
const Platform &currentPlatform();

} // namespace portside
