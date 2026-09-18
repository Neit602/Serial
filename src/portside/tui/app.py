"""The Portside terminal UI (PRD F23, plus the terminal features of M2).

A thin front end: it owns no I/O.  Everything it shows comes from the session's
event bus and the emulator, and everything the user types goes out through
``Session.send``.  The port itself is only ever touched by the session's I/O
threads.
"""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import Callable
from pathlib import Path
from typing import Any, ClassVar

from textual import events, on
from textual.app import App, ComposeResult
from textual.binding import Binding, BindingType
from textual.containers import Vertical
from textual.widgets import Input, Static

from portside.core.breaks import BreakMode
from portside.core.errors import PortsideError
from portside.core.events import (
    BytesReceived,
    ErrorRaised,
    Event,
    ReconnectAttempt,
    StateChanged,
)
from portside.core.pacing import PacePolicy, PasteProgress, paste_text
from portside.core.session import Session
from portside.core.textio import TextPolicy
from portside.emulation.find import Match, SearchOptions, find_all, find_next, find_prev
from portside.emulation.hexview import ViewMode
from portside.emulation.terminal import Terminal
from portside.logging.session_log import LogSettings, SessionLogger
from portside.logging.timestamps import TimestampMode
from portside.profiles.store import Profile
from portside.tui.keys import COMMAND_KEYS, key_to_bytes
from portside.tui.widgets import StatusBar, TerminalView

__all__ = ["PortsideTUI", "run_tui"]

#: PRD risk register: render at most 60 times a second, however fast data comes.
FRAME_INTERVAL = 1 / 60
#: How often the modem lines are polled for the status bar (F4).
SIGNAL_INTERVAL = 1.0

THEMES = ("textual-dark", "textual-light", "tokyo-night")


class PortsideTUI(App[None]):
    """Serial terminal in the terminal."""

    CSS = """
    Screen { layout: vertical; }
    #prompt { height: 3; border: round $accent; display: none; }
    #prompt.visible { display: block; }
    #toast { height: auto; padding: 0 1; background: $warning; color: $text; display: none; }
    #toast.visible { display: block; }
    """

    BINDINGS: ClassVar[list[BindingType]] = [
        Binding("f1", "help", "Help", show=True),
        Binding("f2", "send_break", "Break", show=True),
        Binding("f3", "find", "Find", show=True),
        Binding("f4", "paste_file", "Paste", show=True),
        Binding("f5", "toggle_log", "Log", show=True),
        Binding("f6", "toggle_hex", "Hex", show=True),
        Binding("f7", "cycle_timestamps", "Stamps", show=True),
        Binding("f8", "cycle_theme", "Theme", show=False),
        Binding("f9", "clear", "Clear", show=False),
        Binding("f10", "quit", "Quit", show=True),
        Binding("ctrl+rightsquarebracket", "quit", "Quit", show=False),
        Binding("pageup", "scroll_back", "Scroll back", show=False),
        Binding("pagedown", "scroll_forward", "Scroll fwd", show=False),
    ]

    def __init__(
        self,
        session: Session,
        *,
        terminal: Terminal | None = None,
        text_policy: TextPolicy | None = None,
        logger: SessionLogger | None = None,
        profile: Profile | None = None,
        pace: PacePolicy | None = None,
        break_duration: float = 0.25,
    ) -> None:
        super().__init__()
        self.session = session
        self.profile = profile
        self.text_policy = text_policy or (profile.text if profile else TextPolicy())
        self.pace = pace or (profile.pacing if profile else PacePolicy())
        self.break_duration = break_duration
        self.terminal = terminal or self._terminal_from(profile)
        self.logger = logger or SessionLogger(
            settings=profile.logging if profile else LogSettings(), port=session.port
        )
        self.view = TerminalView(self.terminal, id="terminal")
        self.status = StatusBar(id="status")
        self.prompt = Input(id="prompt")
        self.toast = Static(id="toast")

        self._matches: tuple[Match, ...] = ()
        self._current_match: Match | None = None
        self._prompt_action: str | None = None
        self._paste_task: asyncio.Task[Any] | None = None
        self._paste_note = ""
        self._signals = ""
        self._notice = ""
        #: The most recent message shown to the user; also what tests assert on.
        self.last_message = ""
        self._unsubscribe: Callable[[], None] | None = None
        self._detach_logger: Callable[[], None] | None = None
        self._pump: asyncio.Task[None] | None = None

    @staticmethod
    def _terminal_from(profile: Profile | None) -> Terminal:
        if profile is None:
            return Terminal()
        options = profile.terminal
        return Terminal(
            options.columns,
            options.rows,
            scrollback=options.scrollback,
            charset=options.charset,
            strip_high_bit=options.strip_high_bit,
        )

    # -- lifecycle ---------------------------------------------------------

    def compose(self) -> ComposeResult:
        with Vertical():
            yield self.view
            yield self.toast
            yield self.prompt
            yield self.status

    async def on_mount(self) -> None:
        self.view.focus()
        self._unsubscribe = self.session.events.subscribe(self._on_event)
        self.logger.port = self.session.port
        # The logger listens to the event bus, not to this widget: a scrolled
        # or busy terminal must never cost the log a line (F15).
        self._detach_logger = self.logger.attach(self.session)
        if self.logger.settings.auto_start and self.session.is_connected:
            self.logger.start()
        self._pump = asyncio.create_task(self._consume())
        self.set_interval(FRAME_INTERVAL, self._frame)
        self.set_interval(SIGNAL_INTERVAL, self._poll_signals)
        self._resize_terminal()
        self._update_status()

    async def on_unmount(self) -> None:
        if self._unsubscribe is not None:
            self._unsubscribe()
        if self._detach_logger is not None:
            self._detach_logger()
        for task in (self._pump, self._paste_task):
            if task is not None and not task.done():
                task.cancel()
                with contextlib.suppress(asyncio.CancelledError, PortsideError, Exception):
                    await task
        self.logger.stop()

    async def _consume(self) -> None:
        """Feed received bytes into the emulator (the only consumer of rx)."""
        async for chunk in self.session.stream():
            self.view.feed(chunk)

    def _on_event(self, event: Event) -> None:
        """Session events arrive on the loop thread; keep this cheap."""
        if isinstance(event, StateChanged):
            self._notice = f"{event.new.value}" + (f" — {event.detail}" if event.detail else "")
            self._update_status()
        elif isinstance(event, ReconnectAttempt):
            self._notice = f"reconnecting (attempt {event.attempt})"
            self._update_status()
        elif isinstance(event, ErrorRaised):
            self._show_toast(event.error.display())
        elif isinstance(event, BytesReceived):
            pass  # data is consumed by _consume; counters update on the next frame

    # -- frame loop --------------------------------------------------------

    def _frame(self) -> None:
        self.view.tick()
        self._update_status()

    def _resize_terminal(self) -> None:
        """Match the emulator to the widget, so the device wraps where we do."""
        size = self.view.size
        if size.width and size.height:
            self.terminal.resize(size.width, size.height)

    def on_resize(self, _event: events.Resize) -> None:
        self._resize_terminal()

    async def _poll_signals(self) -> None:
        """Read CTS/DSR/DCD/RI for the status bar (F4)."""
        if not self.session.is_connected:
            self._signals = ""
            return
        try:
            lines = await self.session.modem_lines()
        except PortsideError:
            self._signals = ""
            return
        self._signals = lines.describe()

    def _update_status(self) -> None:
        session = self.session
        parts = [
            session.port,
            session.settings.describe(),
            f"in {session.bytes_in} B",
            f"out {session.bytes_out} B",
        ]
        if self._signals:
            parts.append(self._signals)
        if self.logger.is_logging:
            parts.append(f"REC {self.logger.path.name if self.logger.path else ''}")
        if self.terminal.scroll_offset:
            parts.append(f"scrollback -{self.terminal.scroll_offset}")
        if self.view.view_mode is not ViewMode.TEXT:
            parts.append(self.view.view_mode.value.upper())
        if self._paste_note:
            parts.append(self._paste_note)
        parts.append(self._notice or session.state.value)
        self.status.text = " · ".join(part for part in parts if part)

    def _show_toast(self, message: str) -> None:
        self.last_message = message
        self.toast.update(message)
        self.toast.add_class("visible")
        self.set_timer(6.0, lambda: self.toast.remove_class("visible"))

    # -- keyboard ----------------------------------------------------------

    async def on_key(self, event: events.Key) -> None:
        """Send keystrokes to the device (F7, F9)."""
        if self.prompt.has_focus:
            # The prompt owns the keyboard while it is open; Escape closes it.
            if event.key == "escape":
                event.stop()
                self._close_prompt()
            return
        if event.key in COMMAND_KEYS or event.key.startswith("ctrl+f"):
            return  # handled by BINDINGS
        payload = key_to_bytes(event.key, event.character, self.text_policy)
        if not payload:
            return
        event.stop()
        event.prevent_default()
        # Typing returns the view to the live tail, as every terminal does.
        self.terminal.follow()
        if self.text_policy.local_echo:
            self.terminal.feed(self.text_policy.echo_for(payload))
        try:
            await self.session.send(payload)
        except PortsideError as error:
            self._show_toast(error.display())

    # -- actions -----------------------------------------------------------

    async def action_send_break(self) -> None:
        """F2 — send a break, emulating it if the driver cannot (F12)."""
        try:
            result = await self.session.send_break(self.break_duration)
        except PortsideError as error:
            self._show_toast(error.display())
            return
        self._show_toast(result.describe())
        self.logger.write_note(result.describe())

    def action_toggle_hex(self) -> None:
        """F6 — switch between the terminal and a hex dump (F10)."""
        self.view.view_mode = (
            ViewMode.HEX if self.view.view_mode is ViewMode.TEXT else ViewMode.TEXT
        )
        self._update_status()

    def action_toggle_log(self) -> None:
        """F5 — start or stop the session log (F15)."""
        if self.logger.is_logging:
            path = self.logger.path
            self.logger.stop()
            self._show_toast(f"logging stopped: {path}")
        else:
            self.logger.port = self.session.port
            path = self.logger.start()
            self._show_toast(f"logging to {path}")
        self._update_status()

    def action_cycle_timestamps(self) -> None:
        """F7 — cycle the log's timestamp mode (F16)."""
        modes = list(TimestampMode)
        current = self.logger.settings.timestamps
        nxt = modes[(modes.index(current) + 1) % len(modes)]
        self.logger.restamp(nxt)
        self._show_toast(f"timestamps: {nxt.value}")

    def action_cycle_theme(self) -> None:
        """F8 — dark, light and high-contrast themes (F8, accessibility)."""
        index = THEMES.index(self.theme) if self.theme in THEMES else -1
        self.theme = THEMES[(index + 1) % len(THEMES)]

    def action_clear(self) -> None:
        """F9 — clear the screen and the scrollback."""
        self.terminal.reset()
        self.view.set_matches(())

    def action_scroll_back(self) -> None:
        """PageUp — move up the scrollback (F8)."""
        self.terminal.scroll_page(1)
        self._update_status()

    def action_scroll_forward(self) -> None:
        """PageDown — move back towards the live tail (F8)."""
        self.terminal.scroll_page(-1)
        self._update_status()

    def action_find(self) -> None:
        """F3 — search the screen and scrollback (F8)."""
        self._open_prompt("find", "Find (Enter: next, Esc: close)")

    def action_paste_file(self) -> None:
        """F4 — paste a file at the configured pace (F13)."""
        if self._paste_task is not None and not self._paste_task.done():
            self._paste_task.cancel()
            self._paste_note = "paste cancelled"
            return
        self._open_prompt("paste", "Path of the file to paste")

    def action_help(self) -> None:
        """F1 — the key map, because nobody remembers function keys."""
        self._show_toast(
            "F1 help · F2 break · F3 find · F4 paste file · F5 log · F6 hex · "
            "F7 timestamps · F8 theme · F9 clear · F10 quit · PgUp/PgDn scrollback"
        )

    # -- prompt ------------------------------------------------------------

    def _open_prompt(self, action: str, placeholder: str) -> None:
        self._prompt_action = action
        self.prompt.placeholder = placeholder
        self.prompt.value = ""
        self.prompt.add_class("visible")
        self.prompt.focus()

    def _close_prompt(self) -> None:
        self._prompt_action = None
        self.prompt.remove_class("visible")
        self.view.focus()

    @on(Input.Submitted, "#prompt")
    async def _prompt_submitted(self, event: Input.Submitted) -> None:
        action, value = self._prompt_action, event.value.strip()
        self._close_prompt()
        if not value:
            return
        if action == "find":
            self._run_find(value)
        elif action == "paste":
            await self._start_paste(Path(value))

    def _run_find(self, query: str) -> None:
        try:
            matches = find_all(self.terminal.all_text_lines(), query, SearchOptions())
        except PortsideError as error:
            self._show_toast(error.display())
            return
        self._matches = tuple(matches)
        self.view.set_matches(self._matches)
        if not matches:
            self._show_toast(f"no match for {query!r}")
            return
        self._current_match = find_next(list(matches), self._current_match)
        self._scroll_to_match()
        self._show_toast(f"{len(matches)} match(es) for {query!r}")

    def _scroll_to_match(self) -> None:
        match = self._current_match
        if match is None:
            return
        total = self.terminal.total_lines
        offset = max(0, total - match.line - self.terminal.rows // 2)
        self.terminal.scroll_to(offset)
        self._update_status()

    def action_find_next(self) -> None:
        """Move to the next search hit (F8)."""
        self._current_match = find_next(list(self._matches), self._current_match)
        self._scroll_to_match()

    def action_find_prev(self) -> None:
        """Move to the previous search hit (F8)."""
        self._current_match = find_prev(list(self._matches), self._current_match)
        self._scroll_to_match()

    # -- paste -------------------------------------------------------------

    async def _start_paste(self, path: Path) -> None:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            self._show_toast(f"cannot read {path}: {error}")
            return

        def progress(update: PasteProgress) -> None:
            self._paste_note = f"paste {update.line}/{update.total_lines}"

        async def run() -> None:
            try:
                result = await paste_text(
                    self.session,
                    text,
                    self.pace,
                    text_policy=self.text_policy,
                    on_progress=progress,
                )
            except PortsideError as error:
                self._paste_note = ""
                self._show_toast(error.display())
                return
            self._paste_note = ""
            self._show_toast(result.describe())
            self.logger.write_note(f"paste {path.name}: {result.describe()}")

        self._paste_task = asyncio.create_task(run())


async def run_tui(
    session: Session,
    *,
    profile: Profile | None = None,
    text_policy: TextPolicy | None = None,
    logger: SessionLogger | None = None,
    break_duration: float = 0.25,
    break_mode: BreakMode = BreakMode.AUTO,
) -> None:
    """Run the TUI against an already-open session."""
    session.break_mode = break_mode
    app = PortsideTUI(
        session,
        profile=profile,
        text_policy=text_policy,
        logger=logger,
        break_duration=break_duration,
    )
    await app.run_async()
