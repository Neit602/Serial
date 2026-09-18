"""Session logging (PRD F15, F16).

The logger subscribes to a session's event bus, so it captures every byte that
arrives regardless of what any UI is doing with the data — a slow or scrolled
terminal never costs the log a line.

Two modes:

* ``text`` — decoded, optionally timestamped, UTF-8 with LF endings internally,
  identically on all three platforms (PRD rule: no CRLF logs on Windows).
* ``raw`` — the exact bytes from the wire, for protocol debugging.

Writes go through a buffer flushed on an interval, on rotation and on close, so
logging cannot stall the I/O path.
"""

from __future__ import annotations

import re
import time
from collections.abc import Callable
from dataclasses import dataclass, field, replace
from datetime import datetime
from enum import StrEnum
from pathlib import Path
from typing import IO, TYPE_CHECKING

from portside.emulation.decoder import CharacterSet, IncrementalTextDecoder
from portside.logging.timestamps import Timestamper, TimestampMode

if TYPE_CHECKING:
    from portside.core.events import Event

__all__ = ["LogFormat", "LogSettings", "SessionLogger", "resolve_template"]

#: Characters Windows forbids in a filename; replaced so one template works
#: everywhere (PRD: never hard-code platform differences into paths).
_UNSAFE_NAME = re.compile(r'[<>:"/\\|?*\x00-\x1f]')

DEFAULT_TEMPLATE = "portside-{port}-{date}-{time}.log"


class LogFormat(StrEnum):
    """What goes into the file (F15)."""

    TEXT = "text"
    RAW = "raw"


def _sanitise(value: str) -> str:
    """Make one template substitution safe as a filename component."""
    cleaned = _UNSAFE_NAME.sub("_", value).strip(". ")
    return cleaned or "unnamed"


def resolve_template(
    template: str,
    *,
    port: str = "",
    profile: str = "",
    when: datetime | None = None,
    index: int = 0,
) -> str:
    """Expand a filename template (F15).

    Placeholders: ``{port}``, ``{device}`` (an alias for port), ``{date}``,
    ``{time}``, ``{profile}``, ``{n}``.  Every substitution is sanitised, so a
    port named ``/dev/ttyUSB0`` or ``COM3:`` cannot produce an illegal path.
    """
    moment = when or datetime.now().astimezone()
    safe_port = _sanitise(port)
    values = {
        "port": safe_port,
        "device": safe_port,
        "date": moment.strftime("%Y%m%d"),
        "time": moment.strftime("%H%M%S"),
        "profile": _sanitise(profile) if profile else "default",
        "n": str(index),
    }
    try:
        return template.format(**values)
    except (KeyError, IndexError, ValueError):
        # An unknown placeholder must not lose the user's session; fall back to
        # the default name rather than raising in the middle of a connect.
        return DEFAULT_TEMPLATE.format(**values)


@dataclass(frozen=True, slots=True)
class LogSettings:
    """How a session should be logged (F15, F16)."""

    directory: Path | None = None
    template: str = DEFAULT_TEMPLATE
    log_format: LogFormat = LogFormat.TEXT
    timestamps: TimestampMode = TimestampMode.OFF
    charset: CharacterSet = CharacterSet.UTF8
    auto_start: bool = False
    #: Bytes per file before rotation; 0 disables rotation.
    max_bytes: int = 0
    #: How many rotated files to keep, oldest deleted first.
    keep: int = 5
    flush_interval: float = 1.0


@dataclass
class SessionLogger:
    """Writes one session to disk, driven by its event bus (F15, F16)."""

    settings: LogSettings
    port: str = ""
    profile: str = ""
    now: Callable[[], float] = time.time
    _handle: IO[bytes] | None = field(default=None, init=False)
    _path: Path | None = field(default=None, init=False)
    _buffer: bytearray = field(default_factory=bytearray, init=False)
    _written: int = field(default=0, init=False)
    _rotations: int = field(default=0, init=False)
    _last_flush: float = field(default=0.0, init=False)
    _line_open: bool = field(default=False, init=False)
    _timestamper: Timestamper = field(default_factory=Timestamper, init=False)
    _decoder: IncrementalTextDecoder | None = field(default=None, init=False)
    _unsubscribe: Callable[[], None] | None = field(default=None, init=False)

    # -- lifecycle ---------------------------------------------------------

    @property
    def is_logging(self) -> bool:
        """True while a file is open."""
        return self._handle is not None

    @property
    def path(self) -> Path | None:
        """The file currently being written, if any."""
        return self._path

    @property
    def bytes_written(self) -> int:
        """Bytes written to the current file (buffer included)."""
        return self._written + len(self._buffer)

    @property
    def rotations(self) -> int:
        """How many times the log has rotated (F15)."""
        return self._rotations

    def start(self, path: Path | None = None) -> Path:
        """Open a log file and start recording.  Returns the path used."""
        if self._handle is not None:
            return self._path or Path()
        target = path or self._default_path()
        target.parent.mkdir(parents=True, exist_ok=True)
        # Append: a reconnect continues the same session's log rather than
        # truncating what was already captured.
        self._handle = target.open("ab")
        self._path = target
        self._written = target.stat().st_size
        self._line_open = False
        self._timestamper = Timestamper(self.settings.timestamps, started_at=self.now())
        self._decoder = IncrementalTextDecoder(self.settings.charset)
        self._last_flush = self.now()
        return target

    def stop(self) -> None:
        """Flush and close.  Idempotent."""
        handle, self._handle = self._handle, None
        if handle is None:
            return
        if self._line_open:
            self._buffer += b"\n"
            self._line_open = False
        self._flush_buffer(handle)
        handle.close()

    def attach(self, session: object) -> Callable[[], None]:
        """Subscribe to a session's events; returns an unsubscribe callable.

        With ``auto_start`` the file opens as soon as the session connects and
        closes when it disconnects (F15).
        """
        from portside.core.events import BytesReceived, ErrorRaised, StateChanged
        from portside.core.session import SessionState

        events = session.events  # type: ignore[attr-defined]

        def listen(event: Event) -> None:
            if isinstance(event, BytesReceived):
                self.write(event.data)
            elif isinstance(event, StateChanged):
                if self.settings.auto_start and event.new is SessionState.CONNECTED:
                    self.start()
                elif event.new in {SessionState.CLOSED, SessionState.DISCONNECTED}:
                    self.stop()
            elif isinstance(event, ErrorRaised) and self.is_logging:
                self.write_note(f"error: {event.error}")

        self._unsubscribe = events.subscribe(listen)
        return self.detach

    def detach(self) -> None:
        """Stop listening and close the file."""
        if self._unsubscribe is not None:
            self._unsubscribe()
            self._unsubscribe = None
        self.stop()

    # -- writing -----------------------------------------------------------

    def write(self, data: bytes) -> None:
        """Record bytes received from the device."""
        if self._handle is None or not data:
            return
        payload = data if self.settings.log_format is LogFormat.RAW else self._as_text(data)
        self._buffer += payload
        self._maybe_flush()

    def write_note(self, note: str) -> None:
        """Record a Portside-generated line (errors, break sent, paste started).

        Notes are for humans reading a text log; a raw log stays byte-exact and
        ignores them.
        """
        if self._handle is None or self.settings.log_format is LogFormat.RAW:
            return
        if self._line_open:
            self._buffer += b"\n"
            self._line_open = False
        stamp = self._timestamper.prefix(self.now())
        self._buffer += f"{stamp}[portside] {note}\n".encode()
        self._maybe_flush()

    def _as_text(self, data: bytes) -> bytes:
        """Decode, normalise endings to LF, and stamp each line (F15, F16)."""
        assert self._decoder is not None
        text = self._decoder.decode(data)
        if not text:
            return b""
        # Internally LF, on every platform, as the PRD requires.
        text = text.replace("\r\n", "\n").replace("\r", "\n")
        out: list[str] = []
        for char in text:
            if not self._line_open:
                # A line is stamped when its first character arrives, not when
                # it is completed - otherwise a slow device skews every stamp.
                out.append(self._timestamper.prefix(self.now()))
                self._line_open = True
            out.append(char)
            if char == "\n":
                self._line_open = False
        return "".join(out).encode("utf-8", "replace")

    # -- flushing and rotation --------------------------------------------

    def _maybe_flush(self) -> None:
        moment = self.now()
        due = moment - self._last_flush >= self.settings.flush_interval
        if due or self._rotation_due():
            handle = self._handle
            if handle is not None:
                self._flush_buffer(handle)
            self._last_flush = moment
            self._rotate_if_needed()

    def _rotation_due(self) -> bool:
        return bool(self.settings.max_bytes) and self.bytes_written >= self.settings.max_bytes

    def _flush_buffer(self, handle: IO[bytes]) -> None:
        if not self._buffer:
            return
        chunk = bytes(self._buffer)
        self._buffer.clear()
        handle.write(chunk)
        handle.flush()
        self._written += len(chunk)

    def flush(self) -> None:
        """Force pending data to disk (used on close and by tests)."""
        handle = self._handle
        if handle is not None:
            self._flush_buffer(handle)

    def _rotate_if_needed(self) -> None:
        """Start a new file once the current one is big enough (F15)."""
        if not self._rotation_due() or self._handle is None or self._path is None:
            return
        handle, path = self._handle, self._path
        self._flush_buffer(handle)
        handle.close()
        self._rotations += 1
        rotated = path.with_name(f"{path.stem}.{self._rotations}{path.suffix}")
        path.rename(rotated)
        self._prune(path)
        self._handle = path.open("ab")
        self._written = 0

    def _prune(self, path: Path) -> None:
        """Delete the oldest rotated files beyond ``keep``."""
        if self.settings.keep <= 0:
            return
        pattern = f"{path.stem}.*{path.suffix}"
        rotated = sorted(
            path.parent.glob(pattern),
            key=lambda item: item.stat().st_mtime,
        )
        for stale in rotated[: max(0, len(rotated) - self.settings.keep)]:
            stale.unlink(missing_ok=True)

    # -- helpers -----------------------------------------------------------

    def _default_path(self) -> Path:
        directory = self.settings.directory
        if directory is None:
            from portside.platform import get_platform

            directory = get_platform().log_dir()
        name = resolve_template(
            self.settings.template,
            port=self.port,
            profile=self.profile,
            when=datetime.fromtimestamp(self.now()).astimezone(),
        )
        return directory / name

    def reset_elapsed(self) -> None:
        """Restart the elapsed-time clock (F16)."""
        self._timestamper.reset(self.now())

    def restamp(self, mode: TimestampMode) -> None:
        """Change the timestamp mode of a log that is already open (F16)."""
        self.settings = replace(self.settings, timestamps=mode)
        self._timestamper.mode = mode
