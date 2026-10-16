#pragma once

// The Portside main window (PRD F22, and the app shell for M3).
//
// Tabs of sessions, a device list, the toolbar and menus.  Every session has
// its own I/O thread inside the core, so windows are independent: a tab can
// move to a window of its own without dropping its connection.

#include "core/discovery.h"
#include "core/factory.h"
#include "core/profiles.h"
#include "gui/palette.h"
#include "gui/session_view.h"

#include <QHash>
#include <QMainWindow>

#include <optional>

class QAction;
class QDockWidget;
class QMessageBox;
class QSettings;
class QTabWidget;
class QToolBar;

namespace portside::gui {

class DeviceList;

inline const QString kOrganisation = QStringLiteral("Portside");
inline const QString kApplication = QStringLiteral("Portside");

struct MainWindowOptions
{
    // Empty means the platform's profile directory.
    QString profileDirectory;
    // Injected in tests; empty means the OS's real port list.
    PortLister lister;
    // Injected in tests; empty means real serial ports.
    TransportFactory transportFactory;
    // Where the theme and geometry are remembered; empty means the platform
    // default (QSettings with the organisation and application names).
    QString settingsFile;
    int portPollMs = 1000;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(MainWindowOptions options = {}, QWidget *parent = nullptr);
    ~MainWindow() override;

    // -- sessions ----------------------------------------------------------

    SessionView *currentView() const;
    QList<SessionView *> views() const;
    // Connect to the highlighted device (the Connect button).
    SessionView *connectSelected();
    // Open a session for `port`, applying its bound profile (F1, F2, F20).
    SessionView *connectToPort(const PortInfo &port, const std::optional<Profile> &profile = std::nullopt);
    // Ask for settings first, then connect (F3).
    SessionView *connectWithSettings();
    // Close one tab, confirming if its session is still connected.
    void closeTab(int index);
    void closeCurrentTab();
    // Move the active tab into its own window (F22).
    MainWindow *detachCurrentTab();
    // Take over a session view from another window (F22).
    void adopt(SessionView *view, const QString &title);
    // A second, empty window (F22).
    MainWindow *openNewWindow();

    // -- session actions ---------------------------------------------------

    void sendBreak();
    void toggleLogging();
    void toggleHex();
    void clearTerminal();
    QString copySelection();
    void openFind();
    // Paste a file at the session's pace (F13); asks when `path` is empty.
    void pasteFile(const QString &path = {});
    void editSessionSettings();
    // What the settings dialog does on OK, callable without the dialog.
    void applySessionProfile(const Profile &profile);
    void openProfiles();
    // Save the active session as a named profile (asks when `name` is empty).
    std::optional<Profile> saveSessionAsProfile(const QString &name = {});

    // -- appearance and messages --------------------------------------------

    void applyTheme(ThemeName name);
    ThemeName currentTheme() const { return m_theme; }
    // Show a typed error with its remedy, verbatim (F6).
    void showError(const PortsideError &error);
    // The dialog showError() runs, built but not shown (for tests).
    QMessageBox *buildErrorBox(const PortsideError &error);
    // Put the fix on the clipboard: the indented command lines of a remedy.
    static void copyRemedy(const QString &remedy);
    void showShortcuts();
    void showAbout();

    // Tests turn dialogs off: confirmations then return `answer`, and errors
    // are only recorded in lastMessage().
    void setInteractive(bool interactive, bool answer = true);
    QString lastMessage() const { return m_lastMessage; }

    DeviceList *deviceList() const { return m_deviceList; }
    QTabWidget *tabs() const { return m_tabs; }
    QToolBar *toolBar() const { return m_toolbar; }
    QAction *action(const QString &name) const { return m_actions.value(name); }
    QHash<ThemeName, QAction *> themeActions() const { return m_themeActions; }
    ProfileStore store() const { return m_store; }

Q_SIGNALS:
    // A session was opened (for tests and future plugins).
    void sessionOpened(portside::gui::SessionView *view);

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    QAction *makeAction(const QString &name, const QString &text, const QString &shortcut,
                        void (MainWindow::*slot)(), const QString &tip = {});
    void buildActions();
    void buildMenus();
    void buildToolbar();
    void attachView(SessionView *view);
    void onStatus(const QString &text);
    void notify(const QString &message);
    bool confirm(const QString &title, const QString &question);
    void updateActions();
    void restoreGeometryState();
    static QString tabTitle(const QString &device, const Profile &profile);

    MainWindowOptions m_options;
    ProfileStore m_store;
    QSettings *m_settings = nullptr;
    ThemeName m_theme = ThemeName::Dark;
    QString m_lastMessage;
    bool m_interactive = true;
    bool m_answer = true;

    QTabWidget *m_tabs = nullptr;
    DeviceList *m_deviceList = nullptr;
    QDockWidget *m_devicesDock = nullptr;
    QToolBar *m_toolbar = nullptr;
    QHash<QString, QAction *> m_actions;
    QHash<ThemeName, QAction *> m_themeActions;
};

} // namespace portside::gui
