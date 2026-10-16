// The main window (F1, F2, F20, F22, and the M3 exit criterion).

#include "gui/device_list.h"
#include "gui/gui_support.h"
#include "gui/signals_panel.h"
#include "gui/styles.h"
#include "gui/terminal_widget.h"
#include "support/support.h"

#include <QAction>
#include <QCheckBox>
#include <QMenu>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTest>
#include <QThread>
#include <QToolBar>
#include <QTreeWidget>

using namespace portside;
using namespace portside::gui;
using namespace portside::test;

namespace {

SessionView *connectFirst(MainWindow &window)
{
    window.deviceList()->selectDevice(QStringLiteral("/dev/ttyUSB0"));
    return window.connectSelected();
}

} // namespace

class TestMainWindow : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        m_logs.reset(new QTemporaryDir);
        qputenv("PORTSIDE_LOG_DIR", m_logs->path().toUtf8());
    }

    // -- the exit criterion, in the part a test can hold ----------------------------

    void aNewUserReachesAConnectedSessionInTwoActions()
    {
        // PRD M3: connect in under 30 seconds, first time.  The human half is
        // docs/USABILITY-CHECK.md; this asserts the shape of the path — the
        // device is listed when the window opens, and selecting it then
        // activating it reaches a connected terminal.  No dialogs, no typing.
        WindowFixture fixture;
        auto window = fixture.window();
        window->show();
        QCOMPARE(window->deviceList()->tree()->topLevelItemCount(), 2);
        int actions = 0;
        QTreeWidget *tree = window->deviceList()->tree();
        tree->setCurrentItem(tree->topLevelItem(0));
        ++actions; // click the device
        window->deviceList()->activateSelected();
        ++actions; // press Enter, double-click or Connect: one action
        QVERIFY(actions <= 3);
        SessionView *view = window->currentView();
        QVERIFY(view);
        QVERIFY(view->controller()->isConnected());
    }

    void theWindowIsInteractiveQuickly()
    {
        WindowFixture fixture;
        QElapsedTimer clock;
        clock.start();
        auto window = fixture.window();
        window->show();
        QVERIFY2(clock.elapsed() < 2000, qPrintable(QString::number(clock.elapsed())));
    }

    // -- the device list (F1) ---------------------------------------------------------

    void devicesAreListedWithTheirDetails()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        QTreeWidgetItem *usb = window->deviceList()->tree()->topLevelItem(0);
        QCOMPARE(usb->text(0), QStringLiteral("/dev/ttyUSB0"));
        QCOMPARE(usb->text(1), QStringLiteral("FTDI FT232R"));
        QCOMPARE(usb->text(2), QStringLiteral("0403:6001"));
        QCOMPARE(usb->text(3), QStringLiteral("A50285BI"));
        QVERIFY(usb->toolTip(0).contains(QStringLiteral("USB 0403:6001 serial A50285BI")));
    }

    void hotPlugAddsAndRemovesRows()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        fixture.ports.ports << ftdi(QStringLiteral("/dev/ttyUSB1"));
        QVERIFY(waitFor([&] { return window->deviceList()->tree()->topLevelItemCount() == 3; }, 2000));
        fixture.ports.ports.removeFirst();
        QVERIFY(waitFor([&] { return window->deviceList()->tree()->topLevelItemCount() == 2; }, 2000));
    }

    void theEmptyStateExplainsWhatToCheck()
    {
        WindowFixture fixture;
        fixture.ports.ports.clear();
        auto window = fixture.window();
        window->show();
        QVERIFY(window->deviceList()->emptyLabel()->isVisible());
        QVERIFY(window->deviceList()->emptyLabel()->text().contains(QStringLiteral("driver")));
        QVERIFY(!window->action(QStringLiteral("connect"))->isEnabled());
    }

    // -- sessions (F22) ----------------------------------------------------------------

    void connectingOpensATab()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        QSignalSpy opened(window.get(), &MainWindow::sessionOpened);
        SessionView *view = connectFirst(*window);
        QVERIFY(view);
        QCOMPARE(opened.size(), 1);
        QCOMPARE(window->tabs()->count(), 1);
        QCOMPARE(window->tabs()->tabText(0), QStringLiteral("ttyUSB0 — 115200"));
    }

    void severalSessionsRunIndependently()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        window->connectToPort(portFromRaw(ftdi()));
        window->connectToPort(portFromRaw(uart()));
        QCOMPARE(window->views().size(), 2);
        window->views().at(0)->controller()->send("one");
        window->views().at(1)->controller()->send("two");
        QVERIFY(waitFor([&] { return fixture.transports.made.at(0)->written() == "one"
                                     && fixture.transports.made.at(1)->written() == "two"; }));
    }

    void closingOneTabLeavesTheOthersConnected()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        window->connectToPort(portFromRaw(ftdi()));
        SessionView *second = window->connectToPort(portFromRaw(uart()));
        window->closeTab(0);
        QCOMPARE(window->tabs()->count(), 1);
        QVERIFY(second->controller()->isConnected());
    }

    void closingAConnectedTabAsksFirst()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        window->setInteractive(false, false); // the user says no
        connectFirst(*window);
        window->closeCurrentTab();
        QCOMPARE(window->tabs()->count(), 1);
        window->setInteractive(false, true);
        window->closeCurrentTab();
        QCOMPARE(window->tabs()->count(), 0);
    }

    void detachingATabKeepsTheSessionAlive()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        SessionView *view = connectFirst(*window);
        QPointer<MainWindow> detached = window->detachCurrentTab();
        QVERIFY(detached);
        QCOMPARE(window->tabs()->count(), 0);
        QCOMPARE(detached->currentView(), view);
        QVERIFY(view->controller()->isConnected());
        view->controller()->send("still here");
        QVERIFY(waitFor([&] { return fixture.transports.last()->written() == "still here"; }));
        detached->close();
        QVERIFY(waitFor([&] { return detached.isNull(); }));
    }

    void aNewWindowIsIndependent()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        QPointer<MainWindow> second = window->openNewWindow();
        QVERIFY(second);
        QCOMPARE(second->deviceList()->tree()->topLevelItemCount(), 2);
        second->close();
        QVERIFY(waitFor([&] { return second.isNull(); }));
    }

    void theStatusBarShowsTheSession()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        SessionView *view = connectFirst(*window);
        view->controller()->send("abc");
        QVERIFY(waitFor([&] { return view->statusText().contains(QStringLiteral("out 3 B")); }));
    }

    // -- session actions -----------------------------------------------------------------

    void breakIsSentFromTheToolbar()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        connectFirst(*window);
        window->action(QStringLiteral("break"))->trigger();
        QVERIFY(waitFor([&] { return fixture.transports.last()->breaks() == QList<double>{0.25}; }));
        QVERIFY(waitFor([&] { return window->lastMessage() == QStringLiteral("native break, 250 ms"); }));
    }

    void loggingCanBeToggled()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        SessionView *view = connectFirst(*window);
        window->toggleLogging();
        QVERIFY(view->controller()->isLogging());
        QVERIFY(window->action(QStringLiteral("log"))->isChecked());
        const QString path = view->controller()->logger()->path();
        QVERIFY(path.startsWith(m_logs->path()));
        fixture.transports.last()->feed("logged line\r\n");
        QVERIFY(waitFor([&] { return view->controller()->session()->bytesIn() == 13; }));
        window->toggleLogging();
        QVERIFY(!view->controller()->isLogging());
        QCOMPARE(readFile(path), QByteArray("logged line\n"));
    }

    void hexViewCanBeToggled()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        SessionView *view = connectFirst(*window);
        window->toggleHex();
        QCOMPARE(view->view()->viewMode(), ViewMode::Hex);
        QVERIFY(window->action(QStringLiteral("hex"))->isChecked());
    }

    void pasteFileSendsTheFile()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        connectFirst(*window);
        const QString path = fixture.dir.filePath(QStringLiteral("snippet.txt"));
        writeFile(path, "conf t\nend\n");
        window->pasteFile(path);
        QVERIFY(waitFor([&] { return fixture.transports.last()->written() == "conf t\rend\r"; }));
    }

    void clearFindAndCopy()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        SessionView *view = connectFirst(*window);
        view->view()->feed("router uptime is 5 weeks");
        window->openFind();
        QVERIFY(view->findBar()->isVisibleTo(view));
        view->view()->selectCells({0, 0}, {0, 5});
        QCOMPARE(window->copySelection(), QStringLiteral("router"));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("router"));
        window->clearTerminal();
        QVERIFY(view->terminal()->snapshot().text().join(QString()).isEmpty());
    }

    void settingsChangeTheSessionLive()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        SessionView *view = connectFirst(*window);
        Profile edited = view->controller()->profile();
        edited.serial.baudrate = 9600;
        edited.terminal.fontSize = 16;
        window->applySessionProfile(edited);
        QVERIFY(waitFor([&] { return fixture.transports.last()->settings().baudrate == 9600; }));
        QCOMPARE(view->view()->fontSize(), 16);
        QCOMPARE(window->lastMessage(), QStringLiteral("Settings applied."));
    }

    // -- errors (F6) ------------------------------------------------------------------------

    void anErrorDialogShowsTheRemedyVerbatim()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        const PortsideError error(ErrorKind::PermissionDenied, QStringLiteral("permission denied opening /dev/ttyUSB0"),
                                  QStringLiteral("/dev/ttyUSB0"),
                                  QStringLiteral("Add yourself:\n\n    sudo usermod -aG dialout $USER\n\nThen log in again."));
        QMessageBox *box = window->buildErrorBox(error);
        QCOMPARE(box->text(), error.text());
        QCOMPARE(box->informativeText(), error.remedy());
        bool hasCopy = false;
        for (QAbstractButton *button : box->buttons())
            hasCopy = hasCopy || button->text() == QStringLiteral("Copy fix");
        QVERIFY(hasCopy);
        delete box;
        window->showError(error);
        QCOMPARE(window->lastMessage(), error.display());
    }

    void theFixCanBeCopiedToTheClipboard()
    {
        MainWindow::copyRemedy(QStringLiteral("Do this:\n\n    sudo usermod -aG uucp $USER\n    newgrp uucp\n\nDone."));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("sudo usermod -aG uucp $USER\nnewgrp uucp"));
        MainWindow::copyRemedy(QStringLiteral("No commands here."));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("No commands here."));
    }

    void aFailingConnectReportsTheError()
    {
        WindowFixture fixture;
        fixture.transports.failWith = PortsideError(ErrorKind::PortBusy, QStringLiteral("/dev/ttyUSB0 is busy"),
                                                    QStringLiteral("/dev/ttyUSB0"), QStringLiteral("Close minicom."));
        auto window = fixture.window();
        QVERIFY(!connectFirst(*window));
        QCOMPARE(window->tabs()->count(), 0);
        QVERIFY(window->lastMessage().contains(QStringLiteral("Close minicom.")));
    }

    // -- profiles (F20, F2) -------------------------------------------------------------------

    void aBoundProfileIsAppliedOnConnect()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        Profile bound;
        bound.name = QStringLiteral("cisco");
        bound.serial.baudrate = 9600;
        bound = bound.withDevice(DeviceIdentity::fromPort(portFromRaw(ftdi())));
        window->store().save(bound);
        SessionView *view = connectFirst(*window);
        QCOMPARE(view->controller()->profile().name, QStringLiteral("cisco"));
        QCOMPARE(fixture.transports.last()->settings().baudrate, 9600);
        QCOMPARE(window->tabs()->tabText(0), QStringLiteral("ttyUSB0 — 9600"));
    }

    void aSessionCanBeSavedAsAProfile()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        connectFirst(*window);
        const auto saved = window->saveSessionAsProfile(QStringLiteral("bench"));
        QVERIFY(saved);
        const Profile loaded = window->store().load(QStringLiteral("bench"));
        QCOMPARE(loaded.deviceKeys, QStringList{QStringLiteral("usb_0403_6001_A50285BI")});
        QCOMPARE(loaded.serial.baudrate, 115200);
    }

    // -- appearance and accessibility ----------------------------------------------------------

    void themesApplyToChromeAndTerminal()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        SessionView *view = connectFirst(*window);
        window->applyTheme(ThemeName::Light);
        QCOMPARE(qApp->styleSheet(), stylesheet(ThemeName::Light));
        QCOMPARE(view->view()->paletteName(), ThemeName::Light);
        QVERIFY(window->themeActions().value(ThemeName::Light)->isChecked());
        QVERIFY(!window->themeActions().value(ThemeName::Dark)->isChecked());
    }

    void theThemeIsRemembered()
    {
        WindowFixture fixture;
        {
            auto window = fixture.window();
            window->applyTheme(ThemeName::HighContrast);
        }
        auto again = fixture.window();
        QCOMPARE(again->currentTheme(), ThemeName::HighContrast);
    }

    void everyToolbarActionHasAShortcut()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        for (QAction *action : window->toolBar()->actions())
            QVERIFY2(!action->shortcut().isEmpty(), qPrintable(action->iconText()));
    }

    void interactiveWidgetsHaveAccessibleNames()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        SessionView *view = connectFirst(*window);
        QVERIFY(!window->deviceList()->tree()->accessibleName().isEmpty());
        QVERIFY(!window->tabs()->accessibleName().isEmpty());
        QVERIFY(!view->view()->accessibleName().isEmpty());
        QVERIFY(!view->statusLabel()->accessibleName().isEmpty());
        QVERIFY(!view->signalsPanel()->dtrBox()->accessibleName().isEmpty());
        for (QAction *action : window->findChildren<QAction *>()) {
            if (!action->isSeparator() && !action->menu())
                QVERIFY2(!action->iconText().isEmpty(), qPrintable(action->objectName()));
        }
    }

    void actionsAreDisabledWithoutASession()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        for (const char *name : {"disconnect", "break", "log", "hex", "find", "paste-file", "settings"})
            QVERIFY2(!window->action(QString::fromLatin1(name))->isEnabled(), name);
        QVERIFY(!window->action(QStringLiteral("connect"))->isEnabled());
        window->deviceList()->selectDevice(QStringLiteral("/dev/ttyS0"));
        QVERIFY(window->action(QStringLiteral("connect"))->isEnabled());
    }

    void closingTheWindowClosesEverySessionAndLeaksNoThread()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        SessionView *first = window->connectToPort(portFromRaw(ftdi()));
        SessionView *second = window->connectToPort(portFromRaw(uart()));
        QThread *threads[] = {first->controller()->session()->ioThread(),
                              second->controller()->session()->ioThread()};
        Session *sessions[] = {first->controller()->session(), second->controller()->session()};
        window->close();
        for (Session *session : sessions)
            QCOMPARE(session->state(), SessionState::Closed);
        for (QThread *thread : threads)
            QVERIFY(thread->isFinished());
    }

private:
    std::unique_ptr<QTemporaryDir> m_logs;
};

QTEST_MAIN(TestMainWindow)
#include "tst_main_window.moc"
