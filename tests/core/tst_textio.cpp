// Line discipline: endings, echo, backspace, XON/XOFF (F9, F14).

#include "core/textio.h"

#include <QTest>

using namespace portside;

class TestTextIo : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void endingBytes_()
    {
        QCOMPARE(endingBytes(LineEnding::Cr), QByteArray("\r"));
        QCOMPARE(endingBytes(LineEnding::Lf), QByteArray("\n"));
        QCOMPARE(endingBytes(LineEnding::CrLf), QByteArray("\r\n"));
        QCOMPARE(endingBytes(LineEnding::None), QByteArray());
    }

    void backspaceIsExplicit()
    {
        QCOMPARE(backspaceByte(Backspace::Del), '\x7f');
        QCOMPARE(backspaceByte(Backspace::Bs), '\x08');
        QCOMPARE(TextPolicy{}.backspace, Backspace::Del);
    }

    void encodeLineAddsTheConfiguredEnding()
    {
        TextPolicy policy;
        QCOMPARE(policy.encodeLine(QStringLiteral("show run")), QByteArray("show run\r"));
        policy.sendEnding = LineEnding::CrLf;
        QCOMPARE(policy.encodeLine(QStringLiteral("é")), QByteArray("\xc3\xa9\r\n"));
    }

    void outputFromAnyPlatformReachesTheDeviceTheSameWay()
    {
        TextPolicy policy;
        policy.sendEnding = LineEnding::Cr;
        QCOMPARE(policy.translateOutput("a\r\nb\nc\rd"), QByteArray("a\rb\rc\rd"));
        policy.sendEnding = LineEnding::CrLf;
        QCOMPARE(policy.translateOutput("a\n\nb"), QByteArray("a\r\n\r\nb"));
        policy.sendEnding = LineEnding::None;
        QCOMPARE(policy.translateOutput("a\r\nb"), QByteArray("a\r\nb"));
    }

    void inputIsLeftAloneByDefault()
    {
        TextPolicy policy;
        QCOMPARE(policy.translateInput("a\r\nb"), QByteArray("a\r\nb"));
        policy.receiveEnding = LineEnding::Lf;
        QCOMPARE(policy.translateInput("a\r\nb\rc"), QByteArray("a\nb\nc"));
    }

    void localEchoOnlyWhenAskedFor()
    {
        TextPolicy policy;
        QVERIFY(policy.echoFor("x").isEmpty());
        policy.localEcho = true;
        QCOMPARE(policy.echoFor("x"), QByteArray("x"));
    }

    void xoffPausesAndXonResumes()
    {
        XonXoffGate gate(true, 30.0);
        QVERIFY(!gate.isPaused(0.0));
        gate.observe(QByteArray(1, kXoff), 1.0);
        QVERIFY(gate.isPaused(2.0));
        gate.observe(QByteArray(1, kXon), 3.0);
        QVERIFY(!gate.isPaused(3.0));
    }

    void aLostXonDoesNotWedgeTheSessionForever()
    {
        XonXoffGate gate(true, 5.0);
        gate.observe(QByteArray(1, kXoff), 0.0);
        QVERIFY(gate.isPaused(4.9));
        QVERIFY(!gate.isPaused(5.0));
        QCOMPARE(gate.timeouts(), 1);
    }

    void aDisabledGateIgnoresEverything()
    {
        XonXoffGate gate;
        gate.observe(QByteArray(1, kXoff), 0.0);
        QVERIFY(!gate.isPaused(0.0));
        XonXoffGate enabled(true);
        enabled.observe(QByteArray(1, kXoff), 0.0);
        enabled.resume();
        QVERIFY(!enabled.isPaused(0.1));
    }
};

QTEST_GUILESS_MAIN(TestTextIo)
#include "tst_textio.moc"
