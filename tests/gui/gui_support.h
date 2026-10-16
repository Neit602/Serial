#pragma once

// Fixtures for the GUI tests: scripted ports, loopback transports, and a
// window wired to both, with dialogs turned off.

#include "core/discovery.h"
#include "core/loopback_transport.h"
#include "gui/main_window.h"
#include "gui/session_controller.h"
#include "gui/session_view.h"

#include <QDir>
#include <QPointer>
#include <QTemporaryDir>

#include <memory>

namespace portside::test {

inline RawPort ftdi(const QString &device = QStringLiteral("/dev/ttyUSB0"))
{
    RawPort raw;
    raw.device = device;
    raw.description = QStringLiteral("FT232R USB UART");
    raw.vid = 0x0403;
    raw.pid = 0x6001;
    raw.serialNumber = QStringLiteral("A50285BI");
    return raw;
}

inline RawPort uart(const QString &device = QStringLiteral("/dev/ttyS0"))
{
    RawPort raw;
    raw.device = device;
    return raw;
}

// The ports a test says are plugged in; change `ports` to simulate hot-plug.
struct ScriptedPorts
{
    QList<RawPort> ports;
    PortLister lister()
    {
        return [this] { return ports; };
    }
};

// Builds loopback transports and remembers them, so tests can look at what
// each "device" was sent.
struct LoopbackFactory
{
    QList<QPointer<LoopbackTransport>> made;
    LoopbackOptions options;
    // When set, opening any port fails with it.
    std::optional<PortsideError> failWith;

    TransportFactory factory()
    {
        return [this](const QString &device, const SerialSettings &settings) -> std::unique_ptr<Transport> {
            LoopbackOptions chosen = options;
            chosen.name = device;
            chosen.failOpenWith = failWith;
            auto transport = std::make_unique<LoopbackTransport>(chosen, settings);
            made << transport.get();
            return transport;
        };
    }

    LoopbackTransport *last() const { return made.isEmpty() ? nullptr : made.last().data(); }
};

struct WindowFixture
{
    QTemporaryDir dir;
    ScriptedPorts ports;
    LoopbackFactory transports;

    WindowFixture() { ports.ports = {ftdi(), uart()}; }

    gui::MainWindowOptions options()
    {
        gui::MainWindowOptions options;
        options.profileDirectory = dir.filePath(QStringLiteral("profiles"));
        options.lister = ports.lister();
        options.transportFactory = transports.factory();
        options.settingsFile = dir.filePath(QStringLiteral("settings.ini"));
        options.portPollMs = 50;
        return options;
    }

    std::unique_ptr<gui::MainWindow> window()
    {
        auto made = std::make_unique<gui::MainWindow>(options());
        made->setInteractive(false);
        // A real size, so terminals are as wide as a user's would be.
        made->resize(1100, 700);
        made->show();
        return made;
    }
};

// A session view over a loopback, outside any window.
struct ViewFixture
{
    LoopbackTransport *transport = nullptr;
    std::unique_ptr<gui::SessionView> view;

    explicit ViewFixture(LoopbackOptions options = {}, Profile profile = {})
    {
        options.echo = false;
        auto made = std::make_unique<LoopbackTransport>(options);
        transport = made.get();
        auto session = std::make_unique<Session>(std::move(made));
        session->open();
        if (profile.name.isEmpty())
            profile.name = QStringLiteral("test");
        auto *controller = new gui::SessionController(std::move(session), profile);
        view = std::make_unique<gui::SessionView>(controller);
        view->resize(800, 480);
        view->show();
    }

    gui::SessionController *controller() const { return view->controller(); }
};

} // namespace portside::test
