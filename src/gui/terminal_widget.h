#pragma once

// The terminal display (PRD F7, F8, F10).
//
// A custom painted widget rather than a text widget: per-cell colours, the
// block cursor, the alternate screen and cell-accurate selection all fight a
// QPlainTextEdit, and painting cells directly is what makes the 60 fps cap
// meaningful at 921,600 baud (DECISIONS D24, D30).

#include "core/find.h"
#include "core/hexview.h"
#include "core/terminal.h"
#include "gui/palette.h"

#include <QFont>
#include <QList>
#include <QWidget>

#include <optional>

class QTimer;

namespace portside::gui {

inline constexpr int kFrameIntervalMs = 16; // at most 60 repaints a second
inline constexpr int kDefaultFontSize = 11;
// How much raw traffic the hex view can page back through.
inline constexpr qsizetype kHexBufferBytes = 256 * 1024;

// A cell in the visible grid.
struct CellPosition
{
    int row = 0;
    int column = 0;

    bool operator<(const CellPosition &other) const
    {
        return row < other.row || (row == other.row && column < other.column);
    }
    bool operator==(const CellPosition &) const = default;
};

// A find hit mapped onto cells: a line index and a [first, last) column range.
struct CellMatch
{
    int line = 0;
    int first = 0;
    int last = 0;
};

class TerminalWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TerminalWidget(Terminal *terminal, ThemeName palette = ThemeName::Dark,
                            const QString &fontFamily = {}, int fontSize = kDefaultFontSize,
                            QWidget *parent = nullptr);

    Terminal *terminal() const { return m_terminal; }

    // -- appearance --------------------------------------------------------

    // Use `family` at `size` points; empty falls back to the system monospace.
    void setTerminalFont(const QString &family, int size);
    QString fontFamily() const { return m_font.family(); }
    QFont terminalFont() const { return m_font; }
    int fontSize() const { return m_font.pointSize(); }
    void setTheme(ThemeName name);
    ThemeName paletteName() const { return m_palette->name; }
    QSizeF cellSize() const { return {m_cellWidth, m_cellHeight}; }

    // -- data --------------------------------------------------------------

    // Hand received bytes to the emulator and the hex buffer.
    void feed(const QByteArray &bytes) { feed(bytes, bytes); }
    // The same, when the screen should see a translated form (receive line
    // endings) while the hex view keeps the bytes exactly as received.
    void feed(const QByteArray &raw, const QByteArray &display);
    ViewMode viewMode() const { return m_viewMode; }
    void setViewMode(ViewMode mode);
    // Highlight search results (F8).
    void setMatches(const QList<CellMatch> &matches);
    // Repaints actually performed (for the batching test).
    int paintCount() const { return m_paintCount; }
    // Repaint now if the emulator moved (normally driven by the frame timer).
    void tick();

    // -- selection and clipboard (F8) -------------------------------------

    QString selectedText() const;
    void clearSelection();
    // Copy the selection to the system clipboard; returns the text.
    QString copySelection();
    bool selectToCopy() const { return m_selectToCopy; }
    void setSelectToCopy(bool enabled) { m_selectToCopy = enabled; }
    void selectCells(CellPosition anchor, CellPosition head);

    // -- scrollback (F8) ---------------------------------------------------

    // Scroll by `delta` lines; positive goes back in time.
    void scrollLines(int delta);
    void scrollPages(int pages);
    void followTail();

Q_SIGNALS:
    // The user typed; the owner decides what goes on the wire.
    void keyPressed(QKeyEvent *event);
    // The selection changed (empty when cleared).
    void selectionChanged(const QString &text);
    // The visible geometry changed, in character cells.
    void resizedCells(int columns, int rows);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    bool focusNextPrevChild(bool next) override;

private:
    void resizeTerminal();
    CellPosition cellAt(const QPointF &point) const;
    std::optional<std::pair<CellPosition, CellPosition>> selectionRange() const;
    void paintScreen(QPainter &painter);
    void paintHex(QPainter &painter);
    const QFont &fontFor(quint8 attributes);

    Terminal *m_terminal;
    const TerminalPalette *m_palette;
    ViewMode m_viewMode = ViewMode::Text;
    QList<CellMatch> m_matches;
    QByteArray m_raw;
    qint64 m_rawOrigin = 0;
    quint64 m_lastRevision = 0;
    std::optional<CellPosition> m_anchor;
    std::optional<CellPosition> m_head;
    bool m_selectToCopy = false;
    bool m_dirty = true;
    int m_paintCount = 0;

    QFont m_font;
    QFont m_variants[4]; // regular, bold, italic, bold italic
    double m_cellWidth = 8;
    double m_cellHeight = 16;
    double m_ascent = 12;
    QTimer *m_timer = nullptr;
};

} // namespace portside::gui
