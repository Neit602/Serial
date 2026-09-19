"""One session, wrapped for Qt (PRD F3, F4, F5, F12, F13, F22).

:class:`SessionController` is the only thing in the GUI that knows about the
core.  It republishes the session's event bus as Qt signals — emitted from the
asyncio thread, delivered on the GUI thread by Qt's queued connections — and
offers the actions a UI needs as non-blocking calls.
"""

from __future__ import annotations

import contextlib
from collections.abc import Callable
from concurrent.futures import Future
from pathlib import Path
from typing import Any

from PySide6.QtCore import QObject, Signal

from portside.core.breaks import BreakMode, BreakResult
from portside.core.errors import PortsideError
from portside.core.events import (
    BytesReceived,
    BytesSent,
    ErrorRaised,
    Event,
    ReconnectAttempt,
    StateChanged,
)
from portside.core.identity import DeviceIdentity
from portside.core.pacing import PacePolicy, PasteProgress, PasteResult, paste_text
from portside.core.session import Session, SessionState
from portside.core.settings import SerialSettings
from portside.core.textio import TextPolicy
from portside.gui.runtime import AsyncRuntime
from portside.logging.session_log import SessionLogger
from portside.profiles.store import Profile
from portside.transport.base import ModemLines

__all__ = ["SessionController"]


class SessionController(QObject):
    """A Qt-facing view of one :class:`~portside.core.session.Session`."""

    dataReceived = Signal(bytes)
    bytesSent = Signal(int)
    stateChanged = Signal(str, str)  # state value, detail
    errorRaised = Signal(object, bool)  # PortsideError, fatal
    reconnecting = Signal(int, float)  # attempt, delay
    pasteProgress = Signal(int, int)  # line, total
    pasteFinished = Signal(object)  # PasteResult or None when it failed

    def __init__(
        self,
        session: Session,
        runtime: AsyncRuntime,
        *,
        profile: Profile | None = None,
        text_policy: TextPolicy | None = None,
        logger: SessionLogger | None = None,
        parent: QObject | None = None,
    ) -> None:
        super().__init__(parent)
        self._session = session
        self._runtime = runtime
        self.profile = profile
        self.text_policy = text_policy or (profile.text if profile else TextPolicy())
        self.pace = profile.pacing if profile else PacePolicy()
        self.logger = logger
        self._paste: Future[Any] | None = None
        self._detach_logger: Callable[[], None] | None = None
        if logger is not None:
            logger.port = session.port
            self._detach_logger = logger.attach(session)
        # Signals are emitted from the asyncio thread; Qt queues them onto the
        # GUI thread, which is what makes this safe.
        self._unsubscribe = session.events.subscribe(self._republish)

    # -- introspection -----------------------------------------------------

    @property
    def session(self) -> Session:
        """The wrapped session (for tests and for the status bar)."""
        return self._session

    @property
    def port(self) -> str:
        """Port name in use."""
        return self._session.port

    @property
    def state(self) -> SessionState:
        """Current session state."""
        return self._session.state

    @property
    def is_connected(self) -> bool:
        """True while bytes can flow."""
        return self._session.is_connected

    @property
    def identity(self) -> DeviceIdentity | None:
        """The device behind this session, when it came from discovery."""
        return self._session.identity

    @property
    def settings(self) -> SerialSettings:
        """Line settings in force."""
        return self._session.settings

    def status_line(self) -> str:
        """One-line summary for the status bar (F22)."""
        return self._session.status_line()

    # -- event republishing ------------------------------------------------

    def _republish(self, event: Event) -> None:
        """Turn one core event into one Qt signal."""
        if isinstance(event, BytesReceived):
            self.dataReceived.emit(event.data)
        elif isinstance(event, BytesSent):
            self.bytesSent.emit(event.count)
        elif isinstance(event, StateChanged):
            self.stateChanged.emit(event.new.value, event.detail or "")
        elif isinstance(event, ErrorRaised):
            self.errorRaised.emit(event.error, event.fatal)
        elif isinstance(event, ReconnectAttempt):
            self.reconnecting.emit(event.attempt, event.delay)

    # -- actions (all non-blocking) ---------------------------------------

    def send(self, data: bytes) -> Future[int]:
        """Send bytes to the device."""
        return self._runtime.submit(self._session.send(data))

    def send_break(
        self, duration: float = 0.25, mode: BreakMode | None = None
    ) -> Future[BreakResult]:
        """Send a break, emulated when the driver has none (F12)."""
        return self._runtime.submit(self._session.send_break(duration, mode=mode))

    def set_dtr(self, value: bool) -> Future[bool]:
        """Drive DTR; the result says whether the driver has the line (F4)."""
        return self._runtime.submit(self._session.set_dtr(value))

    def set_rts(self, value: bool) -> Future[bool]:
        """Drive RTS; the result says whether the driver has the line (F4)."""
        return self._runtime.submit(self._session.set_rts(value))

    def modem_lines(self) -> Future[ModemLines]:
        """Read CTS/DSR/DCD/RI (F4)."""
        return self._runtime.submit(self._session.modem_lines())

    def apply_settings(self, settings: SerialSettings) -> Future[None]:
        """Change line settings without dropping the session (F3)."""
        return self._runtime.submit(self._session.apply_settings(settings))

    def paste_text(self, text: str, pace: PacePolicy | None = None) -> Future[Any]:
        """Paste ``text`` at the configured pace (F13)."""
        self.cancel_paste()

        async def run() -> PasteResult | None:
            try:
                result = await paste_text(
                    self._session,
                    text,
                    pace or self.pace,
                    text_policy=self.text_policy,
                    on_progress=lambda update: self._on_paste_progress(update),
                )
            except PortsideError as error:
                self.errorRaised.emit(error, False)
                self.pasteFinished.emit(None)
                return None
            self.pasteFinished.emit(result)
            return result

        self._paste = self._runtime.submit(run())
        return self._paste

    def paste_file(self, path: Path, pace: PacePolicy | None = None) -> Future[Any] | None:
        """Paste a file; reports the error rather than raising (F13)."""
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            self.errorRaised.emit(
                PortsideError(f"cannot read {path}: {error}", port=self.port), False
            )
            return None
        return self.paste_text(text, pace)

    def cancel_paste(self) -> None:
        """Stop a paste in progress (F13)."""
        paste, self._paste = self._paste, None
        if paste is not None and not paste.done():
            paste.cancel()

    def _on_paste_progress(self, update: PasteProgress) -> None:
        self.pasteProgress.emit(update.line, update.total_lines)

    # -- logging -----------------------------------------------------------

    @property
    def is_logging(self) -> bool:
        """True while a session log is being written (F15)."""
        return self.logger is not None and self.logger.is_logging

    def start_logging(self) -> Path | None:
        """Begin logging; returns the path, or ``None`` without a logger."""
        if self.logger is None:
            return None
        self.logger.port = self.port
        return self.logger.start()

    def stop_logging(self) -> None:
        """Stop logging (F15)."""
        if self.logger is not None:
            self.logger.stop()

    # -- shutdown ----------------------------------------------------------

    def close(self, timeout: float = 5.0) -> None:
        """Close the session and stop listening.  Idempotent."""
        self.cancel_paste()
        self._unsubscribe()
        with contextlib.suppress(Exception):  # shutdown must not raise into the UI
            self._runtime.submit(self._session.close()).result(timeout)
        if self._detach_logger is not None:
            self._detach_logger()
            self._detach_logger = None
