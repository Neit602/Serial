// Fallback backend for POSIX systems that are neither Linux nor macOS.

#include "core/platform.h"

namespace portside {

QString PosixPlatform::permissionRemedy(const QString &port) const
{
    return QStringLiteral(
               "You do not have permission to open %1.\n"
               "Serial device nodes are usually owned by a group such as 'dialout' or "
               "'uucp'; add your user to the group that owns the device (check with "
               "'ls -l %1') and start a new login session.")
        .arg(port);
}

QString PosixPlatform::busyRemedy(const QString &port) const
{
    return QStringLiteral(
               "%1 is open in another program.\n"
               "Find it with 'fuser -v %1' or 'lsof %1', close it, and try again.")
        .arg(port);
}

} // namespace portside
