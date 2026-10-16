// The VT100/xterm screen model (F7, F8).

#include "core/terminal.h"

#include <QTest>

using namespace portside;

namespace {

QStringList text(const Terminal &terminal)
{
    return terminal.snapshot().text();
}

const Cell &cellAt(const Terminal &terminal, int row, int column)
{
    static Snapshot snapshot;
    snapshot = terminal.snapshot();
    return snapshot.lines[std::size_t(row)][std::size_t(column)];
}

void feedLines(Terminal &terminal, int count, const char *prefix = "line")
{
    for (int n = 0; n < count; ++n)
        terminal.feed(QByteArray(prefix) + QByteArray::number(n) + "\r\n");
}

QStringList nonEmpty(const QStringList &lines)
{
    QStringList out;
    for (const QString &line : lines) {
        if (!line.isEmpty())
            out << line;
    }
    return out;
}

} // namespace

class TestTerminal : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // -- text -----------------------------------------------------------------

    void plainTextLandsOnScreen()
    {
        Terminal term(20, 4);
        term.feed("Switch> show version\r\n");
        QCOMPARE(text(term).at(0), QStringLiteral("Switch> show version"));
    }

    void defaultsMatchThePrd()
    {
        Terminal term;
        QCOMPARE(term.columns(), 80);
        QCOMPARE(term.rows(), 24);
        QCOMPARE(term.scrollback(), 10'000);
    }

    void carriageReturnOverwritesTheLine()
    {
        Terminal term(20, 4);
        term.feed("50%\r75%\r100%");
        QCOMPARE(text(term).at(0), QStringLiteral("100%"));
    }

    void backspaceMovesBack()
    {
        Terminal term(20, 4);
        term.feed("enabel\x08\x08le");
        QCOMPARE(text(term).at(0), QStringLiteral("enable"));
    }

    void outputWrapsAtTheScreenEdge()
    {
        Terminal term(10, 4);
        term.feed("0123456789abcde");
        QCOMPARE(text(term).mid(0, 2), (QStringList{QStringLiteral("0123456789"), QStringLiteral("abcde")}));
    }

    void exactlyFullLinesDoNotWrapEarly()
    {
        Terminal term(10, 4);
        term.feed("0123456789\r\nnext");
        QCOMPARE(text(term).mid(0, 2), (QStringList{QStringLiteral("0123456789"), QStringLiteral("next")}));
    }

    void tabsAdvanceToTheNextStop()
    {
        Terminal term(20, 4);
        term.feed("a\tb");
        QCOMPARE(text(term).at(0), QStringLiteral("a       b"));
    }

    // -- attributes ---------------------------------------------------------

    void coloursAndBoldAreCaptured()
    {
        Terminal term(20, 4);
        term.feed("\x1b[31;1mERR\x1b[0m ok");
        QCOMPARE(cellAt(term, 0, 0).fg, Color::indexed(1));
        QVERIFY(cellAt(term, 0, 2).has(Cell::Bold));
        QVERIFY(cellAt(term, 0, 4).fg.isDefault());
        QVERIFY(!cellAt(term, 0, 4).has(Cell::Bold));
    }

    void backgroundAndReverseVideo()
    {
        Terminal term(20, 4);
        term.feed("\x1b[44m\x1b[7mX");
        QCOMPARE(cellAt(term, 0, 0).bg, Color::indexed(4));
        QVERIFY(cellAt(term, 0, 0).has(Cell::Reverse));
    }

    void underlineItalicsAndStrikethrough()
    {
        Terminal term(20, 4);
        term.feed("\x1b[4mU\x1b[0m\x1b[3mI\x1b[0m\x1b[9mS");
        QVERIFY(cellAt(term, 0, 0).has(Cell::Underline));
        QVERIFY(cellAt(term, 0, 1).has(Cell::Italic));
        QVERIFY(cellAt(term, 0, 2).has(Cell::Strikethrough));
    }

    void brightAnd256AndTrueColour()
    {
        Terminal term(20, 4);
        term.feed("\x1b[91ma\x1b[38;5;208mb\x1b[38;2;10;20;30mc\x1b[48:2::1:2:3md\x1b[39;49me");
        QCOMPARE(cellAt(term, 0, 0).fg, Color::indexed(9));
        QCOMPARE(cellAt(term, 0, 1).fg, Color::indexed(208));
        QCOMPARE(cellAt(term, 0, 2).fg, Color::rgb(10, 20, 30));
        QCOMPARE(cellAt(term, 0, 3).bg, Color::rgb(1, 2, 3));
        QVERIFY(cellAt(term, 0, 4).fg.isDefault() && cellAt(term, 0, 4).bg.isDefault());
    }

    // -- cursor and erasing -------------------------------------------------

    void cursorPositioning()
    {
        Terminal term(20, 4);
        term.feed("\x1b[2;5HX");
        const Snapshot snapshot = term.snapshot();
        QCOMPARE(snapshot.lines[1][4].ch, U'X');
        QCOMPARE(snapshot.cursor.x, 5);
        QCOMPARE(snapshot.cursor.y, 1);
    }

    void cursorCanBeHiddenAndShown()
    {
        Terminal term(20, 4);
        term.feed("\x1b[?25l");
        QVERIFY(term.snapshot().cursor.hidden);
        term.feed("\x1b[?25h");
        QVERIFY(!term.snapshot().cursor.hidden);
    }

    void eraseInDisplayClearsTheScreen()
    {
        Terminal term(20, 4);
        term.feed("noise everywhere\r\nmore noise\r\n\x1b[2J\x1b[H");
        QCOMPARE(text(term), (QStringList{QString(), QString(), QString(), QString()}));
    }

    void eraseToEndOfLine()
    {
        Terminal term(20, 4);
        term.feed("keep this\r\x1b[5C\x1b[K");
        QCOMPARE(text(term).at(0), QStringLiteral("keep"));
    }

    void insertAndDeleteCharactersAndLines()
    {
        Terminal term(10, 4);
        term.feed("abcdef\r\x1b[2C\x1b[2@XY");
        QCOMPARE(text(term).at(0), QStringLiteral("abXYcdef"));
        term.feed("\r\x1b[3P");
        QCOMPARE(text(term).at(0), QStringLiteral("Ycdef"));
        term.feed("\x1b[2;1Hrow2\x1b[3;1Hrow3\x1b[2;1H\x1b[L");
        QCOMPARE(text(term).mid(1, 3), (QStringList{QString(), QStringLiteral("row2"), QStringLiteral("row3")}));
        term.feed("\x1b[M");
        QCOMPARE(text(term).at(1), QStringLiteral("row2"));
    }

    void scrollRegionsKeepTheRestOfTheScreen()
    {
        Terminal term(10, 5);
        term.feed("top\x1b[2;4r\x1b[4;1Hx\r\ny\r\nz");
        const QStringList lines = text(term);
        QCOMPARE(lines.at(0), QStringLiteral("top"));
        QCOMPARE(lines.at(1), QStringLiteral("x"));
        QCOMPARE(lines.at(2), QStringLiteral("y"));
        QCOMPARE(lines.at(3), QStringLiteral("z"));
        // Lines scrolled inside a region are not scrollback.
        QCOMPARE(term.historySize(), 0);
    }

    void reverseIndexScrollsDownAtTheTop()
    {
        Terminal term(10, 3);
        term.feed("a\r\nb\x1b[H\x1bMz");
        QCOMPARE(text(term), (QStringList{QStringLiteral("z"), QStringLiteral("a"), QStringLiteral("b")}));
    }

    void saveAndRestoreTheCursor()
    {
        Terminal term(10, 3);
        term.feed("ab\x1b" "7\x1b[3;5Hzz\x1b" "8c");
        QCOMPARE(text(term).at(0), QStringLiteral("abc"));
    }

    void autowrapCanBeTurnedOff()
    {
        Terminal term(5, 2);
        term.feed("\x1b[?7labcdefg");
        QCOMPARE(text(term), (QStringList{QStringLiteral("abcdg"), QString()}));
    }

    // -- character sets -----------------------------------------------------

    void decLineDrawingCharset()
    {
        Terminal term(20, 4);
        term.feed("\x1b(0lqqk\x1b(B");
        QCOMPARE(text(term).at(0), QStringLiteral("┌──┐"));
    }

    void lineDrawingDesignationSplitAcrossReads()
    {
        Terminal term(20, 4);
        term.feed("\x1b");
        term.feed("(0");
        term.feed("lqk");
        QCOMPARE(text(term).at(0), QStringLiteral("┌─┐"));
    }

    void charsetDesignationResetsBackToAscii()
    {
        Terminal term(20, 4);
        term.feed("\x1b(0l\x1b(Bl");
        QCOMPARE(text(term).at(0), QStringLiteral("┌l"));
    }

    void shiftOutUsesG1()
    {
        Terminal term(20, 4);
        term.feed("\x1b)0a\x0eq\x0fq");
        QCOMPARE(text(term).at(0), QStringLiteral("a─q"));
    }

    void wideCharactersTakeTwoCells()
    {
        Terminal term(10, 2);
        term.feed("a中b");
        QCOMPARE(text(term).at(0), QStringLiteral("a中b"));
        QCOMPARE(term.snapshot().cursor.x, 4);
        // One entry per UTF-16 unit of the unstripped line, then the end.
        const std::vector<int> columns = term.textColumns(0);
        QCOMPARE(std::vector<int>(columns.begin(), columns.begin() + 4), (std::vector<int>{0, 1, 3, 4}));
        QCOMPARE(columns.back(), 10);
    }

    void unknownEscapeSequencesDoNotCrash()
    {
        Terminal term(40, 4);
        term.feed("\x1b[?9999h\x1b]0;title\x07\x1b[99;99;99mstill here\x1bP1$r\x1b\\");
        QVERIFY(text(term).at(0).contains(QStringLiteral("still here")));
    }

    void deviceQueriesAreAnswered()
    {
        Terminal term(20, 4);
        term.feed("\x1b[3;7H\x1b[6n\x1b[5n\x1b[c");
        QCOMPARE(term.takeResponses(), QByteArray("\x1b[3;7R\x1b[0n\x1b[?1;2c"));
        QVERIFY(term.takeResponses().isEmpty());
    }

    void applicationCursorKeysFollowDecckm()
    {
        Terminal term(20, 4);
        QVERIFY(!term.applicationCursorKeys());
        term.feed("\x1b[?1h");
        QVERIFY(term.applicationCursorKeys());
        term.feed("\x1b[?1l");
        QVERIFY(!term.applicationCursorKeys());
    }

    // -- scrollback (F8) ----------------------------------------------------

    void linesScrollIntoHistory()
    {
        Terminal term(20, 4);
        feedLines(term, 8);
        QCOMPARE(text(term).at(0), QStringLiteral("line5"));
        QCOMPARE(term.totalLines(), 9);
        QVERIFY(term.allTextLines().contains(QStringLiteral("line0")));
    }

    void scrollbackIsBoundedAndKeepsTheNewest()
    {
        Terminal term(20, 4, 10);
        feedLines(term, 100);
        const QStringList lines = term.allTextLines();
        QVERIFY(lines.size() <= 14);
        QVERIFY(lines.contains(QStringLiteral("line99")));
        QVERIFY(!lines.contains(QStringLiteral("line0")));
    }

    void scrollingUpShowsOlderLines()
    {
        Terminal term(20, 4);
        feedLines(term, 10);
        term.scrollBy(3);
        QVERIFY(!term.following());
        QCOMPARE(text(term).at(0), QStringLiteral("line4"));
        QVERIFY(term.snapshot().cursor.hidden);
    }

    void newOutputDoesNotYankAScrolledViewBack()
    {
        Terminal term(20, 4);
        feedLines(term, 10);
        term.scrollBy(3);
        const QStringList visible = text(term);
        term.feed("brand new line\r\n");
        QCOMPARE(text(term), visible);
    }

    void aPinnedViewStaysPutEvenWhenTheHistoryIsFull()
    {
        Terminal term(20, 4, 50);
        feedLines(term, 100);
        term.scrollBy(10);
        const QStringList visible = text(term);
        feedLines(term, 5, "more");
        QCOMPARE(text(term), visible);
    }

    void followReturnsToTheLiveTail()
    {
        Terminal term(20, 4);
        feedLines(term, 10);
        term.scrollBy(5);
        term.follow();
        QVERIFY(term.following());
        QVERIFY(text(term).contains(QStringLiteral("line9")));
    }

    void scrollingIsClampedAtBothEnds()
    {
        Terminal term(20, 4);
        feedLines(term, 10);
        term.scrollBy(1000);
        QCOMPARE(term.scrollOffset(), term.totalLines() - term.rows());
        term.scrollBy(-1000);
        QCOMPARE(term.scrollOffset(), 0);
    }

    void pageScrollingMovesByAScreen()
    {
        Terminal term(20, 4);
        feedLines(term, 20);
        term.scrollPage(1);
        QCOMPARE(term.scrollOffset(), term.rows());
    }

    void eraseSavedLinesClearsTheScrollback()
    {
        Terminal term(20, 4);
        feedLines(term, 10);
        term.feed("\x1b[3J");
        QCOMPARE(term.historySize(), 0);
    }

    // -- alternate screen ---------------------------------------------------

    void alternateScreenHidesAndRestoresThePrimary()
    {
        Terminal term(20, 4);
        term.feed("important output\r\n");
        term.feed("\x1b[?1049h");
        term.feed("full screen program");
        QVERIFY(term.alternateScreen());
        QCOMPARE(text(term).at(0), QStringLiteral("full screen program"));
        term.feed("\x1b[?1049l");
        QVERIFY(!term.alternateScreen());
        QCOMPARE(text(term).at(0), QStringLiteral("important output"));
    }

    void switchingMidChunkDrawsOnTheRightScreen()
    {
        Terminal term(20, 4);
        term.feed("before\x1b[?1049hduring\x1b[?1049lafter");
        QCOMPARE(text(term).at(0), QStringLiteral("beforeafter"));
    }

    void alternateScreenOutputNeverEntersTheScrollback()
    {
        Terminal term(20, 4);
        term.feed("keep me\r\n");
        term.feed("\x1b[?1049h");
        feedLines(term, 50, "frame ");
        term.feed("\x1b[?1049l");
        const QStringList history = term.allTextLines();
        QVERIFY(history.contains(QStringLiteral("keep me")));
        for (const QString &line : history)
            QVERIFY(!line.contains(QStringLiteral("frame")));
    }

    void alternateScreenStartsCleanEachTime()
    {
        Terminal term(20, 4);
        term.feed("\x1b[?1049hfirst visit\x1b[?1049l");
        term.feed("\x1b[?1049h");
        QCOMPARE(text(term).at(0), QString());
    }

    void everyAlternateScreenModeIsRecognised_data()
    {
        QTest::addColumn<QByteArray>("mode");
        QTest::newRow("47") << QByteArray("47");
        QTest::newRow("1047") << QByteArray("1047");
        QTest::newRow("1049") << QByteArray("1049");
    }

    void everyAlternateScreenModeIsRecognised()
    {
        QFETCH(QByteArray, mode);
        Terminal term(20, 4);
        term.feed("\x1b[?" + mode + "h");
        QVERIFY(term.alternateScreen());
        term.feed("\x1b[?" + mode + "l");
        QVERIFY(!term.alternateScreen());
    }

    void scrollingIsInertOnTheAlternateScreen()
    {
        Terminal term(20, 4);
        feedLines(term, 10);
        term.feed("\x1b[?1049h");
        term.scrollBy(5);
        QCOMPARE(term.scrollOffset(), 0);
    }

    // -- geometry and lifecycle ------------------------------------------------

    void shrinkingMovesScreenLinesIntoTheScrollback()
    {
        Terminal term(20, 4);
        feedLines(term, 4);
        term.resize(20, 2);
        QCOMPARE(nonEmpty(term.allTextLines()),
                 (QStringList{QStringLiteral("line0"), QStringLiteral("line1"), QStringLiteral("line2"),
                              QStringLiteral("line3")}));
    }

    void shrinkingDropsBlankRowsBelowTheCursorFirst()
    {
        Terminal term(20, 10);
        term.feed("prompt> ");
        term.resize(20, 3);
        QCOMPARE(text(term).at(0), QStringLiteral("prompt>"));
        QCOMPARE(term.historySize(), 0);
    }

    void shrinkingKeepsTheAttributesOfMovedLines()
    {
        Terminal term(20, 4);
        term.feed("\x1b[31mred line\x1b[0m\r\n");
        feedLines(term, 3);
        term.resize(20, 2);
        QVERIFY(term.historySize() > 0);
        QCOMPARE(term.historyLine(0).at(0).fg, Color::indexed(1));
    }

    void shrinkingAndGrowingDoesNotDuplicateLines()
    {
        Terminal term(20, 4);
        feedLines(term, 3);
        term.resize(20, 2);
        term.resize(20, 8);
        QCOMPARE(nonEmpty(term.allTextLines()),
                 (QStringList{QStringLiteral("line0"), QStringLiteral("line1"), QStringLiteral("line2")}));
    }

    void resizeKeepsTheScrollback()
    {
        Terminal term(20, 4);
        feedLines(term, 10);
        term.resize(40, 6);
        QCOMPARE(term.columns(), 40);
        QCOMPARE(term.rows(), 6);
        QVERIFY(term.allTextLines().contains(QStringLiteral("line9")));
    }

    void splitUtf8RendersOneCharacter()
    {
        Terminal term(20, 4);
        term.feed("caf\xc3");
        term.feed("\xa9");
        QCOMPARE(text(term).at(0), QStringLiteral("café"));
    }

    void charsetCanBeChangedAtRuntime()
    {
        Terminal term(20, 4);
        term.setCharset(CharacterSet::Cp437);
        term.feed(QByteArray(1, char(0xDB)));
        QCOMPARE(text(term).at(0), QStringLiteral("█"));
    }

    void stripHighBitAppliesToTheScreen()
    {
        Terminal term(20, 4);
        term.setStripHighBit(true);
        term.feed(QByteArray("\xc8\xc9"));
        QCOMPARE(text(term).at(0), QStringLiteral("HI"));
    }

    void resetClearsScreenAndHistory()
    {
        Terminal term(20, 4);
        feedLines(term, 10);
        term.reset();
        QCOMPARE(text(term), (QStringList{QString(), QString(), QString(), QString()}));
        QVERIFY(nonEmpty(term.allTextLines()).isEmpty());
    }

    void revisionChangesOnlyWhenSomethingHappens()
    {
        Terminal term(20, 4);
        const quint64 before = term.revision();
        term.feed("");
        term.feed("\xc3"); // half a character: nothing to draw yet
        QCOMPARE(term.revision(), before);
        term.feed("x");
        QVERIFY(term.revision() > before);
    }

    void localEchoTextIsDrawnToo()
    {
        Terminal term(20, 4);
        term.feedText(QStringLiteral("hi ✓"));
        QCOMPARE(text(term).at(0), QStringLiteral("hi ✓"));
    }
};

QTEST_GUILESS_MAIN(TestTerminal)
#include "tst_terminal.moc"
