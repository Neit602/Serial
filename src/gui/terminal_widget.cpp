#include "gui/terminal_widget.h"

#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <set>

namespace portside::gui {

TerminalWidget::TerminalWidget(Terminal *terminal, ThemeName palette, const QString &fontFamily,
                               int fontSize, QWidget *parent)
    : QWidget(parent)
    , m_terminal(terminal)
    , m_palette(&theme(palette))
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setCursor(Qt::IBeamCursor);
    setAccessibleName(tr("Terminal output"));
    setAccessibleDescription(tr("Serial terminal display. Type to send characters to the device."));
    setMinimumSize(80, 40);

    setTerminalFont(fontFamily, fontSize);

    // One timer for the whole widget: repaint only when the emulator moved.
    m_timer = new QTimer(this);
    m_timer->setInterval(kFrameIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &TerminalWidget::tick);
    m_timer->start();
}

// -- appearance ---------------------------------------------------------------

void TerminalWidget::setTerminalFont(const QString &family, int size)
{
    QFont font = family.isEmpty() || !QFontDatabase::hasFamily(family)
                     ? QFontDatabase::systemFont(QFontDatabase::FixedFont)
                     : QFont(family);
    font.setPointSize(size > 0 ? size : kDefaultFontSize);
    font.setStyleHint(QFont::Monospace, QFont::PreferDefault);
    font.setFixedPitch(true);
    // Kerning would pull glyphs off the cell grid.
    font.setKerning(false);
    m_font = font;
    for (int i = 0; i < 4; ++i) {
        m_variants[i] = font;
        m_variants[i].setBold((i & 1) != 0);
        m_variants[i].setItalic((i & 2) != 0);
    }
    const QFontMetricsF metrics(font);
    // The advance of a wide glyph keeps the grid stable across fonts.
    m_cellWidth = std::max(1.0, metrics.horizontalAdvance(QLatin1Char('W')));
    m_cellHeight = std::max(1.0, metrics.height());
    m_ascent = metrics.ascent();
    resizeTerminal();
    m_dirty = true;
    update();
}

void TerminalWidget::setTheme(ThemeName name)
{
    m_palette = &theme(name);
    m_dirty = true;
    update();
}

const QFont &TerminalWidget::fontFor(quint8 attributes)
{
    const int index = ((attributes & Cell::Bold) ? 1 : 0) | ((attributes & Cell::Italic) ? 2 : 0);
    return m_variants[index];
}

// -- data -------------------------------------------------------------------

void TerminalWidget::feed(const QByteArray &raw, const QByteArray &display)
{
    m_terminal->feed(display);
    m_raw += raw;
    if (m_raw.size() > kHexBufferBytes) {
        const qsizetype dropped = m_raw.size() - kHexBufferBytes;
        m_raw.remove(0, dropped);
        m_rawOrigin += dropped;
    }
    if (m_viewMode != ViewMode::Text)
        m_dirty = true;
}

void TerminalWidget::setViewMode(ViewMode mode)
{
    m_viewMode = mode;
    m_dirty = true;
    update();
}

void TerminalWidget::setMatches(const QList<CellMatch> &matches)
{
    m_matches = matches;
    m_dirty = true;
    update();
}

void TerminalWidget::tick()
{
    if (m_terminal->revision() != m_lastRevision || m_dirty) {
        m_lastRevision = m_terminal->revision();
        m_dirty = false;
        update();
    }
}

// -- geometry -----------------------------------------------------------------

void TerminalWidget::resizeTerminal()
{
    // Match the emulator to the widget, so the device wraps where we do.
    const int columns = std::max(1, int(width() / m_cellWidth));
    const int rows = std::max(1, int(height() / m_cellHeight));
    if (columns != m_terminal->columns() || rows != m_terminal->rows()) {
        m_terminal->resize(columns, rows);
        Q_EMIT resizedCells(columns, rows);
    }
}

void TerminalWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    resizeTerminal();
}

CellPosition TerminalWidget::cellAt(const QPointF &point) const
{
    const int row = std::clamp(int(point.y() / m_cellHeight), 0, m_terminal->rows() - 1);
    const int column = std::clamp(int(point.x() / m_cellWidth), 0, m_terminal->columns() - 1);
    return {row, column};
}

// -- painting -------------------------------------------------------------------

void TerminalWidget::paintEvent(QPaintEvent *)
{
    ++m_paintCount;
    QPainter painter(this);
    painter.fillRect(rect(), m_palette->background);
    if (m_viewMode == ViewMode::Text)
        paintScreen(painter);
    else
        paintHex(painter);
}

void TerminalWidget::paintScreen(QPainter &painter)
{
    const Snapshot snapshot = m_terminal->snapshot();
    const int firstLine = snapshot.alternateScreen
                              ? 0
                              : std::max(0, snapshot.totalLines - snapshot.rows - snapshot.scrollOffset);

    // Which visible cells a search hit covers, row by row.
    std::vector<std::set<int>> highlights(std::size_t(snapshot.rows));
    for (const CellMatch &match : std::as_const(m_matches)) {
        const int row = match.line - firstLine;
        if (row < 0 || row >= snapshot.rows)
            continue;
        for (int column = match.first; column < match.last; ++column)
            highlights[std::size_t(row)].insert(column);
    }
    const auto selection = selectionRange();
    const auto inSelection = [&](int row, int column) {
        if (!selection)
            return false;
        const CellPosition here{row, column};
        return !(here < selection->first) && !(selection->second < here);
    };

    struct Style
    {
        QColor fg;
        QColor bg;
        quint8 attrs;
    };
    const auto styleOf = [&](const Cell &cell, int row, int column) {
        QColor fg = m_palette->fg(cell.fg);
        QColor bg = m_palette->bg(cell.bg);
        if (cell.has(Cell::Reverse))
            std::swap(fg, bg);
        if (highlights[std::size_t(row)].count(column) != 0) {
            bg = m_palette->match;
            fg = m_palette->background;
        }
        if (inSelection(row, column))
            bg = m_palette->selection;
        return Style{fg, bg, cell.attrs};
    };

    const int columns = snapshot.columns;
    for (int row = 0; row < snapshot.rows && row < int(snapshot.lines.size()); ++row) {
        const Line &line = snapshot.lines[std::size_t(row)];
        const double y = row * m_cellHeight;
        std::vector<Style> styles;
        styles.reserve(std::size_t(columns));
        for (int column = 0; column < columns && column < int(line.size()); ++column)
            styles.push_back(styleOf(line[std::size_t(column)], row, column));

        // Backgrounds, merged into runs.
        for (int column = 0; column < int(styles.size());) {
            const QColor bg = styles[std::size_t(column)].bg;
            int end = column + 1;
            while (end < int(styles.size()) && styles[std::size_t(end)].bg == bg)
                ++end;
            if (bg != m_palette->background)
                painter.fillRect(QRectF(column * m_cellWidth, y, (end - column) * m_cellWidth, m_cellHeight), bg);
            column = end;
        }

        // Glyphs: runs of ASCII in one style are drawn as one string; anything
        // else cell by cell, so fallback fonts cannot pull the grid apart.
        for (int column = 0; column < int(styles.size());) {
            const Cell &cell = line[std::size_t(column)];
            if (cell.has(Cell::WideTail) || cell.ch == U' ' || cell.ch == 0) {
                ++column;
                continue;
            }
            const Style &style = styles[std::size_t(column)];
            QString text;
            int end = column;
            if (cell.ch < 0x80 && !cell.has(Cell::Wide)) {
                while (end < int(styles.size())) {
                    const Cell &next = line[std::size_t(end)];
                    const Style &nextStyle = styles[std::size_t(end)];
                    if (next.ch >= 0x80 || next.has(Cell::Wide) || next.has(Cell::WideTail)
                        || nextStyle.fg != style.fg || nextStyle.attrs != style.attrs) {
                        break;
                    }
                    text += QLatin1Char(char(next.ch ? next.ch : U' '));
                    ++end;
                }
            } else {
                const char32_t c = cell.ch;
                text = QString::fromUcs4(&c, 1);
                end = column + 1;
            }
            QFont font = fontFor(style.attrs);
            if (style.attrs & (Cell::Underline | Cell::Strikethrough)) {
                font.setUnderline((style.attrs & Cell::Underline) != 0);
                font.setStrikeOut((style.attrs & Cell::Strikethrough) != 0);
            }
            painter.setFont(font);
            painter.setPen(style.fg);
            painter.drawText(QPointF(column * m_cellWidth, y + m_ascent), text);
            column = end;
        }
    }

    // The cursor: a block when focused, an outline when not.
    if (snapshot.cursor.hidden || !snapshot.following())
        return;
    const QRectF cursorRect(snapshot.cursor.x * m_cellWidth, snapshot.cursor.y * m_cellHeight,
                            m_cellWidth, m_cellHeight);
    if (hasFocus()) {
        painter.fillRect(cursorRect, m_palette->cursor);
        const Line &line = snapshot.lines[std::size_t(snapshot.cursor.y)];
        const Cell &under = line[std::size_t(std::min(snapshot.cursor.x, int(line.size()) - 1))];
        if (under.ch != U' ' && under.ch != 0 && !under.has(Cell::WideTail)) {
            const char32_t c = under.ch;
            painter.setFont(fontFor(under.attrs));
            painter.setPen(m_palette->background);
            painter.drawText(QPointF(cursorRect.x(), cursorRect.y() + m_ascent), QString::fromUcs4(&c, 1));
        }
    } else {
        painter.setPen(m_palette->cursor);
        painter.drawRect(cursorRect.adjusted(0.5, 0.5, -0.5, -0.5));
    }
}

void TerminalWidget::paintHex(QPainter &painter)
{
    const int rows = std::max(1, int(height() / m_cellHeight));
    const qint64 total = m_rawOrigin + m_raw.size();
    // Rows start on 16-byte boundaries, as in `hexdump -C`.
    const qint64 lastRow = total == 0 ? 0 : ((total - 1) / 16) * 16;
    const qint64 firstRow = std::max<qint64>(0, lastRow - qint64(rows - 1) * 16);
    const qint64 begin = std::max(firstRow, m_rawOrigin);
    const QByteArray visible = m_raw.mid(begin - m_rawOrigin);
    painter.setFont(m_variants[0]);
    painter.setPen(m_palette->foreground);
    int index = 0;
    for (const HexRow &row : hexRows(visible, begin, 16))
        painter.drawText(QPointF(0, index++ * m_cellHeight + m_ascent), row.render());
}

// -- selection and clipboard ----------------------------------------------------

std::optional<std::pair<CellPosition, CellPosition>> TerminalWidget::selectionRange() const
{
    if (!m_anchor || !m_head)
        return std::nullopt;
    if (*m_head < *m_anchor)
        return std::make_pair(*m_head, *m_anchor);
    return std::make_pair(*m_anchor, *m_head);
}

QString TerminalWidget::selectedText() const
{
    const auto selection = selectionRange();
    if (!selection)
        return {};
    const auto [start, end] = *selection;
    const Snapshot snapshot = m_terminal->snapshot();
    QStringList collected;
    const int lastRow = std::min(end.row, int(snapshot.lines.size()) - 1);
    for (int row = start.row; row <= lastRow; ++row) {
        const Line &line = snapshot.lines[std::size_t(row)];
        const int first = row == start.row ? start.column : 0;
        const int last = row == end.row ? end.column : int(line.size()) - 1;
        Line part;
        for (int column = first; column <= last && column < int(line.size()); ++column)
            part.push_back(line[std::size_t(column)]);
        collected << lineText(part);
    }
    return collected.join(QLatin1Char('\n'));
}

void TerminalWidget::selectCells(CellPosition anchor, CellPosition head)
{
    m_anchor = anchor;
    m_head = head;
    m_dirty = true;
    update();
}

void TerminalWidget::clearSelection()
{
    m_anchor.reset();
    m_head.reset();
    Q_EMIT selectionChanged(QString());
    m_dirty = true;
    update();
}

QString TerminalWidget::copySelection()
{
    const QString text = selectedText();
    if (!text.isEmpty()) {
        if (QClipboard *clipboard = QApplication::clipboard())
            clipboard->setText(text);
    }
    return text;
}

void TerminalWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_anchor = cellAt(event->position());
        m_head = m_anchor;
        m_dirty = true;
        update();
    }
    QWidget::mousePressEvent(event);
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_anchor && (event->buttons() & Qt::LeftButton)) {
        m_head = cellAt(event->position());
        m_dirty = true;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_anchor && event->button() == Qt::LeftButton) {
        m_head = cellAt(event->position());
        const QString text = selectedText();
        Q_EMIT selectionChanged(text);
        if (!text.isEmpty() && m_selectToCopy)
            copySelection();
    }
    QWidget::mouseReleaseEvent(event);
}

// -- scrollback -------------------------------------------------------------------

void TerminalWidget::wheelEvent(QWheelEvent *event)
{
    const int steps = event->angleDelta().y() / 120;
    if (steps != 0)
        scrollLines(steps * 3);
    event->accept();
}

void TerminalWidget::scrollLines(int delta)
{
    m_terminal->scrollBy(delta);
    update();
}

void TerminalWidget::scrollPages(int pages)
{
    m_terminal->scrollPage(pages);
    update();
}

void TerminalWidget::followTail()
{
    m_terminal->follow();
    update();
}

// -- keyboard ---------------------------------------------------------------------

void TerminalWidget::keyPressEvent(QKeyEvent *event)
{
    // Every keystroke goes to the owner, which decides what goes on the wire.
    Q_EMIT keyPressed(event);
    event->accept();
}

bool TerminalWidget::focusNextPrevChild(bool)
{
    // Tab and Backtab belong to the device, not to focus navigation.
    return false;
}

} // namespace portside::gui
