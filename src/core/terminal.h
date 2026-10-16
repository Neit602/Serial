#pragma once

// VT100/xterm screen model (PRD F7, F8).
//
// Bytes in, cells out.  A self-contained emulator: a VT500-style parser
// driving two screens.  What it guarantees beyond "draws the text":
//
// * A real alternate screen.  Full-screen programs (vi, less, a switch's menu
//   UI) draw on their own screen, and their redraws never pour into the
//   scrollback (modes 47, 1047, 1049).
// * A scrollback view that stays where the user put it: scrolling up pins the
//   view to the content being read; new output does not yank it back.
// * DEC line drawing (ESC ( 0), so box-drawn menus and `show` tables render
//   as boxes rather than "lqk".
// * Resizing never loses output: rows that no longer fit move into the
//   scrollback, with their colours (DECISIONS D31).
//
// The class is UI-independent and single-threaded: it belongs to whichever
// thread feeds it (the GUI thread in the desktop app).

#include "core/decoder.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace portside {

// PRD F8: 10,000 lines of scrollback by default, configurable.
inline constexpr int kDefaultScrollback = 10'000;
inline constexpr int kDefaultColumns = 80;
inline constexpr int kDefaultRows = 24;

// A cell colour: the terminal default, one of the 256 indexed colours (0-15
// are the ANSI and bright colours a theme controls), or 24-bit RGB.
class Color
{
public:
    constexpr Color() = default;
    static constexpr Color indexed(int index) { return Color(kIndexed | quint32(index & 0xFF)); }
    static constexpr Color rgb(int r, int g, int b)
    {
        return Color(kRgb | (quint32(r & 0xFF) << 16) | (quint32(g & 0xFF) << 8) | quint32(b & 0xFF));
    }

    constexpr bool isDefault() const { return m_value == 0; }
    constexpr bool isIndexed() const { return (m_value & kTypeMask) == kIndexed; }
    constexpr bool isRgb() const { return (m_value & kTypeMask) == kRgb; }
    constexpr int index() const { return int(m_value & 0xFF); }
    constexpr quint32 rgbValue() const { return m_value & 0xFFFFFF; }

    constexpr bool operator==(const Color &) const = default;

private:
    static constexpr quint32 kTypeMask = 0xFF000000;
    static constexpr quint32 kIndexed = 0x01000000;
    static constexpr quint32 kRgb = 0x02000000;
    constexpr explicit Color(quint32 value)
        : m_value(value)
    {
    }
    quint32 m_value = 0;
};

// One character cell with its attributes.
struct Cell
{
    enum Attribute : quint8 {
        Bold = 0x01,
        Italic = 0x02,
        Underline = 0x04,
        Strikethrough = 0x08,
        Reverse = 0x10,
        Blink = 0x20,
        Wide = 0x40,     // first half of a double-width character
        WideTail = 0x80, // the cell a wide character spills into
    };

    char32_t ch = U' ';
    Color fg;
    Color bg;
    quint8 attrs = 0;

    bool has(Attribute attribute) const { return (attrs & attribute) != 0; }
    // True if nothing but the character matters (fast path for renderers).
    bool isPlain() const { return fg.isDefault() && bg.isDefault() && (attrs & 0x3F) == 0; }

    bool operator==(const Cell &) const = default;
};

using Line = std::vector<Cell>;

struct Cursor
{
    int x = 0;
    int y = 0;
    bool hidden = false;

    bool operator==(const Cursor &) const = default;
};

// A copy of what should be on screen right now.
struct Snapshot
{
    std::vector<Line> lines;
    Cursor cursor;
    int columns = 0;
    int rows = 0;
    bool alternateScreen = false;
    int scrollOffset = 0;
    int totalLines = 0;
    quint64 revision = 0;

    // True when the view is pinned to the live tail.
    bool following() const { return scrollOffset == 0; }
    // The snapshot as plain text lines, right-stripped.
    QStringList text() const;
};

// Plain text of one line: wide characters once, right-stripped.
QString lineText(const Line &line);

class Terminal
{
public:
    explicit Terminal(int columns = kDefaultColumns, int rows = kDefaultRows,
                      int scrollback = kDefaultScrollback,
                      CharacterSet charset = CharacterSet::Utf8, bool stripHighBit = false);
    ~Terminal();
    Terminal(const Terminal &) = delete;
    Terminal &operator=(const Terminal &) = delete;

    // -- configuration -----------------------------------------------------

    int columns() const;
    int rows() const;
    // Configured scrollback depth in lines (F8).
    int scrollback() const;
    CharacterSet charset() const;
    void setCharset(CharacterSet charset);
    bool stripHighBit() const;
    void setStripHighBit(bool strip);

    // True while a full-screen program owns the display.
    bool alternateScreen() const;
    // DECCKM: cursor keys should send ESC O A rather than ESC [ A.
    bool applicationCursorKeys() const;
    // Bumped on every change, so a renderer can skip idle repaints.
    quint64 revision() const;

    // -- input -------------------------------------------------------------

    // Feed bytes received from the device.
    void feed(QByteArrayView data);
    // Feed already-decoded text (local echo, tests).
    void feedText(const QString &text);
    // Clear the screen, the scrollback and any half-decoded character.
    void reset();
    // Resize, keeping the scrollback and what is on screen (D31).  Columns
    // are not reflowed: long lines are truncated, as on a VT100.
    void resize(int columns, int rows);

    // Answers the device asked for (cursor position reports, device
    // attributes).  The owner sends them back down the wire.
    QByteArray takeResponses();

    // -- scrollback (F8) ---------------------------------------------------

    // Scrollback lines plus the lines on screen.
    int totalLines() const;
    // How many lines above the live tail the view is pinned.
    int scrollOffset() const;
    bool following() const { return scrollOffset() == 0; }
    // Pin the view `offset` lines above the tail (0 follows the tail).
    void scrollTo(int offset);
    // Scroll up (positive) or down (negative).
    void scrollBy(int delta);
    // Scroll by whole screens; positive is backwards in time.
    void scrollPage(int pages);
    void scrollToTop();
    void follow();

    // -- output ------------------------------------------------------------

    Snapshot snapshot() const;
    // Scrollback plus screen as plain text, oldest first (find, copy).
    QStringList allTextLines() const;
    // The cell column of each UTF-16 unit of allTextLines()[line], plus one
    // entry for the position just past the end.  Maps a find match onto
    // cells when the line holds wide characters.
    std::vector<int> textColumns(int line) const;
    // Text lines in [start, end) counted from the oldest kept line.
    QStringList textSlice(int start, int end) const;
    // Lines currently held in the primary scrollback, oldest first.
    int historySize() const;
    Line historyLine(int index) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace portside
