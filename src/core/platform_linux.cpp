// Linux platform backend.
//
// Adds what Linux users actually need: the stable /dev/serial/by-id path, the
// kernel driver currently bound to the port, and a permission remedy naming
// the group that really owns the device node (dialout on Debian/Ubuntu, uucp
// on Arch and friends).

#include "core/platform.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#if defined(Q_OS_UNIX)
#include <grp.h>
#include <sys/stat.h>
#endif

namespace portside {

namespace {
// Used when the device node cannot be inspected (it may not exist yet).
const QString kDefaultGroup = QStringLiteral("dialout");
} // namespace

LinuxPlatform::LinuxPlatform(QString byIdDir, QString sysTtyDir)
    : m_byId(std::move(byIdDir))
    , m_sysTty(std::move(sysTtyDir))
{
}

QList<PortInfo> LinuxPlatform::filterPorts(const QList<PortInfo> &ports) const
{
    QList<PortInfo> kept;
    for (const PortInfo &port : ports) {
        const QString node = QFileInfo(port.device).fileName();
        if (node.startsWith(QLatin1String("ttyS"))) {
            QFile type(QDir(m_sysTty).filePath(node + QStringLiteral("/type")));
            if (type.open(QIODevice::ReadOnly) && type.readAll().trimmed() == "0")
                continue;
        }
        kept << port;
    }
    return kept;
}

PortInfo LinuxPlatform::enrichPort(const PortInfo &port) const
{
    PortInfo enriched = port;
    if (enriched.stablePath.isEmpty())
        enriched.stablePath = stablePath(port.device);
    if (enriched.driver.isEmpty())
        enriched.driver = driver(port.device);
    return enriched;
}

QString LinuxPlatform::stablePath(const QString &device) const
{
    const QDir dir(m_byId);
    if (!dir.exists())
        return {};
    const QString target = QFileInfo(device).canonicalFilePath();
    if (target.isEmpty())
        return {};
    const QStringList entries = dir.entryList(QDir::Files | QDir::System | QDir::NoDotAndDotDot,
                                              QDir::Name);
    for (const QString &entry : entries) {
        const QString path = dir.filePath(entry);
        if (QFileInfo(path).canonicalFilePath() == target)
            return path;
    }
    return {};
}

QString LinuxPlatform::driver(const QString &device) const
{
    const QString link = QDir(m_sysTty).filePath(QFileInfo(device).fileName()
                                                 + QStringLiteral("/device/driver"));
    const QFileInfo info(link);
    if (!info.isSymLink())
        return {};
    const QString name = QFileInfo(info.symLinkTarget()).fileName();
    if (name != QLatin1String("port"))
        return name;
    // Since Linux 6.5 an on-board UART's tty hangs off the serial core's
    // "port" and "ctrl" shims; the driver worth reporting is further up.
    QDir node(QFileInfo(QFileInfo(link).absolutePath()).canonicalFilePath());
    for (int level = 0; level < 3 && node.cdUp(); ++level) {
        const QFileInfo parent(node.filePath(QStringLiteral("driver")));
        const QString parentName = QFileInfo(parent.symLinkTarget()).fileName();
        if (parent.isSymLink() && parentName != QLatin1String("port")
            && parentName != QLatin1String("ctrl")) {
            return parentName;
        }
    }
    return name;
}

QString LinuxPlatform::deviceGroup(const QString &port) const
{
#if defined(Q_OS_UNIX)
    struct stat st {};
    if (::stat(QFile::encodeName(port).constData(), &st) != 0)
        return kDefaultGroup;
    const struct group *entry = ::getgrgid(st.st_gid);
    if (entry == nullptr || entry->gr_name == nullptr)
        return kDefaultGroup;
    return QString::fromLocal8Bit(entry->gr_name);
#else
    Q_UNUSED(port);
    return kDefaultGroup;
#endif
}

QString LinuxPlatform::permissionRemedy(const QString &port) const
{
    const QString group = deviceGroup(port);
    return QStringLiteral(
               "You are not a member of the group that owns %1.\n"
               "Add yourself to the '%2' group and start a new login session:\n\n"
               "    sudo usermod -aG %2 $USER\n\n"
               "Then log out and back in (or run 'newgrp %2' in this shell) and try again.\n"
               "Check the groups you are in with 'id -nG'.")
        .arg(port, group);
}

QString LinuxPlatform::busyRemedy(const QString &port) const
{
    return QStringLiteral(
               "%1 is open in another program.\n"
               "Find it with:\n\n"
               "    sudo fuser -v %1       # or: sudo lsof %1\n\n"
               "Common culprits are minicom, screen, picocom, ModemManager and another "
               "Portside window.  ModemManager can be told to leave the port alone with a "
               "udev rule (ENV{ID_MM_DEVICE_IGNORE}=\"1\").")
        .arg(port);
}

} // namespace portside
