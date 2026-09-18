"""Textual widgets for the Portside TUI (PRD F7, F8, F10, F22)."""

from __future__ import annotations

from rich.console import RenderableType
from rich.text import Text
from textual.reactive import reactive
from textual.widget import Widget

from portside.emulation.find import Match
from portside.emulation.hexview import ViewMode, hex_rows
from portside.emulation.terminal import Terminal
from portside.tui.render import snapshot_to_text

__all__ = ["StatusBar", "TerminalView"]

#: How much raw traffic the hex view can page back through.
HEX_BUFFER_BYTES = 256 * 1024


class TerminalView(Widget, can_focus=True):
    """Renders the emulator's screen, or the same data as a hex dump.

    Repaints are driven by the app's frame timer rather than by arriving bytes:
    at 921,600 baud a repaint per read would spend all its time drawing (PRD
    risk register: "render at max 60 fps").
    """

    DEFAULT_CSS = """
    TerminalView {
        height: 1fr;
        width: 1fr;
        background: $surface;
        color: $foreground;
        overflow: hidden;
    }
    """

    view_mode: reactive[ViewMode] = reactive(ViewMode.TEXT)

    def __init__(self, terminal: Terminal, **kwargs: object) -> None:
        super().__init__(**kwargs)  # type: ignore[arg-type]
        self.terminal = terminal
        self._raw: bytearray = bytearray()
        self._raw_origin = 0
        self._matches: tuple[Match, ...] = ()
        self._last_revision = -1

    # -- data --------------------------------------------------------------

    def feed(self, data: bytes) -> None:
        """Hand received bytes to the emulator and the hex buffer."""
        self.terminal.feed(data)
        self._raw += data
        if len(self._raw) > HEX_BUFFER_BYTES:
            dropped = len(self._raw) - HEX_BUFFER_BYTES
            del self._raw[:dropped]
            self._raw_origin += dropped

    def set_matches(self, matches: tuple[Match, ...]) -> None:
        """Highlight these search results (F8)."""
        self._matches = matches
        self.refresh()

    def tick(self) -> None:
        """Repaint if anything changed since the last frame."""
        if self.terminal.revision != self._last_revision:
            self._last_revision = self.terminal.revision
            self.refresh()

    # -- rendering ---------------------------------------------------------

    def render(self) -> RenderableType:
        if self.view_mode is ViewMode.TEXT:
            return self._render_text()
        return self._render_hex()

    def _render_text(self) -> RenderableType:
        snapshot = self.terminal.snapshot()
        first_line = max(0, snapshot.total_lines - snapshot.rows - snapshot.scroll_offset)
        return snapshot_to_text(
            snapshot,
            matches=self._matches,
            first_line=first_line,
            show_cursor=snapshot.following,
        )

    def _render_hex(self) -> RenderableType:
        """The last screenful of raw bytes as a hex dump (F10)."""
        rows = self.size.height or self.terminal.rows
        width = 16
        visible = self._raw[-rows * width :]
        offset = self._raw_origin + len(self._raw) - len(visible)
        text = Text()
        for index, row in enumerate(hex_rows(bytes(visible), offset=offset, width=width)):
            if index:
                text.append("\n")
            text.append(row.render())
        return text

    def watch_view_mode(self, _old: ViewMode, _new: ViewMode) -> None:
        self.refresh()


class StatusBar(Widget):
    """One line of session state: port, framing, counters, signals (F22, F4)."""

    DEFAULT_CSS = """
    StatusBar {
        height: 1;
        width: 1fr;
        background: $panel;
        color: $foreground;
    }
    """

    text: reactive[str] = reactive("")

    def render(self) -> RenderableType:
        return Text(self.text, no_wrap=True, overflow="ellipsis")
