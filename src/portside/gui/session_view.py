"""One session's page: terminal, signals panel and status line (PRD F22).

Everything a tab (or a detached window) needs to show one connection.  It owns
no I/O: it talks to a :class:`~portside.gui.controller.SessionController`.
"""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QKeyEvent
from PySide6.QtWidgets import (
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QProgressBar,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from portside.core.errors import PortsideError
from portside.core.session import SessionState
from portside.emulation.find import Match, SearchOptions, find_all, find_next, find_prev
from portside.emulation.hexview import ViewMode
from portside.emulation.terminal import Terminal
from portside.gui.controller import SessionController
from portside.gui.keymap import qt_key_to_bytes
from portside.gui.palette import ThemeName
from portside.gui.signals_panel import SignalsPanel
from portside.gui.terminal_widget import TerminalWidget
from portside.profiles.store import TerminalOptions

__all__ = ["SessionView"]


class SessionView(QWidget):
    """The terminal, the signal indicators and the status line for one session."""

    #: Emitted when the status line changes, so the window can mirror it.
    statusChanged = Signal(str)
    #: Emitted when an error should be shown to the user.
    errorRaised = Signal(object)  # PortsideError

    def __init__(
        self,
        controller: SessionController,
        *,
        theme: ThemeName = ThemeName.DARK,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.controller = controller
        profile = controller.profile
        options = profile.terminal if profile else TerminalOptions()
        self.terminal = Terminal(
            options.columns,
            options.rows,
            scrollback=options.scrollback,
            charset=options.charset,
            strip_high_bit=options.strip_high_bit,
        )
        self._notice = ""
        self._paste_note = ""
        self._matches: tuple[Match, ...] = ()
        self._current_match: Match | None = None

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        self.view = TerminalWidget(
            self.terminal,
            palette_name=theme,
            font_family=options.font_family,
            font_size=options.font_size,
            parent=self,
        )
        self.view.keyPressed.connect(self._on_key)
        layout.addWidget(self.view, 1)

        self.find_bar = self._build_find_bar()
        self.find_bar.setVisible(False)
        layout.addWidget(self.find_bar)

        self.paste_progress = QProgressBar(self)
        self.paste_progress.setVisible(False)
        self.paste_progress.setAccessibleName(self.tr("Paste progress"))
        layout.addWidget(self.paste_progress)

        bottom = QWidget(self)
        bottom_layout = QHBoxLayout(bottom)
        bottom_layout.setContentsMargins(4, 2, 4, 2)
        self.signals_panel = SignalsPanel(bottom)
        self.signals_panel.dtrToggled.connect(self._on_dtr)
        self.signals_panel.rtsToggled.connect(self._on_rts)
        bottom_layout.addWidget(self.signals_panel)
        self.status_label = QLabel("", bottom)
        self.status_label.setAccessibleName(self.tr("Session status"))
        bottom_layout.addWidget(self.status_label, 1, Qt.AlignmentFlag.AlignRight)
        layout.addWidget(bottom)

        controller.dataReceived.connect(self._on_data)
        controller.stateChanged.connect(self._on_state)
        controller.errorRaised.connect(self._on_error)
        controller.reconnecting.connect(self._on_reconnecting)
        controller.pasteProgress.connect(self._on_paste_progress)
        controller.pasteFinished.connect(self._on_paste_finished)

        self.signals_panel.start_polling(self._poll_signals)
        self._update_status()

    # -- find bar ----------------------------------------------------------

    def _build_find_bar(self) -> QWidget:
        bar = QWidget(self)
        layout = QHBoxLayout(bar)
        layout.setContentsMargins(4, 2, 4, 2)
        self.find_input = QLineEdit(bar)
        self.find_input.setPlaceholderText(self.tr("Find in scrollback"))
        self.find_input.setAccessibleName(self.tr("Find"))
        self.find_input.returnPressed.connect(self.find_next)
        self.find_input.textChanged.connect(self._run_find)
        layout.addWidget(self.find_input, 1)
        for text, slot in (
            (self.tr("Previous"), self.find_previous),
            (self.tr("Next"), self.find_next),
            (self.tr("Close"), self.close_find),
        ):
            button = QPushButton(text, bar)
            button.clicked.connect(slot)
            button.setAccessibleName(text)
            layout.addWidget(button)
        self.find_status = QLabel("", bar)
        layout.addWidget(self.find_status)
        return bar

    def open_find(self) -> None:
        """Show the find bar and focus it (F8)."""
        self.find_bar.setVisible(True)
        self.find_input.setFocus()
        self.find_input.selectAll()

    def close_find(self) -> None:
        """Hide the find bar and clear the highlights."""
        self.find_bar.setVisible(False)
        self._matches = ()
        self.view.set_matches(())
        self.view.setFocus()

    def _run_find(self, query: str) -> None:
        if not query:
            self._matches = ()
            self.view.set_matches(())
            self.find_status.setText("")
            return
        try:
            matches = find_all(self.terminal.all_text_lines(), query, SearchOptions())
        except PortsideError as error:
            self.find_status.setText(str(error))
            return
        self._matches = tuple(matches)
        self.view.set_matches(self._matches)
        self.find_status.setText(
            self.tr("{0} matches").format(len(matches)) if matches else self.tr("no match")
        )
        if matches:
            self._current_match = matches[0]
            self._scroll_to_match()

    def find_next(self) -> None:
        """Move to the next match (F8)."""
        self._current_match = find_next(list(self._matches), self._current_match)
        self._scroll_to_match()

    def find_previous(self) -> None:
        """Move to the previous match (F8)."""
        self._current_match = find_prev(list(self._matches), self._current_match)
        self._scroll_to_match()

    def _scroll_to_match(self) -> None:
        match = self._current_match
        if match is None:
            return
        offset = max(0, self.terminal.total_lines - match.line - self.terminal.rows // 2)
        self.terminal.scroll_to(offset)
        self.view.update()
        self._update_status()

    # -- session events ----------------------------------------------------

    def _on_data(self, data: bytes) -> None:
        self.view.feed(data)
        self._update_status()

    def _on_state(self, state: str, detail: str) -> None:
        self._notice = f"{state}" + (f" — {detail}" if detail else "")
        connected = state == SessionState.CONNECTED.value
        self.signals_panel.set_enabled_for_session(connected)
        self._update_status()

    def _on_error(self, error: PortsideError, fatal: bool) -> None:
        self.errorRaised.emit(error)
        if fatal:
            self._notice = str(error)
            self._update_status()

    def _on_reconnecting(self, attempt: int, delay: float) -> None:
        self._notice = self.tr("reconnecting (attempt {0}, next try in {1:.1f}s)").format(
            attempt, delay
        )
        self._update_status()

    def _on_paste_progress(self, line: int, total: int) -> None:
        self.paste_progress.setVisible(True)
        self.paste_progress.setMaximum(max(1, total))
        self.paste_progress.setValue(line)
        self._paste_note = self.tr("paste {0}/{1}").format(line, total)
        self._update_status()

    def _on_paste_finished(self, result: object) -> None:
        self.paste_progress.setVisible(False)
        self._paste_note = ""
        if result is not None:
            self._notice = getattr(result, "describe", lambda: "")()
        self._update_status()

    def _poll_signals(self) -> None:
        """Ask for the modem lines; the answer arrives on the GUI thread."""
        if not self.controller.is_connected:
            return
        future = self.controller.modem_lines()

        def apply(_done: object) -> None:
            try:
                lines = future.result(timeout=0)
            except Exception:
                return
            self.signals_panel.set_lines(lines)

        future.add_done_callback(apply)

    # -- input -------------------------------------------------------------

    def _on_key(self, event: QKeyEvent) -> None:
        """Send a keystroke to the device (F7, F9)."""
        payload = qt_key_to_bytes(event, self.controller.text_policy)
        if not payload:
            return
        self.terminal.follow()
        if self.controller.text_policy.local_echo:
            self.terminal.feed_text(payload.decode("utf-8", "replace"))
        self.controller.send(payload)

    def _on_dtr(self, value: bool) -> None:
        future = self.controller.set_dtr(value)
        future.add_done_callback(lambda done: self._report_line_support(done, "DTR"))

    def _on_rts(self, value: bool) -> None:
        future = self.controller.set_rts(value)
        future.add_done_callback(lambda done: self._report_line_support(done, "RTS"))

    def _report_line_support(self, future: object, _name: str) -> None:
        """A driver without the line disables the toggles rather than lying (F4)."""
        try:
            supported = bool(future.result(timeout=0))  # type: ignore[attr-defined]
        except Exception:
            return
        if not supported:
            self.signals_panel.set_control_lines_supported(False)

    # -- view options ------------------------------------------------------

    def toggle_hex(self) -> None:
        """Switch between the terminal and a hex dump (F10)."""
        self.view.set_view_mode(
            ViewMode.HEX if self.view.view_mode is ViewMode.TEXT else ViewMode.TEXT
        )
        self._update_status()

    def set_theme(self, theme: ThemeName) -> None:
        """Repaint in another theme (F8)."""
        self.view.set_theme(theme)

    def copy_selection(self) -> str:
        """Copy the selected text to the clipboard (F8)."""
        return self.view.copy_selection()

    def paste_file(self, path: Path) -> None:
        """Paste a file at the session's pace (F13)."""
        self.controller.paste_file(path)

    def clear(self) -> None:
        """Clear the screen and scrollback."""
        self.terminal.reset()
        self.view.update()

    # -- status ------------------------------------------------------------

    def status_text(self) -> str:
        """The status line for this session (F22)."""
        controller = self.controller
        parts = [
            controller.port,
            controller.settings.describe(),
            self.tr("in {0} B").format(controller.session.bytes_in),
            self.tr("out {0} B").format(controller.session.bytes_out),
        ]
        if controller.is_logging:
            parts.append(self.tr("recording"))
        if self.terminal.scroll_offset:
            parts.append(self.tr("scrollback -{0}").format(self.terminal.scroll_offset))
        if self.view.view_mode is not ViewMode.TEXT:
            parts.append(self.view.view_mode.value.upper())
        if self._paste_note:
            parts.append(self._paste_note)
        parts.append(self._notice or controller.state.value)
        return " · ".join(part for part in parts if part)

    def _update_status(self) -> None:
        text = self.status_text()
        self.status_label.setText(text)
        self.statusChanged.emit(text)

    # -- shutdown ----------------------------------------------------------

    def close_session(self) -> None:
        """Stop polling and close the session."""
        self.signals_panel.stop_polling()
        self.controller.close()
