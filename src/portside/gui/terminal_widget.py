"""The terminal display (PRD F7, F8, F10).

A custom painted widget rather than a text widget: per-cell colours, the block
cursor, the alternate screen and cell-accurate selection all fight a
``QPlainTextEdit``, and painting cells directly is what makes the 60 fps cap
meaningful at 921,600 baud.
"""

from __future__ import annotations

from dataclasses import dataclass

from PySide6.QtCore import QPoint, QRect, Qt, QTimer, Signal
from PySide6.QtGui import (
    QFont,
    QFontDatabase,
    QFontMetricsF,
    QKeyEvent,
    QMouseEvent,
    QPainter,
    QPaintEvent,
    QResizeEvent,
    QWheelEvent,
)
from PySide6.QtWidgets import QWidget

from portside.emulation.find import Match
from portside.emulation.hexview import ViewMode, hex_rows
from portside.emulation.terminal import Cell, Snapshot, Terminal
from portside.gui.palette import TerminalPalette, ThemeName, theme

__all__ = ["DEFAULT_FONT_SIZE", "TerminalWidget"]

#: PRD risk register: repaint at most 60 times a second, however fast data comes.
FRAME_INTERVAL_MS = 16
DEFAULT_FONT_SIZE = 11
#: How much raw traffic the hex view can page back through.
HEX_BUFFER_BYTES = 256 * 1024


@dataclass(frozen=True, slots=True)
class CellPosition:
    """A cell in the visible grid."""

    row: int
    column: int

    def __lt__(self, other: CellPosition) -> bool:
        return (self.row, self.column) < (other.row, other.column)


class TerminalWidget(QWidget):
    """Paints a :class:`~portside.emulation.terminal.Snapshot`."""

    #: Emitted when the user types; the owner sends the bytes to the device.
    keyPressed = Signal(QKeyEvent)
    #: Emitted when the selection changes (empty string when cleared).
    selectionChanged = Signal(str)
    #: Emitted when the visible geometry changes, in character cells.
    resizedCells = Signal(int, int)

    def __init__(
        self,
        terminal: Terminal,
        *,
        palette_name: ThemeName = ThemeName.DARK,
        font_family: str = "",
        font_size: int = DEFAULT_FONT_SIZE,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.terminal = terminal
        self._palette: TerminalPalette = theme(palette_name)
        self._view_mode = ViewMode.TEXT
        self._matches: tuple[Match, ...] = ()
        self._raw = bytearray()
        self._raw_origin = 0
        self._last_revision = -1
        self._selection_anchor: CellPosition | None = None
        self._selection_head: CellPosition | None = None
        self.select_to_copy = False

        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setAttribute(Qt.WidgetAttribute.WA_OpaquePaintEvent, True)
        self.setCursor(Qt.CursorShape.IBeamCursor)
        self.setAccessibleName("Terminal output")
        self.setAccessibleDescription(
            "Serial terminal display. Type to send characters to the device."
        )

        self._font = QFont()
        self.set_font(font_family, font_size)

        # One timer for the whole widget: repaint only when the emulator moved.
        self._timer = QTimer(self)
        self._timer.setInterval(FRAME_INTERVAL_MS)
        self._timer.timeout.connect(self._tick)
        self._timer.start()

    # -- appearance --------------------------------------------------------

    def set_font(self, family: str, size: int) -> None:
        """Use ``family`` at ``size`` points; falls back to the system monospace."""
        font = (
            QFont(family, size)
            if family
            else QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont)
        )
        font.setPointSize(size)
        font.setStyleHint(QFont.StyleHint.Monospace)
        font.setFixedPitch(True)
        self._font = font
        metrics = QFontMetricsF(font)
        # horizontalAdvance of a wide glyph keeps the grid stable across fonts.
        self._cell_width = max(1.0, metrics.horizontalAdvance("W"))
        self._cell_height = max(1.0, metrics.height())
        self._ascent = metrics.ascent()
        self._resize_terminal()
        self.update()

    @property
    def font_family(self) -> str:
        """Current font family."""
        return self._font.family()

    @property
    def font_size(self) -> int:
        """Current font size in points."""
        return self._font.pointSize()

    def set_theme(self, name: ThemeName) -> None:
        """Switch palette (F8); the chrome switches with it."""
        self._palette = theme(name)
        self.update()

    @property
    def palette_name(self) -> ThemeName:
        """Name of the palette in use."""
        return self._palette.name

    @property
    def cell_size(self) -> tuple[float, float]:
        """Width and height of one character cell, in pixels."""
        return self._cell_width, self._cell_height

    # -- data --------------------------------------------------------------

    def feed(self, data: bytes) -> None:
        """Hand received bytes to the emulator and the hex buffer."""
        self.terminal.feed(data)
        self._raw += data
        if len(self._raw) > HEX_BUFFER_BYTES:
            dropped = len(self._raw) - HEX_BUFFER_BYTES
            del self._raw[:dropped]
            self._raw_origin += dropped

    @property
    def view_mode(self) -> ViewMode:
        """Text or hex (F10)."""
        return self._view_mode

    def set_view_mode(self, mode: ViewMode) -> None:
        """Switch between the terminal and a hex dump (F10)."""
        self._view_mode = mode
        self.update()

    def set_matches(self, matches: tuple[Match, ...]) -> None:
        """Highlight search results (F8)."""
        self._matches = matches
        self.update()

    def _tick(self) -> None:
        if self.terminal.revision != self._last_revision:
            self._last_revision = self.terminal.revision
            self.update()

    # -- geometry ----------------------------------------------------------

    def _resize_terminal(self) -> None:
        """Match the emulator to the widget, so the device wraps where we do."""
        columns = max(1, int(self.width() / self._cell_width))
        rows = max(1, int(self.height() / self._cell_height))
        if (columns, rows) != (self.terminal.columns, self.terminal.rows):
            self.terminal.resize(columns, rows)
            self.resizedCells.emit(columns, rows)

    def resizeEvent(self, event: QResizeEvent) -> None:
        super().resizeEvent(event)
        self._resize_terminal()

    def _cell_at(self, point: QPoint) -> CellPosition:
        row = max(0, min(int(point.y() / self._cell_height), self.terminal.rows - 1))
        column = max(0, min(int(point.x() / self._cell_width), self.terminal.columns - 1))
        return CellPosition(row=row, column=column)

    # -- painting ----------------------------------------------------------

    def paintEvent(self, event: QPaintEvent) -> None:
        painter = QPainter(self)
        painter.setFont(self._font)
        painter.fillRect(self.rect(), self._palette.bg("default"))
        if self._view_mode is ViewMode.TEXT:
            self._paint_screen(painter)
        else:
            self._paint_hex(painter)
        painter.end()

    def _paint_screen(self, painter: QPainter) -> None:
        snapshot = self.terminal.snapshot()
        first_line = max(0, snapshot.total_lines - snapshot.rows - snapshot.scroll_offset)
        highlights = self._highlight_map(first_line)
        selection = self._selection_range()

        for row, line in enumerate(snapshot.lines):
            for column, cell in enumerate(line):
                selected = selection is not None and self._in_selection(
                    CellPosition(row, column), selection
                )
                matched = column in highlights.get(row, ())
                self._paint_cell(painter, row, column, cell, selected=selected, matched=matched)
        self._paint_cursor(painter, snapshot)

    def _paint_cell(
        self,
        painter: QPainter,
        row: int,
        column: int,
        cell: Cell,
        *,
        selected: bool,
        matched: bool,
    ) -> None:
        rect = QRect(
            int(column * self._cell_width),
            int(row * self._cell_height),
            int(self._cell_width + 1),
            int(self._cell_height),
        )
        foreground = self._palette.fg(cell.fg)
        background = self._palette.bg(cell.bg)
        if cell.reverse:
            foreground, background = background, foreground
        if matched:
            background = self._palette.colour("", default=self._palette.match)
            foreground = self._palette.bg("default")
        if selected:
            background = self._palette.colour("", default=self._palette.selection)

        if background != self._palette.bg("default"):
            painter.fillRect(rect, background)
        if cell.char == " ":
            return
        font = QFont(self._font)
        font.setBold(cell.bold)
        font.setItalic(cell.italics)
        font.setUnderline(cell.underscore)
        font.setStrikeOut(cell.strikethrough)
        painter.setFont(font)
        painter.setPen(foreground)
        painter.drawText(
            QPoint(int(column * self._cell_width), int(row * self._cell_height + self._ascent)),
            cell.char,
        )

    def _paint_cursor(self, painter: QPainter, snapshot: Snapshot) -> None:
        if snapshot.cursor.hidden or not snapshot.following:
            return
        rect = QRect(
            int(snapshot.cursor.x * self._cell_width),
            int(snapshot.cursor.y * self._cell_height),
            max(1, int(self._cell_width)),
            int(self._cell_height),
        )
        cursor_colour = self._palette.colour("", default=self._palette.cursor)
        if self.hasFocus():
            painter.fillRect(rect, cursor_colour)
        else:
            painter.setPen(cursor_colour)
            painter.drawRect(rect.adjusted(0, 0, -1, -1))

    def _paint_hex(self, painter: QPainter) -> None:
        rows = max(1, int(self.height() / self._cell_height))
        visible = self._raw[-rows * 16 :]
        offset = self._raw_origin + len(self._raw) - len(visible)
        painter.setPen(self._palette.fg("default"))
        for index, row in enumerate(hex_rows(bytes(visible), offset=offset, width=16)):
            painter.drawText(QPoint(0, int(index * self._cell_height + self._ascent)), row.render())

    def _highlight_map(self, first_line: int) -> dict[int, set[int]]:
        highlights: dict[int, set[int]] = {}
        for match in self._matches:
            row = match.line - first_line
            if 0 <= row < self.terminal.rows:
                highlights.setdefault(row, set()).update(range(match.start, match.end))
        return highlights

    # -- selection and clipboard (F8) -------------------------------------

    def _selection_range(self) -> tuple[CellPosition, CellPosition] | None:
        if self._selection_anchor is None or self._selection_head is None:
            return None
        start, end = self._selection_anchor, self._selection_head
        return (start, end) if start < end else (end, start)

    @staticmethod
    def _in_selection(position: CellPosition, selection: tuple[CellPosition, CellPosition]) -> bool:
        start, end = selection
        if position.row < start.row or position.row > end.row:
            return False
        if position.row == start.row and position.column < start.column:
            return False
        return not (position.row == end.row and position.column > end.column)

    def selected_text(self) -> str:
        """The current selection as text, joined with newlines across rows."""
        selection = self._selection_range()
        if selection is None:
            return ""
        start, end = selection
        lines = self.terminal.snapshot().lines
        collected: list[str] = []
        for row in range(start.row, min(end.row, len(lines) - 1) + 1):
            line = lines[row]
            first = start.column if row == start.row else 0
            last = end.column if row == end.row else len(line) - 1
            collected.append("".join(cell.char for cell in line[first : last + 1]).rstrip())
        return "\n".join(collected)

    def clear_selection(self) -> None:
        """Drop the selection."""
        self._selection_anchor = None
        self._selection_head = None
        self.selectionChanged.emit("")
        self.update()

    def copy_selection(self) -> str:
        """Copy the selection to the system clipboard (F8).  Returns the text."""
        text = self.selected_text()
        if text:
            from PySide6.QtWidgets import QApplication

            clipboard = QApplication.clipboard()
            if clipboard is not None:
                clipboard.setText(text)
        return text

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self._selection_anchor = self._cell_at(event.position().toPoint())
            self._selection_head = self._selection_anchor
            self.update()
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._selection_anchor is not None:
            self._selection_head = self._cell_at(event.position().toPoint())
            self.update()
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        if self._selection_anchor is not None:
            text = self.selected_text()
            self.selectionChanged.emit(text)
            if text and self.select_to_copy:
                self.copy_selection()
        super().mouseReleaseEvent(event)

    # -- scrollback (F8) ---------------------------------------------------

    def wheelEvent(self, event: QWheelEvent) -> None:
        steps = event.angleDelta().y() // 120
        if steps:
            self.terminal.scroll_by(steps * 3)
            self.update()
        event.accept()

    def scroll_lines(self, delta: int) -> None:
        """Scroll the view by ``delta`` lines; positive goes back in time."""
        self.terminal.scroll_by(delta)
        self.update()

    def scroll_pages(self, pages: int) -> None:
        """Scroll by whole screens."""
        self.terminal.scroll_page(pages)
        self.update()

    def follow_tail(self) -> None:
        """Return to the live output."""
        self.terminal.follow()
        self.update()

    # -- keyboard ----------------------------------------------------------

    def keyPressEvent(self, event: QKeyEvent) -> None:
        """Hand every keystroke to the owner, which decides what goes on the wire."""
        self.keyPressed.emit(event)
        event.accept()
