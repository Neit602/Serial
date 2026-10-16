// One session's page: keys, signals, find, status (F4, F7, F8, F9, F22).

#include "gui/gui_support.h"
#include "gui/keymap.h"
#include "gui/signals_panel.h"
#include "gui/terminal_widget.h"
#include "support/support.h"

#include <QCheckBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QSignalSpy>
#include <QTest>

using namespace portside;
using namespace portside::gui;
using namespace portside::test;

namespace {

#if defined(Q_OS_MACOS)
constexpr Qt::KeyboardModifier kControl = Qt::MetaModifier;
#else
constexpr Qt::KeyboardModifier kControl = Qt::ControlModifier;
#endif

} // namespace

class TestSessionView : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // -- keyboard (F7, F9) ---------------------------------------------------

    void typingReachesTheDevice()
    {
        ViewFixture f;
        QTest::keyClicks(f.view->view(), QStringLiteral("show ip"));
        QVERIFY(waitFor([&] { return f.transport->written() == "show ip"; }));
    }

    void enterSendsTheConfiguredEnding()
    {
        ViewFixture f;
        TextPolicy policy;
        policy.sendEnding = LineEnding::CrLf;
        f.controller()->setTextPolicy(policy);
        QTest::keyClick(f.view->view(), Qt::Key_Return);
        QVERIFY(waitFor([&] { return f.transport->written() == "\r\n"; }));
    }

    void backspaceFollowsTheProfile()
    {
        TextPolicy policy;
        policy.backspace = Backspace::Bs;
        const QKeyEvent backspace(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
        QCOMPARE(keyToBytes(backspace, policy), QByteArray("\x08"));
        policy.backspace = Backspace::Del;
        QCOMPARE(keyToBytes(backspace, policy), QByteArray("\x7f"));
    }

    void controlKeysReachTheDevice()
    {
        // Ctrl-C must interrupt the switch, not the app.
        ViewFixture f;
        QTest::keyClick(f.view->view(), Qt::Key_C, kControl);
        QTest::keyClick(f.view->view(), Qt::Key_BracketLeft, kControl);
        QVERIFY(waitFor([&] { return f.transport->written() == "\x03\x1b"; }));
    }

    void cursorKeysFollowTheTerminalsMode()
    {
        ViewFixture f;
        QTest::keyClick(f.view->view(), Qt::Key_Up);
        f.transport->feed("\x1b[?1h");
        QVERIFY(waitFor([&] { return f.view->terminal()->applicationCursorKeys(); }));
        QTest::keyClick(f.view->view(), Qt::Key_Up);
        QVERIFY(waitFor([&] { return f.transport->written() == "\x1b[A\x1bOA"; }));
    }

    void localEchoShowsWhatWasTyped()
    {
        ViewFixture f;
        TextPolicy policy;
        policy.localEcho = true;
        f.controller()->setTextPolicy(policy);
        QTest::keyClicks(f.view->view(), QStringLiteral("hi"));
        QCOMPARE(f.view->terminal()->snapshot().text().at(0), QStringLiteral("hi"));
    }

    void typingReturnsToTheLiveTail()
    {
        ViewFixture f;
        for (int n = 0; n < 200; ++n)
            f.view->view()->feed("line" + QByteArray::number(n) + "\r\n");
        f.view->view()->scrollPages(1);
        QVERIFY(!f.view->terminal()->following());
        QTest::keyClick(f.view->view(), Qt::Key_X);
        QVERIFY(f.view->terminal()->following());
    }

    void pageKeysScrollLocallyAndSendNothing()
    {
        ViewFixture f;
        for (int n = 0; n < 200; ++n)
            f.view->view()->feed("line" + QByteArray::number(n) + "\r\n");
        QTest::keyClick(f.view->view(), Qt::Key_PageUp);
        QVERIFY(f.view->terminal()->scrollOffset() > 0);
        QVERIFY(f.view->statusText().contains(QStringLiteral("scrollback -")));
        QTest::keyClick(f.view->view(), Qt::Key_PageDown);
        QCOMPARE(f.view->terminal()->scrollOffset(), 0);
        QTest::qWait(50);
        QVERIFY(f.transport->written().isEmpty());
    }

    void deviceQueriesAreAnsweredOnTheWire()
    {
        ViewFixture f;
        f.transport->feed("\x1b[5;10H\x1b[6n");
        QVERIFY(waitFor([&] { return f.transport->written() == "\x1b[5;10R"; }));
    }

    // -- signals (F4) ----------------------------------------------------------

    void unknownModemLinesAreShownAsUnknown()
    {
        ViewFixture f;
        f.view->signalsPanel()->setLines(ModemLines{true, false, std::nullopt, std::nullopt});
        QCOMPARE(f.view->signalsPanel()->stateOf(QStringLiteral("CTS")), QStringLiteral("asserted"));
        QCOMPARE(f.view->signalsPanel()->stateOf(QStringLiteral("DSR")), QStringLiteral("low"));
        QCOMPARE(f.view->signalsPanel()->stateOf(QStringLiteral("DCD")), QStringLiteral("not reported by this driver"));
    }

    void theLinesArePolled()
    {
        ViewFixture f;
        QVERIFY(waitFor([&] { return f.view->signalsPanel()->stateOf(QStringLiteral("CTS")) == QStringLiteral("asserted"); }, 3000));
    }

    void togglingDtrReachesTheTransport()
    {
        ViewFixture f;
        f.view->signalsPanel()->dtrBox()->setChecked(false);
        QVERIFY(waitFor([&] { return f.transport->dtr() == std::optional<bool>(false); }));
        f.view->signalsPanel()->rtsBox()->setChecked(false);
        QVERIFY(waitFor([&] { return f.transport->rts() == std::optional<bool>(false); }));
    }

    void aDriverWithoutControlLinesDisablesTheToggles()
    {
        LoopbackOptions options;
        options.supportsControlLines = false;
        ViewFixture f(options);
        f.view->signalsPanel()->dtrBox()->setChecked(false);
        QVERIFY(waitFor([&] { return !f.view->signalsPanel()->dtrBox()->isEnabled(); }));
        QVERIFY(!f.view->signalsPanel()->dtrBox()->toolTip().isEmpty());
    }

    void signalsAreGreyedOutWhenDisconnected()
    {
        ViewFixture f;
        QVERIFY(f.view->signalsPanel()->isEnabled());
        f.view->closeSession();
        QVERIFY(!f.view->signalsPanel()->isEnabled());
    }

    // -- find (F8) ---------------------------------------------------------------

    void findHighlightsAndCountsMatches()
    {
        ViewFixture f;
        f.view->view()->feed("Gi1/0/1 up\r\nGi1/0/2 down\r\nGi1/0/1 flap\r\n");
        f.view->openFind();
        QVERIFY(f.view->findBar()->isVisible());
        f.view->findInput()->setText(QStringLiteral("gi1/0/1"));
        QCOMPARE(f.view->matches().size(), 2);
        QCOMPARE(f.view->findStatus()->text(), QStringLiteral("2 matches"));
    }

    void findReportsNoMatch()
    {
        ViewFixture f;
        f.view->view()->feed("nothing here");
        f.view->findInput()->setText(QStringLiteral("absent"));
        QCOMPARE(f.view->findStatus()->text(), QStringLiteral("no match"));
    }

    void findIsLiteralByDefault()
    {
        ViewFixture f;
        f.view->view()->feed("10.0.0.1 and 10a0b0c1");
        f.view->findInput()->setText(QStringLiteral("10.0.0.1"));
        QCOMPARE(f.view->matches().size(), 1);
    }

    void findMovesThroughTheScrollback()
    {
        ViewFixture f;
        f.view->view()->feed("needle early\r\n");
        for (int n = 0; n < 300; ++n)
            f.view->view()->feed("hay\r\n");
        f.view->findInput()->setText(QStringLiteral("needle"));
        QVERIFY(f.view->terminal()->scrollOffset() > 0);
        QVERIFY(f.view->terminal()->snapshot().text().contains(QStringLiteral("needle early")));
        f.view->findNext();
        f.view->findPrevious();
    }

    void closingFindClearsTheHighlights()
    {
        ViewFixture f;
        f.view->view()->feed("abc abc");
        f.view->openFind();
        f.view->findInput()->setText(QStringLiteral("abc"));
        f.view->closeFind();
        QVERIFY(!f.view->findBar()->isVisible());
        QVERIFY(f.view->matches().isEmpty());
    }

    // -- status and paste (F22, F13) -------------------------------------------------

    void theStatusLineReportsTheSession()
    {
        ViewFixture f;
        f.transport->feed("12345");
        QVERIFY(waitFor([&] { return f.view->statusText().contains(QStringLiteral("in 5 B")); }));
        const QString status = f.view->statusText();
        QVERIFY2(status.startsWith(QStringLiteral("loopback:// · 115200 8N1 none · in 5 B · out 0 B")),
                 qPrintable(status));
        QVERIFY(status.endsWith(QStringLiteral("connected")));
        f.view->toggleHex();
        QVERIFY(f.view->statusText().contains(QStringLiteral("HEX")));
    }

    void reconnectingIsVisible()
    {
        LoopbackOptions options;
        auto transport = std::make_unique<LoopbackTransport>(options);
        LoopbackTransport *raw = transport.get();
        SessionOptions sessionOptions;
        sessionOptions.reconnect.enabled = true;
        sessionOptions.reconnect.initialDelay = 5.0;
        sessionOptions.reopen = [] { return std::make_unique<LoopbackTransport>(); };
        auto session = std::make_unique<Session>(std::move(transport), sessionOptions);
        session->open();
        Profile profile;
        profile.name = QStringLiteral("p");
        SessionView view(new SessionController(std::move(session), profile));
        QSignalSpy errors(&view, &SessionView::errorRaised);
        raw->setVanishAfter(1);
        view.controller()->send("x");
        QVERIFY(waitFor([&] { return view.statusText().contains(QStringLiteral("reconnecting (attempt 1")); }));
        // Waiting for the device is news for the status line, not a dialog.
        QVERIFY(errors.isEmpty());
        view.closeSession();
    }

    void pastingAFileShowsProgressAndSendsIt()
    {
        ViewFixture f;
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("config.txt"));
        writeFile(path, "hostname core\ninterface Gi1/0/1\n");
        QSignalSpy finished(f.controller(), &SessionController::pasteFinished);
        f.view->pasteFile(path);
        QVERIFY(waitFor([&] { return !finished.isEmpty(); }));
        QVERIFY(waitFor([&] { return f.transport->written() == "hostname core\rinterface Gi1/0/1\r"; }));
        QVERIFY(!f.view->pasteProgress()->isVisible());
        QVERIFY(f.view->statusText().contains(QStringLiteral("sent 2/2 lines")));
    }

    void anUnreadableFileIsReported()
    {
        ViewFixture f;
        QSignalSpy errors(f.view.get(), &SessionView::errorRaised);
        f.view->pasteFile(QStringLiteral("/nonexistent/portside.txt"));
        QCOMPARE(errors.size(), 1);
    }

    void receivedEndingsCanBeNormalisedForTheScreen()
    {
        Profile profile;
        profile.name = QStringLiteral("lf-device");
        profile.text.receiveEnding = LineEnding::CrLf;
        ViewFixture f({}, profile);
        f.transport->feed("one\ntwo\n");
        QVERIFY(waitFor([&] { return f.view->terminal()->snapshot().text().mid(0, 2)
                                     == QStringList{QStringLiteral("one"), QStringLiteral("two")}; }));
    }

    void clearEmptiesTheTerminal()
    {
        ViewFixture f;
        f.view->view()->feed("junk\r\nmore");
        f.view->clear();
        QVERIFY(f.view->terminal()->snapshot().text().join(QString()).isEmpty());
    }
};

QTEST_MAIN(TestSessionView)
#include "tst_session_view.moc"
