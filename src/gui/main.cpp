// `portside-gui` entry point.

#include "core/version.h"
#include "gui/main_window.h"

#include <QApplication>

int main(int argc, char **argv)
{
    QApplication::setOrganizationName(portside::gui::kOrganisation);
    QApplication::setApplicationName(portside::gui::kApplication);
    QApplication::setApplicationVersion(QString::fromLatin1(portside::kVersion));
    QApplication app(argc, argv);
    portside::gui::MainWindow window;
    window.show();
    return QApplication::exec();
}
