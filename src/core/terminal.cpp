#include "core/terminal.h"

#include "core/charwidth.h"

#include <algorithm>
#include <deque>

namespace portside {

namespace {

// DEC Special Graphics, 0x5F-0x7E (selected with ESC ( 0).
constexpr char32_t kDecGraphics[32] = {
    U' ',      // _ blank
    U'◆', // ` diamond
    U'▒', // a checkerboard
    U'␉', // b HT
    U'␌', // c FF
    U'␍', // d CR
    U'␊', // e LF
    U'°', // f degree
    U'±', // g plus/minus
    U'␤', // h NL
    U'␋', // i VT
    U'┘', // j ┘
    U'┐', // k ┐
    U'┌', // l ┌
    U'└', // m └
    U'┼', // n ┼
    U'⎺', // o scan line 1
    U'⎻', // p scan line 3
    U'─', // q ─
    U'⎼', // r scan line 7
    U'⎽', // s scan line 9
    U'├', // t ├
    U'┤', // u ┤
    U'┴', // v ┴
    U'┬', // w ┬
    U'│', // x │
    U'≤', // y ≤
    U'≥', // z ≥
    U'π', // { π
    U'≠', // | ≠
    U'£', // } £
    U'·', // ~ ·
};

struct Charsets
{
    char32_t g[2] = {U'B', U'B'};
    int active = 0;
};

struct SavedCursor
{
    bool valid = false;
    int x = 0;
    int y = 0;
    Cell pen;
    bool originMode = false;
    bool wrapPending = false;
    Charsets charsets;
};

bool isBlankLine(const Line &line)
{
    return std::all_of(line.begin(), line.end(), [](const Cell &cell) { return cell == Cell{}; });
}

// One screen: the primary (with history) or the alternate (without).
class Screen
{
public:
    Screen(int columns, int rows, int historyLimit)
        : cols(columns)
        , rowCount(rows)
        , historyMax(historyLimit)
    {
        reset();
    }

    int cols;
    int rowCount;
    std::vector<Line> buf;
    int x = 0;
    int y = 0;
    bool wrapPending = false;
    bool cursorHidden = false;
    Cell pen;
    int top = 0;
    int bottom = 0;
    bool autowrap = true;
    bool insertMode = false;
    bool originMode = false;
    bool newlineMode = false;
    bool appCursor = false;
    std::vector<bool> tabs;
    Charsets charsets;
    SavedCursor saved;
    char32_t lastPrinted = 0;

    std::deque<Line> history;
    int historyMax;
    // Monotonic count of lines pushed into the history, which is what keeps
    // a scrolled-back view pinned even when the history is full.
    quint64 pushed = 0;

    Cell blank() const
    {
        // Background colour erase, as xterm does: erased cells keep the
        // current background but nothing else.
        Cell cell;
        cell.bg = pen.bg;
        return cell;
    }

    Line blankLine() const { return Line(std::size_t(cols), blank()); }

    void resetTabs()
    {
        tabs.assign(std::size_t(cols), false);
        for (int i = 8; i < cols; i += 8)
            tabs[std::size_t(i)] = true;
    }

    void reset()
    {
        pen = Cell{};
        buf.assign(std::size_t(rowCount), Line(std::size_t(cols), Cell{}));
        x = y = 0;
        wrapPending = false;
        cursorHidden = false;
        top = 0;
        bottom = rowCount - 1;
        autowrap = true;
        insertMode = false;
        originMode = false;
        newlineMode = false;
        appCursor = false;
        charsets = Charsets{};
        saved = SavedCursor{};
        lastPrinted = 0;
        resetTabs();
    }

    void pushHistory(Line line)
    {
        if (historyMax <= 0)
            return;
        // History lines are stored trimmed; renderers pad them.
        while (!line.empty() && line.back() == Cell{})
            line.pop_back();
        history.push_back(std::move(line));
        while (int(history.size()) > historyMax)
            history.pop_front();
        ++pushed;
    }

    // -- cursor ------------------------------------------------------------

    void clampCursor()
    {
        x = std::clamp(x, 0, cols - 1);
        y = std::clamp(y, 0, rowCount - 1);
    }

    void cursorTo(int row, int column)
    {
        if (originMode) {
            row += top;
            y = std::clamp(row, top, bottom);
        } else {
            y = std::clamp(row, 0, rowCount - 1);
        }
        x = std::clamp(column, 0, cols - 1);
        wrapPending = false;
    }

    void cursorUp(int n)
    {
        const int limit = y >= top ? top : 0;
        y = std::max(limit, y - n);
        wrapPending = false;
    }

    void cursorDown(int n)
    {
        const int limit = y <= bottom ? bottom : rowCount - 1;
        y = std::min(limit, y + n);
        wrapPending = false;
    }

    void cursorForward(int n)
    {
        x = std::min(cols - 1, x + n);
        wrapPending = false;
    }

    void cursorBack(int n)
    {
        x = std::max(0, x - n);
        wrapPending = false;
    }

    void carriageReturn()
    {
        x = 0;
        wrapPending = false;
    }

    void backspace()
    {
        // VT100: with a wrap pending the cursor sits on the last column, and
        // a backspace moves left from there.
        wrapPending = false;
        x = std::max(0, x - 1);
    }

    void tab(int count = 1)
    {
        for (int i = 0; i < count; ++i) {
            int next = x + 1;
            while (next < cols && !tabs[std::size_t(next)])
                ++next;
            x = std::min(next, cols - 1);
        }
    }

    void backTab(int count = 1)
    {
        for (int i = 0; i < count; ++i) {
            int previous = x - 1;
            while (previous > 0 && !tabs[std::size_t(previous)])
                --previous;
            x = std::max(previous, 0);
        }
    }

    void saveCursor() { saved = {true, x, y, pen, originMode, wrapPending, charsets}; }

    void restoreCursor()
    {
        if (!saved.valid) {
            x = y = 0;
            pen = Cell{};
            originMode = false;
            wrapPending = false;
            charsets = Charsets{};
            return;
        }
        x = saved.x;
        y = saved.y;
        pen = saved.pen;
        originMode = saved.originMode;
        wrapPending = saved.wrapPending;
        charsets = saved.charsets;
        clampCursor();
    }

    // -- scrolling ---------------------------------------------------------

    void scrollUp(int n)
    {
        n = std::clamp(n, 0, bottom - top + 1);
        if (n == 0)
            return;
        if (top == 0) {
            for (int i = 0; i < n; ++i)
                pushHistory(buf[std::size_t(i)]);
        }
        std::rotate(buf.begin() + top, buf.begin() + top + n, buf.begin() + bottom + 1);
        for (int row = bottom - n + 1; row <= bottom; ++row)
            buf[std::size_t(row)] = blankLine();
    }

    void scrollDown(int n)
    {
        n = std::clamp(n, 0, bottom - top + 1);
        if (n == 0)
            return;
        std::rotate(buf.begin() + top, buf.begin() + bottom + 1 - n, buf.begin() + bottom + 1);
        for (int row = top; row < top + n; ++row)
            buf[std::size_t(row)] = blankLine();
    }

    void index()
    {
        if (y == bottom)
            scrollUp(1);
        else if (y < rowCount - 1)
            ++y;
    }

    void reverseIndex()
    {
        if (y == top)
            scrollDown(1);
        else if (y > 0)
            --y;
    }

    void lineFeed()
    {
        index();
        if (newlineMode)
            x = 0;
        wrapPending = false;
    }

    void setMargins(int newTop, int newBottom)
    {
        if (newTop < 0 || newBottom >= rowCount || newTop >= newBottom)
            return;
        top = newTop;
        bottom = newBottom;
        cursorTo(0, 0);
    }

    // -- editing -----------------------------------------------------------

    void blankify(Cell &cell) const
    {
        const Color background = cell.bg;
        cell = Cell{};
        cell.bg = background;
    }

    // Writing over half of a wide character must not leave the other half.
    void clearWideNeighbours(Line &line, int column, int width)
    {
        if (line[std::size_t(column)].has(Cell::WideTail) && column > 0)
            blankify(line[std::size_t(column - 1)]);
        const int end = column + width;
        if (end < cols && line[std::size_t(end)].has(Cell::WideTail)
            && line[std::size_t(end - 1)].has(Cell::Wide)) {
            blankify(line[std::size_t(end)]);
        }
    }

    void put(char32_t c)
    {
        const char32_t designation = charsets.g[charsets.active];
        if (designation == U'0' && c >= 0x5F && c <= 0x7E)
            c = kDecGraphics[c - 0x5F];
        else if (designation == U'A' && c == U'#')
            c = U'£';

        int width = charWidth(c);
        if (width == 0)
            return; // combining marks are dropped; see DECISIONS
        if (width == 2 && cols < 2)
            width = 1;

        if (wrapPending && autowrap) {
            carriageReturn();
            index();
        }
        wrapPending = false;

        if (width == 2 && x == cols - 1) {
            if (autowrap) {
                blankify(buf[std::size_t(y)][std::size_t(x)]);
                carriageReturn();
                index();
            } else {
                x = cols - 2;
            }
        }
        if (insertMode)
            insertChars(width);

        Line &line = buf[std::size_t(y)];
        clearWideNeighbours(line, x, width);
        Cell cell = pen;
        cell.ch = c;
        cell.attrs &= quint8(~(Cell::Wide | Cell::WideTail));
        if (width == 2) {
            cell.attrs |= Cell::Wide;
            line[std::size_t(x)] = cell;
            Cell tail = cell;
            tail.ch = U' ';
            tail.attrs = quint8((tail.attrs & ~Cell::Wide) | Cell::WideTail);
            line[std::size_t(x + 1)] = tail;
        } else {
            line[std::size_t(x)] = cell;
        }
        lastPrinted = c;
        x += width;
        if (x >= cols) {
            x = cols - 1;
            wrapPending = autowrap;
        }
    }

    void eraseInLine(int mode)
    {
        Line &line = buf[std::size_t(y)];
        int from = 0;
        int to = cols - 1;
        if (mode == 0)
            from = x;
        else if (mode == 1)
            to = x;
        for (int i = from; i <= to; ++i)
            line[std::size_t(i)] = blank();
    }

    void eraseInDisplay(int mode)
    {
        switch (mode) {
        case 0:
            eraseInLine(0);
            for (int row = y + 1; row < rowCount; ++row)
                buf[std::size_t(row)] = blankLine();
            break;
        case 1:
            eraseInLine(1);
            for (int row = 0; row < y; ++row)
                buf[std::size_t(row)] = blankLine();
            break;
        case 3:
            // xterm: also forget the saved lines.
            history.clear();
            [[fallthrough]];
        case 2:
            for (Line &line : buf)
                line = blankLine();
            break;
        default:
            break;
        }
    }

    void insertLines(int n)
    {
        if (y < top || y > bottom)
            return;
        n = std::min(n, bottom - y + 1);
        std::rotate(buf.begin() + y, buf.begin() + bottom + 1 - n, buf.begin() + bottom + 1);
        for (int row = y; row < y + n; ++row)
            buf[std::size_t(row)] = blankLine();
        carriageReturn();
    }

    void deleteLines(int n)
    {
        if (y < top || y > bottom)
            return;
        n = std::min(n, bottom - y + 1);
        std::rotate(buf.begin() + y, buf.begin() + y + n, buf.begin() + bottom + 1);
        for (int row = bottom - n + 1; row <= bottom; ++row)
            buf[std::size_t(row)] = blankLine();
        carriageReturn();
    }

    void insertChars(int n)
    {
        Line &line = buf[std::size_t(y)];
        n = std::min(n, cols - x);
        std::rotate(line.begin() + x, line.end() - n, line.end());
        for (int i = x; i < x + n; ++i)
            line[std::size_t(i)] = blank();
    }

    void deleteChars(int n)
    {
        Line &line = buf[std::size_t(y)];
        n = std::min(n, cols - x);
        std::rotate(line.begin() + x, line.begin() + x + n, line.end());
        for (int i = cols - n; i < cols; ++i)
            line[std::size_t(i)] = blank();
        wrapPending = false;
    }

    void eraseChars(int n)
    {
        Line &line = buf[std::size_t(y)];
        const int end = std::min(cols, x + n);
        for (int i = x; i < end; ++i)
            line[std::size_t(i)] = blank();
        wrapPending = false;
    }

    void alignmentTest()
    {
        Cell cell;
        cell.ch = U'E';
        for (Line &line : buf)
            std::fill(line.begin(), line.end(), cell);
        top = 0;
        bottom = rowCount - 1;
        x = y = 0;
    }

    // -- geometry ----------------------------------------------------------

    void resize(int newCols, int newRows)
    {
        if (newRows < rowCount) {
            int overflow = rowCount - newRows;
            // Blank rows below the cursor go first: they hold nothing.
            int removableBelow = 0;
            for (int row = rowCount - 1; row > y && removableBelow < overflow; --row) {
                if (!isBlankLine(buf[std::size_t(row)]))
                    break;
                ++removableBelow;
            }
            buf.resize(buf.size() - std::size_t(removableBelow));
            overflow -= removableBelow;
            // The rest move into the scrollback rather than being lost.
            for (int i = 0; i < overflow; ++i)
                pushHistory(buf[std::size_t(i)]);
            buf.erase(buf.begin(), buf.begin() + overflow);
            y -= overflow;
        } else if (newRows > rowCount) {
            for (int i = rowCount; i < newRows; ++i)
                buf.emplace_back(std::size_t(cols), Cell{});
        }
        for (Line &line : buf) {
            line.resize(std::size_t(newCols), Cell{});
            if (newCols > 0 && line.back().has(Cell::Wide))
                blankify(line.back());
        }
        cols = newCols;
        rowCount = newRows;
        top = 0;
        bottom = rowCount - 1;
        wrapPending = false;
        clampCursor();
        resetTabs();
    }
};

} // namespace

QString lineText(const Line &line)
{
    QString text;
    text.reserve(qsizetype(line.size()));
    for (const Cell &cell : line) {
        if (cell.has(Cell::WideTail))
            continue;
        const char32_t c = cell.ch ? cell.ch : U' ';
        text += QString::fromUcs4(&c, 1);
    }
    qsizetype end = text.size();
    while (end > 0 && text.at(end - 1) == QLatin1Char(' '))
        --end;
    text.truncate(end);
    return text;
}

QStringList Snapshot::text() const
{
    QStringList out;
    for (const Line &line : lines)
        out << lineText(line);
    return out;
}

// -- the emulator ------------------------------------------------------------

struct Terminal::Impl
{
    enum class State { Ground, Escape, EscapeIntermediate, Csi, Osc, String, StringEscape };

    Impl(int columns, int rows, int scrollbackLines, CharacterSet charset, bool strip)
        : scrollback(std::max(0, scrollbackLines))
        , primary(columns, rows, scrollback)
        // The alternate screen keeps no history: full-screen programs redraw
        // constantly and their frames are not scrollback anyone wants.
        , alternate(columns, rows, 0)
        , decoder(charset, strip)
    {
    }

    int scrollback;
    Screen primary;
    Screen alternate;
    bool altActive = false;
    IncrementalTextDecoder decoder;
    int scrollOffset = 0;
    quint64 revision = 0;
    QByteArray responses;

    // Parser state.
    State state = State::Ground;
    std::u32string intermediates;
    char32_t privateMarker = 0;
    std::vector<std::vector<int>> groups; // ';'-separated, ':' sub-parameters
    std::vector<int> group;
    int number = -1; // -1: no digits yet (a defaulted parameter)

    Screen &active() { return altActive ? alternate : primary; }
    const Screen &active() const { return altActive ? alternate : primary; }

    int maxScrollOffset() const { return int(primary.history.size()); }

    void feedCodePoints(const std::u32string &text)
    {
        if (text.empty())
            return;
        const quint64 before = primary.pushed;
        for (const char32_t c : text)
            process(c);
        ++revision;
        // Keep a scrolled-back view pinned to the same content.
        if (scrollOffset > 0) {
            const quint64 added = primary.pushed - before;
            if (added > 0)
                scrollOffset = int(std::min<quint64>(quint64(scrollOffset) + added,
                                                     quint64(maxScrollOffset())));
        }
    }

    void setAlternate(bool on)
    {
        if (on == altActive)
            return;
        if (on) {
            // The alternate screen always starts clean (1049 semantics).
            alternate = Screen(primary.cols, primary.rowCount, 0);
        }
        altActive = on;
        scrollOffset = 0;
    }

    // -- parameters --------------------------------------------------------

    int param(std::size_t index, int fallback) const
    {
        if (index >= groups.size() || groups[index].empty() || groups[index][0] < 0)
            return fallback;
        return groups[index][0];
    }

    // Movement counts treat 0 like 1, as every VT does.
    int count(std::size_t index) const
    {
        const int value = param(index, 1);
        return value <= 0 ? 1 : value;
    }

    void clearParams()
    {
        groups.clear();
        group.clear();
        number = -1;
        privateMarker = 0;
        intermediates.clear();
    }

    // -- the state machine -------------------------------------------------

    void process(char32_t c)
    {
        switch (state) {
        case State::Osc:
        case State::String:
            if (c == 0x07 && state == State::Osc) { // BEL ends an OSC
                state = State::Ground;
            } else if (c == 0x1B) {
                state = State::StringEscape;
            } else if (c == 0x9C) {
                state = State::Ground;
            }
            return; // OSC payloads (window titles, ...) are ignored
        case State::StringEscape:
            if (c == U'\\') {
                state = State::Ground;
                return;
            }
            state = State::Escape;
            clearParams();
            break; // the ESC began a new sequence; handle `c` below
        default:
            break;
        }

        if (c == 0x1B) {
            state = State::Escape;
            clearParams();
            return;
        }
        if (c == 0x18 || c == 0x1A) { // CAN, SUB abort a sequence
            state = State::Ground;
            return;
        }
        if (c < 0x20) {
            execute(c); // C0 controls act even inside a sequence
            return;
        }
        if (c == 0x7F)
            return;

        switch (state) {
        case State::Ground:
            if (c >= 0x80 && c < 0xA0)
                return; // C1 controls are not interpreted in UTF-8 mode
            active().put(c);
            return;
        case State::Escape:
            if (c >= 0x20 && c <= 0x2F) {
                intermediates.push_back(c);
                state = State::EscapeIntermediate;
            } else if (c == U'[') {
                clearParams();
                state = State::Csi;
            } else if (c == U']') {
                state = State::Osc;
            } else if (c == U'P' || c == U'X' || c == U'^' || c == U'_') {
                state = State::String;
            } else {
                state = State::Ground;
                escDispatch(c);
            }
            return;
        case State::EscapeIntermediate:
            if (c >= 0x20 && c <= 0x2F) {
                intermediates.push_back(c);
                return;
            }
            state = State::Ground;
            escDispatch(c);
            return;
        case State::Csi:
            if (c >= U'0' && c <= U'9') {
                number = std::min((number < 0 ? 0 : number) * 10 + int(c - U'0'), 99999);
            } else if (c == U':') {
                group.push_back(number);
                number = -1;
            } else if (c == U';') {
                group.push_back(number);
                groups.push_back(group);
                group.clear();
                number = -1;
            } else if (c >= 0x3C && c <= 0x3F) {
                if (groups.empty() && group.empty() && number < 0)
                    privateMarker = c;
            } else if (c >= 0x20 && c <= 0x2F) {
                intermediates.push_back(c);
            } else if (c >= 0x40 && c <= 0x7E) {
                group.push_back(number);
                groups.push_back(group);
                state = State::Ground;
                csiDispatch(c);
            } else {
                state = State::Ground;
            }
            return;
        default:
            return;
        }
    }

    void execute(char32_t c)
    {
        Screen &s = active();
        switch (c) {
        case 0x08:
            s.backspace();
            break;
        case 0x09:
            if (!s.wrapPending)
                s.tab();
            break;
        case 0x0A:
        case 0x0B:
        case 0x0C:
            s.lineFeed();
            break;
        case 0x0D:
            s.carriageReturn();
            break;
        case 0x0E: // SO: invoke G1
            s.charsets.active = 1;
            break;
        case 0x0F: // SI: invoke G0
            s.charsets.active = 0;
            break;
        default:
            break; // BEL, NUL and the rest are ignored
        }
    }

    void escDispatch(char32_t final)
    {
        Screen &s = active();
        if (intermediates.empty()) {
            switch (final) {
            case U'7':
                s.saveCursor();
                break;
            case U'8':
                s.restoreCursor();
                break;
            case U'D':
                s.index();
                break;
            case U'E':
                s.carriageReturn();
                s.index();
                break;
            case U'M':
                s.reverseIndex();
                break;
            case U'H':
                s.tabs[std::size_t(s.x)] = true;
                break;
            case U'c':
                fullReset();
                break;
            default:
                break; // keypad modes, single shifts and the rest
            }
            return;
        }
        const char32_t first = intermediates.front();
        if (first == U'(') {
            s.charsets.g[0] = final;
        } else if (first == U')') {
            s.charsets.g[1] = final;
        } else if (first == U'#' && final == U'8') {
            s.alignmentTest();
        }
    }

    // RIS: back to the power-on state, keeping the scrollback as xterm does.
    void fullReset()
    {
        altActive = false;
        primary.reset();
        alternate.reset();
        scrollOffset = 0;
    }

    void csiDispatch(char32_t final)
    {
        Screen &s = active();
        if (privateMarker == U'?') {
            switch (final) {
            case U'h':
            case U'l':
                for (std::size_t i = 0; i < groups.size(); ++i)
                    setPrivateMode(param(i, 0), final == U'h');
                break;
            case U'J':
                s.eraseInDisplay(param(0, 0));
                break;
            case U'K':
                s.eraseInLine(param(0, 0));
                break;
            default:
                break;
            }
            return;
        }
        if (privateMarker == U'>') {
            if (final == U'c')
                responses += QByteArrayLiteral("\x1b[>0;95;0c");
            return;
        }
        if (privateMarker != 0)
            return;
        if (!intermediates.empty()) {
            if (intermediates == U"!" && final == U'p')
                softReset();
            return; // cursor style and the rest are cosmetic
        }

        switch (final) {
        case U'@':
            s.insertChars(count(0));
            break;
        case U'A':
            s.cursorUp(count(0));
            break;
        case U'B':
        case U'e':
            s.cursorDown(count(0));
            break;
        case U'C':
        case U'a':
            s.cursorForward(count(0));
            break;
        case U'D':
            s.cursorBack(count(0));
            break;
        case U'E':
            s.cursorDown(count(0));
            s.carriageReturn();
            break;
        case U'F':
            s.cursorUp(count(0));
            s.carriageReturn();
            break;
        case U'G':
        case U'`':
            s.x = std::clamp(count(0) - 1, 0, s.cols - 1);
            s.wrapPending = false;
            break;
        case U'H':
        case U'f':
            s.cursorTo(count(0) - 1, count(1) - 1);
            break;
        case U'I':
            s.tab(count(0));
            break;
        case U'J':
            s.eraseInDisplay(param(0, 0));
            break;
        case U'K':
            s.eraseInLine(param(0, 0));
            break;
        case U'L':
            s.insertLines(count(0));
            break;
        case U'M':
            s.deleteLines(count(0));
            break;
        case U'P':
            s.deleteChars(count(0));
            break;
        case U'S':
            s.scrollUp(count(0));
            break;
        case U'T':
            if (groups.size() <= 1)
                s.scrollDown(count(0));
            break;
        case U'X':
            s.eraseChars(count(0));
            break;
        case U'Z':
            s.backTab(count(0));
            break;
        case U'b':
            if (s.lastPrinted != 0) {
                const int repeat = std::min(count(0), s.cols * s.rowCount);
                for (int i = 0; i < repeat; ++i)
                    s.put(s.lastPrinted);
            }
            break;
        case U'c':
            if (param(0, 0) == 0)
                responses += QByteArrayLiteral("\x1b[?1;2c"); // VT100 with AVO
            break;
        case U'd': {
            const int row = count(0) - 1;
            s.cursorTo(row, s.x);
            break;
        }
        case U'g':
            if (param(0, 0) == 0)
                s.tabs[std::size_t(s.x)] = false;
            else if (param(0, 0) == 3)
                std::fill(s.tabs.begin(), s.tabs.end(), false);
            break;
        case U'h':
        case U'l':
            for (std::size_t i = 0; i < groups.size(); ++i) {
                const int mode = param(i, 0);
                if (mode == 4)
                    s.insertMode = final == U'h';
                else if (mode == 20)
                    s.newlineMode = final == U'h';
            }
            break;
        case U'm':
            selectGraphicRendition(s);
            break;
        case U'n':
            if (param(0, 0) == 5) {
                responses += QByteArrayLiteral("\x1b[0n");
            } else if (param(0, 0) == 6) {
                const int row = s.originMode ? s.y - s.top : s.y;
                responses += QStringLiteral("\x1b[%1;%2R").arg(row + 1).arg(s.x + 1).toLatin1();
            }
            break;
        case U'r': {
            const int bottomRow = param(1, 0) <= 0 ? s.rowCount : std::min(param(1, 0), s.rowCount);
            s.setMargins(count(0) - 1, bottomRow - 1);
            break;
        }
        case U's':
            s.saveCursor();
            break;
        case U'u':
            s.restoreCursor();
            break;
        default:
            break; // window manipulation and the rest are ignored
        }
    }

    void softReset()
    {
        Screen &s = active();
        s.pen = Cell{};
        s.top = 0;
        s.bottom = s.rowCount - 1;
        s.insertMode = false;
        s.originMode = false;
        s.autowrap = true;
        s.appCursor = false;
        s.cursorHidden = false;
        s.charsets = Charsets{};
        s.saved = SavedCursor{};
    }

    void setPrivateMode(int mode, bool on)
    {
        Screen &s = active();
        switch (mode) {
        case 1:
            s.appCursor = on;
            break;
        case 6:
            s.originMode = on;
            s.cursorTo(0, 0);
            break;
        case 7:
            s.autowrap = on;
            if (!on)
                s.wrapPending = false;
            break;
        case 25:
            s.cursorHidden = !on;
            break;
        case 47:
        case 1047:
        case 1049:
            setAlternate(on);
            break;
        case 1048:
            if (on)
                s.saveCursor();
            else
                s.restoreCursor();
            break;
        default:
            break; // mouse reporting, bracketed paste and friends
        }
    }

    static Color extendedColor(const std::vector<std::vector<int>> &all, std::size_t &i)
    {
        const std::vector<int> &head = all[i];
        if (head.size() > 1) {
            // Colon form: 38:5:n, 38:2:r:g:b or 38:2:colourspace:r:g:b.
            if (head[1] == 5 && head.size() > 2)
                return Color::indexed(std::max(0, head[2]));
            if (head[1] == 2) {
                const std::size_t base = head.size() >= 6 ? 3 : 2;
                if (head.size() >= base + 3)
                    return Color::rgb(std::max(0, head[base]), std::max(0, head[base + 1]),
                                      std::max(0, head[base + 2]));
            }
            return {};
        }
        const auto value = [&](std::size_t at) {
            return at < all.size() && !all[at].empty() ? std::max(0, all[at][0]) : 0;
        };
        const int kind = value(i + 1);
        if (kind == 5) {
            const Color colour = Color::indexed(value(i + 2));
            i += 2;
            return colour;
        }
        if (kind == 2) {
            const Color colour = Color::rgb(value(i + 2), value(i + 3), value(i + 4));
            i += 4;
            return colour;
        }
        i += 1;
        return {};
    }

    void selectGraphicRendition(Screen &s)
    {
        Cell &pen = s.pen;
        for (std::size_t i = 0; i < groups.size(); ++i) {
            const int p = groups[i].empty() || groups[i][0] < 0 ? 0 : groups[i][0];
            switch (p) {
            case 0:
                pen = Cell{};
                break;
            case 1:
                pen.attrs |= Cell::Bold;
                break;
            case 3:
                pen.attrs |= Cell::Italic;
                break;
            case 4:
                if (groups[i].size() > 1 && groups[i][1] == 0)
                    pen.attrs &= quint8(~Cell::Underline);
                else
                    pen.attrs |= Cell::Underline;
                break;
            case 5:
            case 6:
                pen.attrs |= Cell::Blink;
                break;
            case 7:
                pen.attrs |= Cell::Reverse;
                break;
            case 9:
                pen.attrs |= Cell::Strikethrough;
                break;
            case 21:
                pen.attrs |= Cell::Underline;
                break;
            case 22:
                pen.attrs &= quint8(~Cell::Bold);
                break;
            case 23:
                pen.attrs &= quint8(~Cell::Italic);
                break;
            case 24:
                pen.attrs &= quint8(~Cell::Underline);
                break;
            case 25:
                pen.attrs &= quint8(~Cell::Blink);
                break;
            case 27:
                pen.attrs &= quint8(~Cell::Reverse);
                break;
            case 29:
                pen.attrs &= quint8(~Cell::Strikethrough);
                break;
            case 38:
                pen.fg = extendedColor(groups, i);
                break;
            case 39:
                pen.fg = Color{};
                break;
            case 48:
                pen.bg = extendedColor(groups, i);
                break;
            case 49:
                pen.bg = Color{};
                break;
            default:
                if (p >= 30 && p <= 37)
                    pen.fg = Color::indexed(p - 30);
                else if (p >= 40 && p <= 47)
                    pen.bg = Color::indexed(p - 40);
                else if (p >= 90 && p <= 97)
                    pen.fg = Color::indexed(p - 90 + 8);
                else if (p >= 100 && p <= 107)
                    pen.bg = Color::indexed(p - 100 + 8);
                break; // dim, conceal and the rest are ignored
            }
        }
    }

    // -- views -------------------------------------------------------------

    Line padded(const Line &line) const
    {
        Line out = line;
        out.resize(std::size_t(primary.cols), Cell{});
        return out;
    }

    // The line `index` of history + screen (or of the alternate screen).
    const Line &textLine(int index) const
    {
        if (altActive)
            return alternate.buf[std::size_t(index)];
        const int historyCount = int(primary.history.size());
        if (index < historyCount)
            return primary.history[std::size_t(index)];
        return primary.buf[std::size_t(index - historyCount)];
    }

    int textLineCount() const
    {
        return altActive ? alternate.rowCount : int(primary.history.size()) + primary.rowCount;
    }
};

Terminal::Terminal(int columns, int rows, int scrollback, CharacterSet charset, bool stripHighBit)
    : d(std::make_unique<Impl>(std::max(1, columns), std::max(1, rows), scrollback, charset,
                               stripHighBit))
{
}

Terminal::~Terminal() = default;

int Terminal::columns() const { return d->primary.cols; }
int Terminal::rows() const { return d->primary.rowCount; }
int Terminal::scrollback() const { return d->scrollback; }
CharacterSet Terminal::charset() const { return d->decoder.charset(); }
void Terminal::setCharset(CharacterSet charset) { d->decoder.setCharset(charset); }
bool Terminal::stripHighBit() const { return d->decoder.stripHighBit(); }
void Terminal::setStripHighBit(bool strip) { d->decoder.setStripHighBit(strip); }
bool Terminal::alternateScreen() const { return d->altActive; }
bool Terminal::applicationCursorKeys() const { return d->active().appCursor; }
quint64 Terminal::revision() const { return d->revision; }

void Terminal::feed(QByteArrayView data)
{
    if (data.isEmpty())
        return;
    d->feedCodePoints(d->decoder.decode(data));
}

void Terminal::feedText(const QString &text)
{
    if (text.isEmpty())
        return;
    const QList<uint> ucs4 = text.toUcs4();
    d->feedCodePoints(std::u32string(ucs4.begin(), ucs4.end()));
}

void Terminal::reset()
{
    d->primary.reset();
    d->primary.history.clear();
    d->alternate.reset();
    d->decoder.reset();
    d->state = Impl::State::Ground;
    d->clearParams();
    d->altActive = false;
    d->scrollOffset = 0;
    ++d->revision;
}

void Terminal::resize(int columns, int rows)
{
    columns = std::max(1, columns);
    rows = std::max(1, rows);
    if (columns == this->columns() && rows == this->rows())
        return;
    d->primary.resize(columns, rows);
    d->alternate.resize(columns, rows);
    d->scrollOffset = std::min(d->scrollOffset, d->maxScrollOffset());
    ++d->revision;
}

QByteArray Terminal::takeResponses()
{
    QByteArray out;
    out.swap(d->responses);
    return out;
}

int Terminal::totalLines() const
{
    return int(d->primary.history.size()) + rows();
}

int Terminal::scrollOffset() const { return d->scrollOffset; }

void Terminal::scrollTo(int offset)
{
    // The alternate screen has no history to scroll through.
    if (d->altActive)
        return;
    d->scrollOffset = std::clamp(offset, 0, d->maxScrollOffset());
    ++d->revision;
}

void Terminal::scrollBy(int delta) { scrollTo(d->scrollOffset + delta); }
void Terminal::scrollPage(int pages) { scrollBy(pages * rows()); }
void Terminal::scrollToTop() { scrollTo(d->maxScrollOffset()); }
void Terminal::follow() { scrollTo(0); }

Snapshot Terminal::snapshot() const
{
    Snapshot snap;
    snap.columns = columns();
    snap.rows = rows();
    snap.alternateScreen = d->altActive;
    snap.scrollOffset = d->scrollOffset;
    snap.totalLines = totalLines();
    snap.revision = d->revision;
    if (d->altActive || d->scrollOffset == 0) {
        const Screen &screen = d->active();
        snap.lines = screen.buf;
        snap.cursor = {screen.x, screen.y, screen.cursorHidden};
        return snap;
    }
    // Compose the window from history plus the live screen.  The cursor
    // lives on the live screen, which is not in view.
    const int start = std::max(0, totalLines() - rows() - d->scrollOffset);
    snap.lines.reserve(std::size_t(rows()));
    for (int row = 0; row < rows(); ++row)
        snap.lines.push_back(d->padded(d->textLine(start + row)));
    snap.cursor = {0, 0, true};
    return snap;
}

QStringList Terminal::allTextLines() const
{
    QStringList lines;
    const int count = d->textLineCount();
    lines.reserve(count);
    for (int i = 0; i < count; ++i)
        lines << lineText(d->textLine(i));
    return lines;
}

std::vector<int> Terminal::textColumns(int line) const
{
    std::vector<int> columns;
    if (line < 0 || line >= d->textLineCount())
        return columns;
    const Line &cells = d->textLine(line);
    int column = 0;
    for (const Cell &cell : cells) {
        if (!cell.has(Cell::WideTail)) {
            const int units = cell.ch > 0xFFFF ? 2 : 1;
            for (int u = 0; u < units; ++u)
                columns.push_back(column);
        }
        ++column;
    }
    columns.push_back(column);
    return columns;
}

QStringList Terminal::textSlice(int start, int end) const
{
    const QStringList lines = allTextLines();
    start = std::clamp(start, 0, int(lines.size()));
    end = std::clamp(end, start, int(lines.size()));
    return lines.mid(start, end - start);
}

int Terminal::historySize() const
{
    return int(d->primary.history.size());
}

Line Terminal::historyLine(int index) const
{
    return d->primary.history.at(std::size_t(index));
}

} // namespace portside
