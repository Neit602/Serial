#include "gui/main_window.h"

#include "core/version.h"
#include "gui/device_list.h"
#include "gui/profiles_dialog.h"
#include "gui/session_controller.h"
#include "gui/session_view.h"
#include "gui/settings_dialog.h"
#include "gui/styles.h"
#include "gui/terminal_widget.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>

namespace portside::gui {

MainWindow::MainWindow(MainWindowOptions options, QWidget *parent)
    : QMainWindow(parent)
    , m_options(std::move(options))
    , m_store(m_options.profileDirectory)
{
    setWindowTitle(QStringLiteral("Portside"));
    if (!m_options.transportFactory) {
        m_options.transportFactory = [](const QString &device, const SerialSettings &settings) {
            return createTransport(device, settings);
        };
    }
    m_settings = m_options.settingsFile.isEmpty()
                     ? new QSettings(kOrganisation, kApplication, this)
                     : new QSettings(m_options.settingsFile, QSettings::IniFormat, this);
    m_theme = themeFromName(m_settings->value(QStringLiteral("theme"), QStringLiteral("dark")).toString());

    m_tabs = new QTabWidget(this);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    m_tabs->setAccessibleName(tr("Sessions"));
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] {
        updateActions();
        if (SessionView *view = currentView())
            statusBar()->showMessage(view->statusText());
    });
    setCentralWidget(m_tabs);

    m_deviceList = new DeviceList(m_options.lister, m_options.portPollMs, this);
    connect(m_deviceList, &DeviceList::portActivated, this,
            [this](const PortInfo &port) { connectToPort(port); });
    connect(m_deviceList, &DeviceList::portSelected, this, &MainWindow::updateActions);
    m_devicesDock = new QDockWidget(tr("Devices"), this);
    m_devicesDock->setObjectName(QStringLiteral("devices"));
    m_devicesDock->setWidget(m_deviceList);
    m_devicesDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::LeftDockWidgetArea, m_devicesDock);

    buildActions();
    buildMenus();
    buildToolbar();
    statusBar()->showMessage(tr("Ready. Pick a device and press Connect."));
    setAcceptDrops(true);
    applyTheme(m_theme);
    restoreGeometryState();
    updateActions();
}

MainWindow::~MainWindow()
{
    // Children are destroyed after this body runs, and some of them signal on
    // the way out (selection and tab changes); nothing may reach this window
    // by then.
    m_deviceList->disconnect(this);
    m_tabs->disconnect(this);
    for (SessionView *view : views())
        view->disconnect(this);
}

// -- actions and menus ----------------------------------------------------------

QAction *MainWindow::makeAction(const QString &name, const QString &text, const QString &shortcut,
                                void (MainWindow::*slot)(), const QString &tip)
{
    auto *action = new QAction(text, this);
    action->setObjectName(name);
    if (slot != nullptr)
        connect(action, &QAction::triggered, this, slot);
    if (!shortcut.isEmpty())
        action->setShortcut(QKeySequence(shortcut));
    const QString plain = QString(text).remove(QLatin1Char('&')).remove(QStringLiteral("…"));
    action->setToolTip(tip.isEmpty() ? plain : tip);
    // Every control needs a name a screen reader can read out.
    action->setIconText(plain);
    m_actions.insert(name, action);
    return action;
}

void MainWindow::buildActions()
{
    makeAction(QStringLiteral("connect"), tr("&Connect"), QStringLiteral("Ctrl+N"), nullptr,
               tr("Open a session for the selected device"));
    connect(m_actions.value(QStringLiteral("connect")), &QAction::triggered, this,
            [this] { connectSelected(); });
    makeAction(QStringLiteral("connect-with-settings"), tr("Connect &with settings…"),
               QStringLiteral("Ctrl+Shift+N"), nullptr);
    connect(m_actions.value(QStringLiteral("connect-with-settings")), &QAction::triggered, this,
            [this] { connectWithSettings(); });
    makeAction(QStringLiteral("disconnect"), tr("&Disconnect"), QStringLiteral("Ctrl+W"),
               &MainWindow::closeCurrentTab);
    makeAction(QStringLiteral("new-window"), tr("New &Window"), QStringLiteral("Ctrl+Shift+W"), nullptr);
    connect(m_actions.value(QStringLiteral("new-window")), &QAction::triggered, this,
            [this] { openNewWindow(); });
    makeAction(QStringLiteral("detach"), tr("&Detach tab to window"), {}, nullptr);
    connect(m_actions.value(QStringLiteral("detach")), &QAction::triggered, this,
            [this] { detachCurrentTab(); });
    makeAction(QStringLiteral("settings"), tr("&Session settings…"), QStringLiteral("Ctrl+,"),
               &MainWindow::editSessionSettings);
    makeAction(QStringLiteral("profiles"), tr("&Profiles…"), QStringLiteral("Ctrl+P"),
               &MainWindow::openProfiles);
    makeAction(QStringLiteral("save-profile"), tr("Save session as profile…"), {}, nullptr);
    connect(m_actions.value(QStringLiteral("save-profile")), &QAction::triggered, this,
            [this] { saveSessionAsProfile(); });
    makeAction(QStringLiteral("copy"), tr("&Copy"), QStringLiteral("Ctrl+Shift+C"), nullptr);
    connect(m_actions.value(QStringLiteral("copy")), &QAction::triggered, this, [this] { copySelection(); });
    makeAction(QStringLiteral("paste-file"), tr("Paste &file…"), QStringLiteral("Ctrl+Shift+V"), nullptr);
    connect(m_actions.value(QStringLiteral("paste-file")), &QAction::triggered, this, [this] { pasteFile(); });
    makeAction(QStringLiteral("find"), tr("&Find…"), QStringLiteral("Ctrl+F"), &MainWindow::openFind);
    makeAction(QStringLiteral("break"), tr("Send &break"), QStringLiteral("Ctrl+B"), &MainWindow::sendBreak);
    makeAction(QStringLiteral("log"), tr("&Logging"), QStringLiteral("Ctrl+L"), &MainWindow::toggleLogging)
        ->setCheckable(true);
    makeAction(QStringLiteral("hex"), tr("&Hex view"), QStringLiteral("Ctrl+H"), &MainWindow::toggleHex)
        ->setCheckable(true);
    makeAction(QStringLiteral("clear"), tr("C&lear"), QStringLiteral("Ctrl+K"), &MainWindow::clearTerminal);
    makeAction(QStringLiteral("quit"), tr("&Quit"), QStringLiteral("Ctrl+Q"), nullptr);
    connect(m_actions.value(QStringLiteral("quit")), &QAction::triggered, this, &QWidget::close);
    makeAction(QStringLiteral("shortcuts"), tr("&Shortcuts"), QStringLiteral("F1"), &MainWindow::showShortcuts);
    makeAction(QStringLiteral("about"), tr("&About Portside"), {}, &MainWindow::showAbout);

    const std::pair<ThemeName, QString> themes[] = {{ThemeName::Dark, tr("&Dark")},
                                                    {ThemeName::Light, tr("&Light")},
                                                    {ThemeName::HighContrast, tr("&High contrast")}};
    for (const auto &[name, label] : themes) {
        QAction *action = makeAction(QStringLiteral("theme-") + enumName(name), label, {}, nullptr);
        action->setCheckable(true);
        const ThemeName chosen = name;
        connect(action, &QAction::triggered, this, [this, chosen] { applyTheme(chosen); });
        m_themeActions.insert(name, action);
    }
}

void MainWindow::buildMenus()
{
    const auto add = [this](QMenu *menu, const QStringList &names) {
        for (const QString &name : names) {
            if (name.isEmpty())
                menu->addSeparator();
            else
                menu->addAction(m_actions.value(name));
        }
    };
    add(menuBar()->addMenu(tr("&File")),
        {QStringLiteral("connect"), QStringLiteral("connect-with-settings"), QStringLiteral("disconnect"),
         QString(), QStringLiteral("new-window"), QStringLiteral("detach"), QString(),
         QStringLiteral("quit")});
    add(menuBar()->addMenu(tr("&Edit")),
        {QStringLiteral("copy"), QStringLiteral("paste-file"), QStringLiteral("find"), QString(),
         QStringLiteral("settings"), QStringLiteral("profiles"), QStringLiteral("save-profile")});
    add(menuBar()->addMenu(tr("&Session")),
        {QStringLiteral("break"), QStringLiteral("log"), QStringLiteral("hex"), QStringLiteral("clear")});
    QMenu *view = menuBar()->addMenu(tr("&View"));
    QMenu *themes = view->addMenu(tr("&Theme"));
    for (const ThemeName name : {ThemeName::Dark, ThemeName::Light, ThemeName::HighContrast})
        themes->addAction(m_themeActions.value(name));
    view->addAction(m_devicesDock->toggleViewAction());
    add(menuBar()->addMenu(tr("&Help")), {QStringLiteral("shortcuts"), QStringLiteral("about")});
}

void MainWindow::buildToolbar()
{
    m_toolbar = addToolBar(tr("Main"));
    m_toolbar->setObjectName(QStringLiteral("main-toolbar"));
    m_toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    for (const char *name : {"connect", "disconnect", "break", "paste-file", "log", "hex", "find"})
        m_toolbar->addAction(m_actions.value(QString::fromLatin1(name)));
}

// -- sessions -----------------------------------------------------------------------

SessionView *MainWindow::currentView() const
{
    return qobject_cast<SessionView *>(m_tabs->currentWidget());
}

QList<SessionView *> MainWindow::views() const
{
    QList<SessionView *> found;
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto *view = qobject_cast<SessionView *>(m_tabs->widget(i)))
            found << view;
    }
    return found;
}

SessionView *MainWindow::connectSelected()
{
    const auto port = m_deviceList->selectedPort();
    if (!port) {
        notify(tr("Select a device in the list first."));
        return nullptr;
    }
    return connectToPort(*port);
}

SessionView *MainWindow::connectToPort(const PortInfo &port, const std::optional<Profile> &profile)
{
    // The device list already holds everything discovery knows about this
    // port, so the identity comes from there: re-resolving would cost a
    // second scan and lose the identity of unlisted ports (DECISIONS D32).
    const DeviceIdentity identity = DeviceIdentity::fromPort(port);
    Profile chosen;
    if (profile) {
        chosen = *profile;
    } else if (auto bound = m_store.forDevice(identity)) {
        chosen = *bound;
    } else {
        chosen.name = port.device;
    }

    std::unique_ptr<Session> session;
    try {
        SessionOptions options;
        options.settings = chosen.serial;
        options.identity = identity;
        options.breakMode = chosen.breakMode;
        // Reconnect by identity, so the adapter is found again even if it
        // comes back under a different name (F2 + F5).
        options.reconnect.enabled = true;
        options.reopen = identityReopener(identity, chosen.serial, m_options.lister, nullptr,
                                          m_options.transportFactory);
        session = std::make_unique<Session>(m_options.transportFactory(port.device, chosen.serial), options);
        session->open();
    } catch (const PortsideError &error) {
        showError(error);
        return nullptr;
    }

    auto logger = std::make_unique<SessionLogger>(chosen.logging, port.device, chosen.name);
    auto *controller = new SessionController(std::move(session), chosen, std::move(logger));
    auto *view = new SessionView(controller, m_theme, m_tabs);
    if (chosen.logging.autoStart) {
        try {
            controller->startLogging();
        } catch (const PortsideError &error) {
            showError(error);
        }
    }
    attachView(view);
    const int index = m_tabs->addTab(view, tabTitle(port.device, chosen));
    m_tabs->setCurrentIndex(index);
    view->view()->setFocus();
    updateActions();
    Q_EMIT sessionOpened(view);
    return view;
}

QString MainWindow::tabTitle(const QString &device, const Profile &profile)
{
    const QString name = QFileInfo(device).fileName();
    return QStringLiteral("%1 — %2").arg(name.isEmpty() ? device : name).arg(profile.serial.baudrate);
}

SessionView *MainWindow::connectWithSettings()
{
    const auto port = m_deviceList->selectedPort();
    if (!port) {
        notify(tr("Select a device in the list first."));
        return nullptr;
    }
    Profile base;
    if (auto bound = m_store.forDevice(DeviceIdentity::fromPort(*port)))
        base = *bound;
    else
        base.name = port->device;
    SettingsDialog dialog(base, this);
    if (dialog.exec() != QDialog::Accepted)
        return nullptr;
    return connectToPort(*port, dialog.toProfile());
}

void MainWindow::attachView(SessionView *view)
{
    connect(view, &SessionView::statusChanged, this, &MainWindow::onStatus);
    connect(view, &SessionView::errorRaised, this, &MainWindow::showError);
}

void MainWindow::closeTab(int index)
{
    auto *view = qobject_cast<SessionView *>(m_tabs->widget(index));
    if (view == nullptr)
        return;
    if (view->controller()->isConnected()
        && !confirm(tr("Close session"), tr("%1 is still connected. Close it?").arg(view->controller()->port()))) {
        return;
    }
    m_tabs->removeTab(index);
    view->closeSession();
    view->deleteLater();
    updateActions();
}

void MainWindow::closeCurrentTab()
{
    if (m_tabs->currentIndex() >= 0)
        closeTab(m_tabs->currentIndex());
}

MainWindow *MainWindow::detachCurrentTab()
{
    SessionView *view = currentView();
    if (view == nullptr)
        return nullptr;
    const int index = m_tabs->indexOf(view);
    const QString title = m_tabs->tabText(index);
    m_tabs->removeTab(index);
    view->disconnect(this);
    auto *window = new MainWindow(m_options);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setInteractive(m_interactive, m_answer);
    window->adopt(view, title);
    window->show();
    updateActions();
    return window;
}

void MainWindow::adopt(SessionView *view, const QString &title)
{
    view->setParent(m_tabs);
    view->setTheme(m_theme);
    attachView(view);
    const int index = m_tabs->addTab(view, title);
    m_tabs->setCurrentIndex(index);
    updateActions();
}

MainWindow *MainWindow::openNewWindow()
{
    auto *window = new MainWindow(m_options);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setInteractive(m_interactive, m_answer);
    window->show();
    return window;
}

// -- session actions ----------------------------------------------------------------

void MainWindow::sendBreak()
{
    SessionView *view = currentView();
    if (view == nullptr)
        return;
    view->controller()->sendBreak().then(this, [this](QFuture<BreakResult> done) {
        try {
            notify(done.result().describe());
        } catch (const PortsideError &error) {
            showError(error);
        } catch (const std::exception &) {
        }
    });
}

void MainWindow::toggleLogging()
{
    SessionView *view = currentView();
    if (view == nullptr)
        return;
    if (view->controller()->isLogging()) {
        view->controller()->stopLogging();
        notify(tr("Logging stopped."));
    } else {
        try {
            const QString path = view->controller()->startLogging();
            notify(tr("Logging to %1").arg(QDir::toNativeSeparators(path)));
        } catch (const PortsideError &error) {
            showError(error);
        }
    }
    updateActions();
}

void MainWindow::toggleHex()
{
    if (SessionView *view = currentView()) {
        view->toggleHex();
        updateActions();
    }
}

void MainWindow::clearTerminal()
{
    if (SessionView *view = currentView())
        view->clear();
}

QString MainWindow::copySelection()
{
    SessionView *view = currentView();
    return view != nullptr ? view->copySelection() : QString();
}

void MainWindow::openFind()
{
    if (SessionView *view = currentView())
        view->openFind();
}

void MainWindow::pasteFile(const QString &path)
{
    SessionView *view = currentView();
    if (view == nullptr)
        return;
    QString target = path;
    if (target.isEmpty()) {
        target = QFileDialog::getOpenFileName(this, tr("Paste file"));
        if (target.isEmpty())
            return;
    }
    view->pasteFile(target);
}

void MainWindow::editSessionSettings()
{
    SessionView *view = currentView();
    if (view == nullptr)
        return;
    SettingsDialog dialog(view->controller()->profile(), this);
    if (dialog.exec() == QDialog::Accepted)
        applySessionProfile(dialog.toProfile());
}

void MainWindow::applySessionProfile(const Profile &profile)
{
    SessionView *view = currentView();
    if (view == nullptr)
        return;
    SessionController *controller = view->controller();
    controller->setProfile(profile);
    try {
        controller->session()->applySettings(profile.serial).then(this, [this](QFuture<void> done) {
            try {
                done.waitForFinished();
            } catch (const PortsideError &error) {
                showError(error);
            } catch (const std::exception &) {
            }
        });
    } catch (const PortsideError &error) {
        showError(error);
        return;
    }
    view->applyTerminalOptions();
    notify(tr("Settings applied."));
}

void MainWindow::openProfiles()
{
    SessionView *view = currentView();
    ProfilesDialog dialog(m_store, view != nullptr ? view->controller()->identity() : std::nullopt, this);
    dialog.exec();
}

std::optional<Profile> MainWindow::saveSessionAsProfile(const QString &name)
{
    SessionView *view = currentView();
    if (view == nullptr)
        return std::nullopt;
    QString chosen = name;
    if (chosen.isEmpty()) {
        bool ok = false;
        chosen = QInputDialog::getText(this, tr("Save profile"), tr("Profile name:"), QLineEdit::Normal,
                                       view->controller()->port(), &ok);
        if (!ok)
            return std::nullopt;
    }
    chosen = chosen.trimmed();
    if (chosen.isEmpty())
        return std::nullopt;
    Profile profile = view->controller()->profile();
    profile.name = chosen;
    profile.serial = view->controller()->settings();
    if (const auto identity = view->controller()->identity())
        profile = profile.withDevice(*identity);
    try {
        m_store.save(profile);
    } catch (const PortsideError &error) {
        showError(error);
        return std::nullopt;
    }
    view->controller()->setProfile(profile);
    notify(tr("Saved profile %1").arg(chosen));
    return profile;
}

// -- appearance ---------------------------------------------------------------------

void MainWindow::applyTheme(ThemeName name)
{
    m_theme = name;
    if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance()))
        app->setStyleSheet(stylesheet(name));
    for (SessionView *view : views())
        view->setTheme(name);
    for (auto it = m_themeActions.constBegin(); it != m_themeActions.constEnd(); ++it)
        it.value()->setChecked(it.key() == name);
    m_settings->setValue(QStringLiteral("theme"), enumName(name));
}

// -- errors and status -----------------------------------------------------------------

QMessageBox *MainWindow::buildErrorBox(const PortsideError &error)
{
    auto *box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setIcon(QMessageBox::Warning);
    box->setWindowTitle(tr("Serial port problem"));
    box->setText(error.text());
    if (!error.remedy().isEmpty()) {
        box->setInformativeText(error.remedy());
        QPushButton *copy = box->addButton(tr("Copy fix"), QMessageBox::ActionRole);
        const QString remedy = error.remedy();
        connect(copy, &QPushButton::clicked, box, [remedy] { copyRemedy(remedy); });
    }
    box->addButton(QMessageBox::Close);
    return box;
}

void MainWindow::showError(const PortsideError &error)
{
    m_lastMessage = error.display();
    if (!m_interactive)
        return;
    buildErrorBox(error)->exec();
}

void MainWindow::copyRemedy(const QString &remedy)
{
    QClipboard *clipboard = QApplication::clipboard();
    if (clipboard == nullptr)
        return;
    // The command lines are the indented lines of the remedy text.
    QStringList commands;
    for (const QString &line : remedy.split(QLatin1Char('\n'))) {
        if (line.startsWith(QStringLiteral("    ")))
            commands << line.trimmed();
    }
    clipboard->setText(commands.isEmpty() ? remedy : commands.join(QLatin1Char('\n')));
}

void MainWindow::setInteractive(bool interactive, bool answer)
{
    m_interactive = interactive;
    m_answer = answer;
}

bool MainWindow::confirm(const QString &title, const QString &question)
{
    if (!m_interactive)
        return m_answer;
    return QMessageBox::question(this, title, question) == QMessageBox::Yes;
}

void MainWindow::notify(const QString &message)
{
    m_lastMessage = message;
    if (!message.isEmpty())
        statusBar()->showMessage(message, 8000);
}

void MainWindow::onStatus(const QString &text)
{
    if (sender() == currentView())
        statusBar()->showMessage(text);
}

void MainWindow::updateActions()
{
    SessionView *view = currentView();
    const bool active = view != nullptr;
    for (const char *name : {"disconnect", "detach", "settings", "save-profile", "copy", "paste-file",
                             "find", "break", "log", "hex", "clear"}) {
        m_actions.value(QString::fromLatin1(name))->setEnabled(active);
    }
    m_actions.value(QStringLiteral("connect"))->setEnabled(m_deviceList->selectedPort().has_value());
    if (active) {
        m_actions.value(QStringLiteral("log"))->setChecked(view->controller()->isLogging());
        m_actions.value(QStringLiteral("hex"))->setChecked(view->view()->viewMode() == ViewMode::Hex);
    }
}

void MainWindow::showShortcuts()
{
    QStringList lines;
    for (QAction *action : findChildren<QAction *>()) {
        if (!action->shortcut().isEmpty())
            lines << QStringLiteral("%1\t%2").arg(action->shortcut().toString(QKeySequence::NativeText),
                                                  action->iconText());
    }
    lines.sort();
    m_lastMessage = lines.join(QLatin1Char('\n'));
    if (m_interactive)
        QMessageBox::information(this, tr("Keyboard shortcuts"), m_lastMessage);
}

void MainWindow::showAbout()
{
    m_lastMessage = tr("Portside %1\nA cross-platform serial terminal.\nMIT licensed.")
                        .arg(QString::fromLatin1(kVersion));
    if (m_interactive)
        QMessageBox::about(this, tr("About Portside"), m_lastMessage);
}

// -- drag and drop (F13's file half) ----------------------------------------------------

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls() && currentView() != nullptr)
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.isEmpty())
        return;
    // Dropping a file pastes it at the session's pace.
    const QString path = urls.first().toLocalFile();
    if (QFileInfo(path).isFile()) {
        pasteFile(path);
        event->acceptProposedAction();
    }
}

// -- geometry and shutdown -------------------------------------------------------------

void MainWindow::restoreGeometryState()
{
    const QByteArray geometry = m_settings->value(QStringLiteral("geometry")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    else
        resize(1100, 700);
    const QByteArray state = m_settings->value(QStringLiteral("windowState")).toByteArray();
    if (!state.isEmpty()) {
        restoreState(state);
    } else {
        // Wide enough on first launch for port, device, VID:PID and serial.
        resizeDocks({m_devicesDock}, {380}, Qt::Horizontal);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_settings->setValue(QStringLiteral("geometry"), saveGeometry());
    m_settings->setValue(QStringLiteral("windowState"), saveState());
    m_deviceList->stop();
    // Every session closes with its window, so no I/O thread outlives it.
    for (SessionView *view : views())
        view->closeSession();
    QMainWindow::closeEvent(event);
}

} // namespace portside::gui
