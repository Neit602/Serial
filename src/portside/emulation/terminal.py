"""VT100/xterm screen model on top of pyte (PRD F7, F8).

Two things this adds over using pyte directly:

* **A real alternate screen.**  pyte records the 1049/1047/47 private modes but
  does not swap buffers, so a full-screen program (``vi``, ``less``, a switch's
  menu UI) would otherwise paint over the primary screen and pour its redraws
  into the scrollback.  :class:`Terminal` keeps two screens and routes bytes to
  whichever is active.
* **A scrollback view that stays where the user put it.**  Scrolling up pins
  the view to the content being read; new output does not yank it back to the
  bottom until the user says so.
* **DEC line drawing.**  pyte can translate the special graphics set but its
  stream never dispatches the ``ESC ( 0`` that selects it, so box-drawn menus
  and ``show`` tables come out as ``lqk``.  Those designations are handled here.

The class is UI-independent: it turns bytes into :class:`Snapshot` values, and
knows nothing about Textual, Qt or a terminal.
"""

from __future__ import annotations

from collections.abc import Callable, Iterable, Sequence
from dataclasses import dataclass
from typing import Any, Final

import pyte

from portside.emulation.decoder import CharacterSet, IncrementalTextDecoder

__all__ = [
    "DEFAULT_COLUMNS",
    "DEFAULT_ROWS",
    "DEFAULT_SCROLLBACK",
    "Cell",
    "Cursor",
    "Line",
    "Snapshot",
    "Terminal",
]

#: PRD F8: 10,000 lines of scrollback by default, configurable.
DEFAULT_SCROLLBACK: Final = 10_000
DEFAULT_COLUMNS: Final = 80
DEFAULT_ROWS: Final = 24

#: Private modes that mean "switch to the alternate screen".
_ALTERNATE_MODES: Final = frozenset({47, 1047, 1049})


@dataclass(frozen=True, slots=True)
class Cell:
    """One character cell with its attributes."""

    char: str = " "
    fg: str = "default"
    bg: str = "default"
    bold: bool = False
    italics: bool = False
    underscore: bool = False
    strikethrough: bool = False
    reverse: bool = False
    blink: bool = False

    @property
    def is_plain(self) -> bool:
        """True if nothing but the character matters (fast path for renderers)."""
        return (
            self.fg == "default"
            and self.bg == "default"
            and not (
                self.bold
                or self.italics
                or self.underscore
                or self.strikethrough
                or self.reverse
                or self.blink
            )
        )


Line = tuple[Cell, ...]

_BLANK = Cell()


@dataclass(frozen=True, slots=True)
class Cursor:
    """Cursor position and visibility."""

    x: int = 0
    y: int = 0
    hidden: bool = False


@dataclass(frozen=True, slots=True)
class Snapshot:
    """An immutable view of the screen, safe to hand to any UI thread."""

    lines: tuple[Line, ...]
    cursor: Cursor
    columns: int
    rows: int
    alternate_screen: bool
    scroll_offset: int
    total_lines: int
    revision: int

    @property
    def following(self) -> bool:
        """True when the view is pinned to the live tail."""
        return self.scroll_offset == 0

    def text(self) -> list[str]:
        """The snapshot as plain text lines, right-stripped."""
        return ["".join(cell.char for cell in line).rstrip() for line in self.lines]


class _Screen(pyte.HistoryScreen):
    """pyte screen that reports alternate-screen mode changes to its owner."""

    def __init__(
        self,
        columns: int,
        lines: int,
        history: int,
        on_alternate: Callable[[bool], None],
    ) -> None:
        super().__init__(columns, lines, history=history, ratio=0.5)
        self._on_alternate = on_alternate

    def set_mode(self, *modes: int, **kwargs: Any) -> None:
        if kwargs.get("private") and any(mode in _ALTERNATE_MODES for mode in modes):
            self._on_alternate(True)
        super().set_mode(*modes, **kwargs)

    def reset_mode(self, *modes: int, **kwargs: Any) -> None:
        if kwargs.get("private") and any(mode in _ALTERNATE_MODES for mode in modes):
            self._on_alternate(False)
        super().reset_mode(*modes, **kwargs)


class Terminal:
    """Byte stream in, screen model out.

    Args:
        columns / rows: screen geometry.
        scrollback: how many lines of history to keep (F8).
        charset / strip_high_bit: decoding options (F10, F11).
    """

    def __init__(
        self,
        columns: int = DEFAULT_COLUMNS,
        rows: int = DEFAULT_ROWS,
        *,
        scrollback: int = DEFAULT_SCROLLBACK,
        charset: CharacterSet = CharacterSet.UTF8,
        strip_high_bit: bool = False,
    ) -> None:
        self._columns = max(1, columns)
        self._rows = max(1, rows)
        self._scrollback = max(0, scrollback)
        self._decoder = IncrementalTextDecoder(charset, strip_high_bit=strip_high_bit)

        self._alternate_active = False
        self._primary = _Screen(self._columns, self._rows, self._scrollback, self._set_alternate)
        # The alternate screen keeps no history: full-screen programs redraw
        # constantly and their frames are not scrollback anyone wants.
        self._alternate = _Screen(self._columns, self._rows, 0, self._set_alternate)
        self._primary_stream = pyte.Stream(self._primary)
        self._alternate_stream = pyte.Stream(self._alternate)

        self._scroll_offset = 0
        self._revision = 0
        #: Holds a charset designation split across two reads.
        self._pending_escape = ""

    # -- configuration -----------------------------------------------------

    @property
    def columns(self) -> int:
        """Screen width in cells."""
        return self._columns

    @property
    def rows(self) -> int:
        """Screen height in cells."""
        return self._rows

    @property
    def charset(self) -> CharacterSet:
        """Character set used to decode incoming bytes (F11)."""
        return self._decoder.charset

    def set_charset(self, charset: CharacterSet) -> None:
        """Switch character set from now on (F11)."""
        self._decoder.set_charset(charset)

    @property
    def strip_high_bit(self) -> bool:
        """Whether bit 7 is stripped from incoming bytes (F10)."""
        return self._decoder.strip_high_bit

    def set_strip_high_bit(self, strip: bool) -> None:
        """Turn high-bit stripping on or off (F10)."""
        self._decoder.set_strip_high_bit(strip)

    @property
    def scrollback(self) -> int:
        """Configured scrollback depth in lines (F8)."""
        return self._scrollback

    @property
    def alternate_screen(self) -> bool:
        """True while a full-screen program owns the display."""
        return self._alternate_active

    @property
    def revision(self) -> int:
        """Bumped on every change, so a renderer can skip idle repaints."""
        return self._revision

    # -- input -------------------------------------------------------------

    def feed(self, data: bytes) -> None:
        """Feed bytes received from the device."""
        if not data:
            return
        text = self._decoder.decode(data)
        if not text:
            return
        self.feed_text(text)

    def feed_text(self, text: str) -> None:
        """Feed already-decoded text (used by local echo and by tests)."""
        if not text:
            return
        before = self.total_lines
        self._feed_segments(text)
        self._after_change(before)

    def _feed_segments(self, text: str) -> None:
        """Feed text to pyte, applying charset designations in stream order.

        pyte implements ``define_charset`` but its stream never calls it, so
        without this every box-drawing character arrives as the ASCII letter it
        is encoded as (``lqk`` instead of ``┌─┐``).  The designations have to be
        applied *where they occur*: text before ``ESC ( 0`` is ASCII, text after
        it is line drawing, and a chunk can contain both.
        """
        buffer = self._pending_escape + text
        self._pending_escape = ""
        pending_text: list[str] = []
        index = 0
        length = len(buffer)

        def flush() -> None:
            if pending_text:
                self._active_stream().feed("".join(pending_text))
                pending_text.clear()

        while index < length:
            char = buffer[index]
            if char != "\x1b":
                pending_text.append(char)
                index += 1
                continue
            if index + 1 >= length:
                # ESC at the very end: hold it until the rest arrives.
                self._pending_escape = buffer[index:]
                break
            marker = buffer[index + 1]
            if marker not in "()":
                pending_text.append(char)
                index += 1
                continue
            if index + 2 >= length:
                self._pending_escape = buffer[index:]
                break
            flush()
            self._active_screen().define_charset(buffer[index + 2], marker)
            index += 3
        flush()

    def _active_screen(self) -> pyte.Screen:
        """The screen bytes are currently being drawn on."""
        return self._alternate if self._alternate_active else self._primary

    def _active_stream(self) -> pyte.Stream:
        """The stream feeding the active screen."""
        return self._alternate_stream if self._alternate_active else self._primary_stream

    def _after_change(self, lines_before: int) -> None:
        """Keep a scrolled-back view pinned to the same content."""
        self._revision += 1
        if self._scroll_offset:
            added = self.total_lines - lines_before
            if added > 0:
                self._scroll_offset = min(self._scroll_offset + added, self._max_scroll_offset())

    def _set_alternate(self, active: bool) -> None:
        """Enter or leave the alternate screen (called from the pyte screens)."""
        if active == self._alternate_active:
            return
        if active:
            # 1049 semantics: the alternate screen always starts clean.
            self._alternate.reset()
            self._alternate.resize(self._rows, self._columns)
        self._alternate_active = active
        self._scroll_offset = 0
        self._revision += 1

    def reset(self) -> None:
        """Clear the screen, the scrollback and any half-decoded character."""
        self._primary.reset()
        self._alternate.reset()
        self._primary.history.top.clear()
        self._primary.history.bottom.clear()
        self._decoder.reset()
        self._pending_escape = ""
        self._alternate_active = False
        self._scroll_offset = 0
        self._revision += 1

    def resize(self, columns: int, rows: int) -> None:
        """Resize the screen, keeping the scrollback."""
        columns = max(1, columns)
        rows = max(1, rows)
        if (columns, rows) == (self._columns, self._rows):
            return
        self._columns, self._rows = columns, rows
        self._primary.resize(rows, columns)
        self._alternate.resize(rows, columns)
        self._scroll_offset = min(self._scroll_offset, self._max_scroll_offset())
        self._revision += 1

    # -- scrollback (F8) ---------------------------------------------------

    @property
    def total_lines(self) -> int:
        """Scrollback lines plus the lines on screen."""
        return len(self._history_lines()) + self._rows

    @property
    def scroll_offset(self) -> int:
        """How many lines above the live tail the view is pinned."""
        return self._scroll_offset

    @property
    def following(self) -> bool:
        """True when the view follows new output."""
        return self._scroll_offset == 0

    def _max_scroll_offset(self) -> int:
        return max(0, self.total_lines - self._rows)

    def scroll_to(self, offset: int) -> None:
        """Pin the view ``offset`` lines above the tail (0 follows the tail)."""
        # The alternate screen has no history to scroll through.
        if self._alternate_active:
            return
        self._scroll_offset = max(0, min(offset, self._max_scroll_offset()))
        self._revision += 1

    def scroll_by(self, delta: int) -> None:
        """Scroll up (positive) or down (negative) by ``delta`` lines."""
        self.scroll_to(self._scroll_offset + delta)

    def scroll_page(self, pages: int) -> None:
        """Scroll by whole screens; positive is backwards in time."""
        self.scroll_by(pages * self._rows)

    def scroll_to_top(self) -> None:
        """Jump to the oldest line kept in the scrollback."""
        self.scroll_to(self._max_scroll_offset())

    def follow(self) -> None:
        """Return to the live tail and track new output."""
        self.scroll_to(0)

    # -- output ------------------------------------------------------------

    def snapshot(self) -> Snapshot:
        """An immutable picture of what should be on screen right now."""
        screen = self._alternate if self._alternate_active else self._primary
        if self._alternate_active or self._scroll_offset == 0:
            lines = tuple(self._line(screen.buffer[y]) for y in range(self._rows))
            cursor = Cursor(screen.cursor.x, screen.cursor.y, bool(screen.cursor.hidden))
        else:
            lines = self._scrolled_lines()
            # The cursor lives on the live screen, which is not in view.
            cursor = Cursor(0, 0, hidden=True)
        return Snapshot(
            lines=lines,
            cursor=cursor,
            columns=self._columns,
            rows=self._rows,
            alternate_screen=self._alternate_active,
            scroll_offset=self._scroll_offset,
            total_lines=self.total_lines,
            revision=self._revision,
        )

    def _scrolled_lines(self) -> tuple[Line, ...]:
        """Compose the visible window from history plus the live screen."""
        history = self._history_lines()
        screen_lines = [self._primary.buffer[y] for y in range(self._rows)]
        combined = [*history, *screen_lines]
        start = max(0, len(combined) - self._rows - self._scroll_offset)
        window = combined[start : start + self._rows]
        while len(window) < self._rows:  # pragma: no cover - only on a tiny history
            window.append({})
        return tuple(self._line(line) for line in window)

    def _history_lines(self) -> list[Any]:
        return list(self._primary.history.top)

    def _line(self, raw: Any) -> Line:
        """Convert one pyte buffer row into immutable cells."""
        cells: list[Cell] = []
        for x in range(self._columns):
            char = raw.get(x)
            if char is None:
                cells.append(_BLANK)
                continue
            cells.append(
                Cell(
                    char=char.data or " ",
                    fg=char.fg,
                    bg=char.bg,
                    bold=char.bold,
                    italics=char.italics,
                    underscore=char.underscore,
                    strikethrough=char.strikethrough,
                    reverse=char.reverse,
                    blink=char.blink,
                )
            )
        return tuple(cells)

    # -- text access (find, copy, logging) ---------------------------------

    def all_text_lines(self) -> list[str]:
        """Scrollback plus screen as plain text, oldest first (F8 find/copy)."""
        screen = self._alternate if self._alternate_active else self._primary
        history = [] if self._alternate_active else self._history_lines()
        lines = [*history, *(screen.buffer[y] for y in range(self._rows))]
        return ["".join(self._raw_text(line)).rstrip() for line in lines]

    def _raw_text(self, raw: Any) -> Iterable[str]:
        for x in range(self._columns):
            char = raw.get(x)
            yield char.data if char is not None and char.data else " "

    def text_slice(self, start: int, end: int) -> Sequence[str]:
        """Text lines in ``[start, end)`` counted from the oldest kept line."""
        lines = self.all_text_lines()
        return lines[max(0, start) : max(0, end)]
