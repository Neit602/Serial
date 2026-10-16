// `portside` entry point.

#include "cli/cli.h"
#include "core/version.h"

#include <QCoreApplication>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Portside"));
    QCoreApplication::setApplicationName(QStringLiteral("portside"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(portside::kVersion));
    return portside::cli::run(QCoreApplication::arguments().mid(1));
}
