// File transfers in the desktop app (PLAN-M4 §6.2, §7.3, §7.4): the dialog,
// the progress panel, cancel, the ZMODEM auto-start prompt and drag-and-drop.

#include "gui/settings_dialog.h"
#include "gui/terminal_widget.h"
#include "gui/transfer_dialog.h"
#include "gui_support.h"
#include "support/loopback_peer.h"
#include "support/support.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QTabWidget>
#include <QtTest>

#include <future>

using namespace portside;
using namespace portside::gui;
using namespace portside::test;

namespace {

QByteArray randomBytes(qsizetype size)
{
    QByteArray data(size, Qt::Uninitialized);
    for (qsizetype i = 0; i < size; ++i)
        data[i] = char(QRandomGenerator::global()->bounded(256));
    return data;
}

// A window connected to the FTDI port over a loopback that does not echo,
// with a protocol engine standing in for the device.
struct Connected
{
    WindowFixture fixture;
    std::unique_ptr<MainWindow> window;
    SessionView *view = nullptr;

    Connected()
    {
        fixture.transports.options.echo = false;
        window = fixture.window();
        view = window->connectToPort(portFromRaw(ftdi()));
    }

    LoopbackTransport *device() const { return fixture.transports.last(); }
    SessionController *controller() const { return view->controller(); }
};

TransferOptions deviceOptions(Protocol protocol, const QString &directory = {})
{
    TransferOptions options;
    options.protocol = protocol;
    options.directory = directory;
    options.startTimeout = 10.0;
    return options;
}

} // namespace

class TestTransfersGui : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // -- the dialog -----------------------------------------------------------

    void theSendDialogOnlyAllowsWhatTheProtocolCanDo()
    {
        QTemporaryDir dir;
        const QString a = dir.filePath(QStringLiteral("a.bin"));
        const QString b = dir.filePath(QStringLiteral("b.bin"));
        writeFile(a, "a");
        writeFile(b, "b");
        TransferOptions defaults;
        defaults.protocol = Protocol::Xmodem;
        TransferDialog dialog(TransferDirection::Send, defaults);
        QVERIFY(!dialog.okButton()->isEnabled());
        QVERIFY(dialog.problem().contains(QStringLiteral("Add")));

        dialog.setFiles({a, b});
        QVERIFY(!dialog.okButton()->isEnabled());
        QVERIFY(dialog.problem().contains(QStringLiteral("one file")));

        dialog.protocol()->setCurrentIndex(dialog.protocol()->findData(QStringLiteral("zmodem")));
        QVERIFY(dialog.okButton()->isEnabled());
        QCOMPARE(dialog.files(), (QStringList{a, b}));
        QCOMPARE(dialog.options().protocol, Protocol::Zmodem);
        QVERIFY(!dialog.chunkSize()->isEnabled());

        dialog.protocol()->setCurrentIndex(dialog.protocol()->findData(QStringLiteral("raw")));
        QVERIFY(dialog.chunkSize()->isEnabled());
        dialog.chunkSize()->setValue(64);
        dialog.chunkDelay()->setValue(0.02);
        QCOMPARE(dialog.options().rawChunkSize, 64);
        QCOMPARE(dialog.options().rawChunkDelay, 0.02);

        dialog.fileList()->selectAll();
        for (QPushButton *button : dialog.findChildren<QPushButton *>()) {
            if (button->accessibleName() == QStringLiteral("Remove files"))
                button->click();
        }
        QVERIFY(dialog.files().isEmpty());
        QVERIFY(!dialog.okButton()->isEnabled());
    }

    void theReceiveDialogShowsTheOptionsThatApply()
    {
        TransferOptions defaults;
        defaults.protocol = Protocol::Zmodem;
        defaults.directory = QStringLiteral("/tmp/downloads");
        defaults.overwrite = OverwritePolicy::Rename;
        TransferDialog dialog(TransferDirection::Receive, defaults);
        // Raw cannot receive, so it is not offered.
        QCOMPARE(dialog.protocol()->findData(QStringLiteral("raw")), -1);
        QVERIFY(dialog.resume()->isEnabled());
        QVERIFY(!dialog.outputName()->isEnabled());
        QCOMPARE(dialog.options().overwrite, OverwritePolicy::Rename);
        QCOMPARE(dialog.options().directory, QStringLiteral("/tmp/downloads"));

        dialog.protocol()->setCurrentIndex(dialog.protocol()->findData(QStringLiteral("xmodem")));
        QVERIFY(dialog.outputName()->isEnabled());
        QVERIFY(dialog.textMode()->isEnabled());
        QVERIFY(!dialog.resume()->isEnabled());
        dialog.outputName()->setText(QStringLiteral("flash.bin"));
        dialog.textMode()->setChecked(true);
        QCOMPARE(dialog.options().outputName, QStringLiteral("flash.bin"));
        QVERIFY(dialog.options().textMode);

        dialog.directory()->clear();
        QVERIFY(!dialog.okButton()->isEnabled());
    }

    // -- sending and receiving through the window -------------------------------

    void sendingShowsProgressAndLocksThePort()
    {
        Connected app;
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("image.bin"));
        const QByteArray content = randomBytes(60000);
        writeFile(path, content);
        LoopbackPeer peer(app.device());
        std::atomic<bool> cancel{false};
        // The device starts its receiver a moment after Portside starts.
        auto device = std::async(std::launch::async, [&] {
            try {
                receiveFiles(peer, deviceOptions(Protocol::Zmodem, dir.filePath(QStringLiteral("device"))), cancel);
            } catch (const PortsideError &error) {
                return error.display();
            }
            return QString();
        });
        QVERIFY(app.window->sendFiles({path}, Protocol::Zmodem));
        QVERIFY(app.view->transferPanel()->isVisible());
        QVERIFY(app.view->transferTitle()->text().contains(QStringLiteral("ZMODEM")));
        QVERIFY(!app.window->action(QStringLiteral("send-file"))->isEnabled());
        QVERIFY(!app.window->action(QStringLiteral("break"))->isEnabled());
        QVERIFY(!app.window->action(QStringLiteral("paste-file"))->isEnabled());
        QVERIFY(app.window->action(QStringLiteral("cancel-transfer"))->isEnabled());

        // Keys typed now go nowhere, and the status line says why.
        QKeyEvent key(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
        QApplication::sendEvent(app.view->view(), &key);
        QVERIFY(app.view->statusText().contains(QStringLiteral("typing is paused")));

        QVERIFY(waitFor([&] { return !app.controller()->isTransferring(); }, 30000));
        QCOMPARE(device.get(), QString());
        QCOMPARE(readFile(dir.filePath(QStringLiteral("device/image.bin"))), content);
        QVERIFY(!app.view->transferPanel()->isVisible());
        QVERIFY(app.view->statusText().contains(QStringLiteral("ZMODEM: sent 1 file")));
        QVERIFY(app.window->action(QStringLiteral("send-file"))->isEnabled());
        QVERIFY(!app.window->action(QStringLiteral("cancel-transfer"))->isEnabled());
    }

    void receivingSavesIntoTheChosenFolder()
    {
        Connected app;
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("running-config"));
        const QByteArray content = QByteArray("hostname lab-sw1\r\n").repeated(500);
        writeFile(path, content);
        LoopbackPeer peer(app.device());
        std::atomic<bool> cancel{false};
        TransferOptions options = app.controller()->transferOptions(Protocol::Kermit);
        options.directory = dir.filePath(QStringLiteral("inbox"));
        QVERIFY(app.window->receiveFiles(options));
        QSignalSpy progressed(app.controller(), &SessionController::transferProgress);
        auto device = std::async(std::launch::async, [&] {
            try {
                sendFiles(peer, {path}, deviceOptions(Protocol::Kermit), cancel);
            } catch (const PortsideError &error) {
                return error.display();
            }
            return QString();
        });
        QVERIFY(waitFor([&] { return !app.controller()->isTransferring(); }, 30000));
        QCOMPARE(device.get(), QString());
        QCOMPARE(readFile(dir.filePath(QStringLiteral("inbox/running-config"))), content);
        QVERIFY(!progressed.isEmpty());
        QVERIFY(app.view->statusText().contains(QStringLiteral("Kermit: received 1 file")));
        QVERIFY(app.window->lastMessage().contains(QStringLiteral("Kermit")));
    }

    void cancelFromThePanelSaysSoWithoutAnErrorBox()
    {
        Connected app;
        TransferOptions options = app.controller()->transferOptions(Protocol::Zmodem);
        QTemporaryDir dir;
        options.directory = dir.path();
        QVERIFY(app.window->receiveFiles(options));
        app.window->showError(PortsideError(ErrorKind::Generic, QStringLiteral("marker")));
        app.view->transferCancel()->click();
        QVERIFY(waitFor([&] { return !app.controller()->isTransferring(); }, 5000));
        QVERIFY(app.view->statusText().contains(QStringLiteral("transfer cancelled")));
        // No error dialog for something the user asked for.
        QCOMPARE(app.window->lastMessage(), QStringLiteral("marker"));
        // The device was told, in ZMODEM's own words.
        QVERIFY(app.device()->written().contains(QByteArray(5, '\x18')));
    }

    void aFailedTransferShowsItsRemedy()
    {
        Connected app;
        TransferOptions options = app.controller()->transferOptions(Protocol::Ymodem);
        options.startTimeout = 0.5;
        QTemporaryDir dir;
        options.directory = dir.path();
        QVERIFY(app.window->receiveFiles(options));
        QVERIFY(waitFor([&] { return !app.controller()->isTransferring(); }, 10000));
        QVERIFY2(app.window->lastMessage().contains(QStringLiteral("sb")), qPrintable(app.window->lastMessage()));
        QVERIFY(app.view->statusText().contains(QStringLiteral("transfer failed")));
    }

    // -- ZMODEM auto-start (F18) --------------------------------------------------

    void theDeviceStartingSzOffersAReceive()
    {
        Connected app;
        QTemporaryDir dir;
        Profile profile = app.controller()->profile();
        profile.transfer.directory = dir.path();
        app.controller()->setProfile(profile);
        app.window->setInteractive(false, true);
        app.device()->feed("sz firmware.bin\r\n");
        app.device()->feed("rz\r**\x18" "B00000000000000\r\x8a\x11");
        QVERIFY(waitFor([&] { return app.controller()->isTransferring(); }, 3000));
        QCOMPARE(app.controller()->transfer()->options().protocol, Protocol::Zmodem);
        QCOMPARE(app.controller()->transfer()->direction(), TransferDirection::Receive);
        QCOMPARE(app.controller()->transfer()->options().directory, dir.path());
        app.controller()->cancelTransfer();
        QVERIFY(waitFor([&] { return !app.controller()->isTransferring(); }, 5000));
    }

    void decliningTheOfferStopsTheDevicesSz()
    {
        Connected app;
        app.window->setInteractive(false, false);
        app.device()->feed("**\x18" "B00000000000000\r\x8a\x11");
        QVERIFY(waitFor([&] { return app.device()->written().contains(QByteArray(10, '\x18')); }, 3000));
        QVERIFY(!app.controller()->isTransferring());
    }

    void autoStartCanBeTurnedOff()
    {
        Connected app;
        Profile profile = app.controller()->profile();
        profile.transfer.zmodemAutoStart = false;
        app.controller()->setProfile(profile);
        app.window->setInteractive(false, true);
        app.device()->feed("**\x18" "B00000000000000\r\x8a\x11");
        QTest::qWait(300);
        QVERIFY(!app.controller()->isTransferring());
        QVERIFY(app.device()->written().isEmpty());
    }

    void aDeviceRunningRzIsNoticed()
    {
        Connected app;
        app.window->setInteractive(false, true);
        app.device()->feed("**\x18" "B0100000023be50\r\x8a\x11");
        QVERIFY(waitFor([&] {
            return app.window->lastMessage().contains(QStringLiteral("waiting for a ZMODEM file"));
        }, 3000));
    }

    // -- drag and drop (F19) --------------------------------------------------------

    void droppingAFileCanSendIt()
    {
        Connected app;
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("dropped.bin"));
        writeFile(path, randomBytes(5000));
        app.window->setDropAction(MainWindow::DropAction::Send);
        app.window->dropFiles({path});
        QVERIFY(app.controller()->isTransferring());
        QCOMPARE(app.controller()->transfer()->files(), QStringList{path});
        // The profile's protocol: ZMODEM unless it says otherwise.
        QCOMPARE(app.controller()->transfer()->options().protocol, Protocol::Zmodem);
        app.controller()->cancelTransfer();
        QVERIFY(waitFor([&] { return !app.controller()->isTransferring(); }, 5000));
    }

    void droppingAFileCanStillPasteIt()
    {
        Connected app;
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("config.txt"));
        writeFile(path, "conf t\n");
        app.window->setDropAction(MainWindow::DropAction::Paste);
        app.window->dropFiles({path});
        QVERIFY(!app.controller()->isTransferring());
        QVERIFY(waitFor([&] { return app.device()->written().contains("conf t"); }, 5000));

        app.window->setDropAction(MainWindow::DropAction::Ignore);
        const qsizetype before = app.device()->written().size();
        app.window->dropFiles({path});
        QTest::qWait(200);
        QCOMPARE(app.device()->written().size(), before);
    }

    // -- profile defaults (F20) -------------------------------------------------------

    void theSettingsDialogEditsTransferDefaults()
    {
        Profile profile;
        profile.name = QStringLiteral("lab");
        profile.transfer.protocol = Protocol::Ymodem;
        profile.transfer.directory = QStringLiteral("/srv/images");
        profile.transfer.overwrite = OverwritePolicy::Overwrite;
        profile.transfer.zmodemAutoStart = false;
        SettingsDialog dialog(profile);
        QCOMPARE(dialog.tabs()->tabText(dialog.tabs()->count() - 1), QStringLiteral("Transfers"));
        QCOMPARE(dialog.toProfile().transfer, profile.transfer);
        dialog.transferProtocol()->setCurrentIndex(dialog.transferProtocol()->findData(QStringLiteral("kermit")));
        dialog.zmodemAutoStart()->setChecked(true);
        const Profile edited = dialog.toProfile();
        QCOMPARE(edited.transfer.protocol, Protocol::Kermit);
        QVERIFY(edited.transfer.zmodemAutoStart);
        QCOMPARE(edited.transfer.directory, QStringLiteral("/srv/images"));
    }

    void theTransferMenuIsThere()
    {
        Connected app;
        QStringList menus;
        for (QAction *action : app.window->menuBar()->actions())
            menus << action->text().remove(QLatin1Char('&'));
        QVERIFY(menus.contains(QStringLiteral("Transfer")));
        QCOMPARE(app.window->action(QStringLiteral("send-file"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+S")));
        QCOMPARE(app.window->action(QStringLiteral("receive-file"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+R")));
        // Without a session there is nothing to transfer over.
        app.window->closeCurrentTab();
        QVERIFY(!app.window->action(QStringLiteral("send-file"))->isEnabled());
        QVERIFY(!app.window->action(QStringLiteral("cancel-transfer"))->isEnabled());
    }
};

QTEST_MAIN(TestTransfersGui)
#include "tst_transfers.moc"
