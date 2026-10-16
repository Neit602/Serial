// The painted terminal widget (F7, F8, F10).

#include "gui/terminal_widget.h"
#include "support/support.h"

#include <QApplication>
#include <QClipboard>
#include <QFontMetricsF>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>
#include <algorithm>

using namespace portside;
using namespace portside::gui;
using namespace portside::test;

namespace {

struct Fixture
{
    Terminal terminal{80, 24};
    TerminalWidget widget{&terminal};

    Fixture()
    {
        widget.resize(640, 400);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));
    }

    QPoint centreOf(int row, int column) const
    {
        const QSizeF cell = widget.cellSize();
        return QPoint(int((column + 0.5) * cell.width()), int((row + 0.5) * cell.height()));
    }
};

} // namespace

class TestTerminalWidget : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void theEmulatorIsSizedToTheWidget()
    {
        Fixture f;
        const QSizeF cell = f.widget.cellSize();
        QCOMPARE(f.terminal.columns(), int(f.widget.width() / cell.width()));
        QCOMPARE(f.terminal.rows(), int(f.widget.height() / cell.height()));
        QSignalSpy resized(&f.widget, &TerminalWidget::resizedCells);
        f.widget.resize(320, 200);
        QCOMPARE(resized.size(), 1);
        QCOMPARE(f.terminal.columns(), int(320 / cell.width()));
    }

    void receivedTextReachesTheEmulator()
    {
        Fixture f;
        f.widget.feed("Router# ");
        QCOMPARE(f.terminal.snapshot().text().at(0), QStringLiteral("Router#"));
    }

    void paintingHandlesColoursAttributesAndWideText()
    {
        Fixture f;
        f.widget.feed("\x1b[1;31mred\x1b[0m \x1b[7mrev\x1b[0m \x1b[4;38;5;208munder\x1b[0m 中文 ┌─┐\r\n");
        f.widget.setFocus();
        const QImage image = f.widget.grab().toImage();
        QVERIFY(!image.isNull());
    }

    void renderingIsBatchedNotPerByte()
    {
        Fixture f;
        QTest::qWait(50);
        const int before = f.widget.paintCount();
        for (int i = 0; i < 2000; ++i)
            f.widget.feed("x");
        QTest::qWait(100);
        const int painted = f.widget.paintCount() - before;
        QVERIFY2(painted <= 12, qPrintable(QString::number(painted)));
        QVERIFY(painted >= 1);
    }

    void theHexViewKeepsTheBytes()
    {
        Fixture f;
        f.widget.feed(QByteArray("\x00\x01hello\xff", 8));
        f.widget.setViewMode(ViewMode::Hex);
        QCOMPARE(f.widget.viewMode(), ViewMode::Hex);
        QVERIFY(!f.widget.grab().isNull());
    }

    void changingTheFontChangesTheCellGrid()
    {
        Fixture f;
        const QSizeF small = f.widget.cellSize();
        f.widget.setTerminalFont({}, 20);
        QVERIFY(f.widget.cellSize().height() > small.height());
        QCOMPARE(f.widget.fontSize(), 20);
    }

    void anUnknownFontFallsBackToAMonospace()
    {
        Fixture f;
        f.widget.setTerminalFont(QStringLiteral("No Such Font Anywhere"), 11);
        QVERIFY(f.widget.cellSize().width() > 0);
        // Whatever the system substitutes must still keep the grid: narrow
        // and wide glyphs advance by the same amount.  (Font databases do not
        // always flag a monospace font as fixed pitch, so measure instead.)
        const QFontMetricsF metrics(f.widget.terminalFont());
        QCOMPARE(metrics.horizontalAdvance(QLatin1Char('i')), metrics.horizontalAdvance(QLatin1Char('W')));
    }

    void themesCanBeSwitched()
    {
        Fixture f;
        f.widget.setTheme(ThemeName::HighContrast);
        QCOMPARE(f.widget.paletteName(), ThemeName::HighContrast);
        f.widget.setTheme(ThemeName::Light);
        QCOMPARE(f.widget.paletteName(), ThemeName::Light);
    }

    void draggingSelectsText()
    {
        Fixture f;
        f.widget.feed("show running-config\r\nsecond line");
        QSignalSpy selected(&f.widget, &TerminalWidget::selectionChanged);
        QTest::mousePress(&f.widget, Qt::LeftButton, {}, f.centreOf(0, 0));
        QTest::mouseRelease(&f.widget, Qt::LeftButton, {}, f.centreOf(0, 3));
        QCOMPARE(f.widget.selectedText(), QStringLiteral("show"));
        QCOMPARE(selected.last().first().toString(), QStringLiteral("show"));
    }

    void selectionSpansSeveralRows()
    {
        Fixture f;
        f.widget.feed("first\r\nsecond\r\nthird");
        f.widget.selectCells({0, 2}, {2, 1});
        QCOMPARE(f.widget.selectedText(), QStringLiteral("rst\nsecond\nth"));
    }

    void copyPutsTheSelectionOnTheClipboard()
    {
        Fixture f;
        f.widget.feed("10.0.0.1");
        f.widget.selectCells({0, 0}, {0, 7});
        QCOMPARE(f.widget.copySelection(), QStringLiteral("10.0.0.1"));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("10.0.0.1"));
    }

    void selectToCopyIsAnOption()
    {
        Fixture f;
        QApplication::clipboard()->setText(QStringLiteral("untouched"));
        f.widget.feed("auto copy");
        f.widget.setSelectToCopy(true);
        QTest::mousePress(&f.widget, Qt::LeftButton, {}, f.centreOf(0, 0));
        QTest::mouseRelease(&f.widget, Qt::LeftButton, {}, f.centreOf(0, 3));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("auto"));
    }

    void copyingNothingLeavesTheClipboardAlone()
    {
        Fixture f;
        QApplication::clipboard()->setText(QStringLiteral("keep me"));
        QCOMPARE(f.widget.copySelection(), QString());
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("keep me"));
    }

    void theSelectionCanBeCleared()
    {
        Fixture f;
        f.widget.feed("abc");
        f.widget.selectCells({0, 0}, {0, 2});
        f.widget.clearSelection();
        QCOMPARE(f.widget.selectedText(), QString());
    }

    void scrollingMovesThroughTheHistory()
    {
        Fixture f;
        for (int i = 0; i < 100; ++i)
            f.widget.feed("line " + QByteArray::number(i) + "\r\n");
        f.widget.scrollLines(10);
        QCOMPARE(f.terminal.scrollOffset(), 10);
        f.widget.scrollPages(-1);
        QCOMPARE(f.terminal.scrollOffset(), std::max(0, 10 - f.terminal.rows()));
        f.widget.followTail();
        QVERIFY(f.terminal.following());
    }

    void theWheelScrolls()
    {
        Fixture f;
        for (int i = 0; i < 100; ++i)
            f.widget.feed("line " + QByteArray::number(i) + "\r\n");
        QWheelEvent wheel(QPointF(10, 10), f.widget.mapToGlobal(QPointF(10, 10)), QPoint(), QPoint(0, 240),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&f.widget, &wheel);
        QCOMPARE(f.terminal.scrollOffset(), 6);
    }

    void tabGoesToTheDeviceNotToFocusNavigation()
    {
        qRegisterMetaType<QKeyEvent *>();
        Fixture f;
        QSignalSpy keys(&f.widget, &TerminalWidget::keyPressed);
        f.widget.setFocus();
        QTest::keyClick(&f.widget, Qt::Key_Tab);
        QCOMPARE(keys.size(), 1);
        QVERIFY(f.widget.hasFocus());
    }

    void theTerminalHasAnAccessibleName()
    {
        Fixture f;
        QCOMPARE(f.widget.accessibleName(), QStringLiteral("Terminal output"));
        QVERIFY(!f.widget.accessibleDescription().isEmpty());
    }
};

QTEST_MAIN(TestTerminalWidget)
#include "tst_terminal_widget.moc"
