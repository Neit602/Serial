"""In-memory transport for tests and demos.

Two things real hardware cannot do on demand: behave deterministically, and
fail exactly when you want it to.  :class:`LoopbackTransport` does both — it
echoes what is written to it (or plays a canned script) and can be told to
refuse to open, or to make the device vanish after N bytes, so the session's
error and reconnect paths are exercised without hardware.
"""

from __future__ import annotations

import threading
from collections import deque
from collections.abc import Callable

from portside.core.errors import TransportClosedError, TransportError
from portside.core.settings import SerialSettings
from portside.transport.base import DEFAULT_BREAK_DURATION, ModemLines, Transport

__all__ = ["LoopbackTransport"]


class LoopbackTransport(Transport):
    """A loopback port with optional fault injection.

    Args:
        name: the name this transport reports.
        settings: initial line settings.
        echo: if True (default) written bytes become readable bytes.
        fail_open_with: raised by :meth:`open` — for permission/busy tests.
        vanish_after: raise :class:`DeviceVanishedError` once this many bytes
            have been read or written.
        modem_lines: what :meth:`get_modem_lines` reports.
        supports_control_lines: when False, DTR/RTS report "not supported" the
            way a PTY or a minimal USB bridge does.
    """

    def __init__(
        self,
        name: str = "loopback://",
        settings: SerialSettings | None = None,
        *,
        echo: bool = True,
        fail_open_with: TransportError | Callable[[], TransportError] | None = None,
        vanish_after: int | None = None,
        modem_lines: ModemLines | None = None,
        supports_control_lines: bool = True,
    ) -> None:
        self._name = name
        self._settings = settings or SerialSettings()
        self._echo = echo
        self._fail_open_with = fail_open_with
        self._vanish_after = vanish_after
        self._modem_lines = modem_lines or ModemLines(cts=True, dsr=True, dcd=False, ri=False)
        self._supports_control_lines = supports_control_lines
        self._open = False
        self._buffer = bytearray()
        self._lock = threading.Lock()
        self._data_ready = threading.Condition(self._lock)
        self._cancelled = False
        self._transferred = 0
        #: Everything the "device" was sent, for assertions in tests.
        self.written = bytearray()
        #: Breaks sent, as durations, for assertions in tests.
        self.breaks: deque[float] = deque()
        self.dtr: bool | None = None
        self.rts: bool | None = None
        self.open_count = 0

    # -- test helpers ------------------------------------------------------

    def feed(self, data: bytes) -> None:
        """Make ``data`` readable, as if the device had sent it."""
        with self._data_ready:
            self._buffer.extend(data)
            self._data_ready.notify_all()

    def set_fault(self, *, vanish_after: int | None = None) -> None:
        """Arm (or disarm) the vanish fault at runtime."""
        self._vanish_after = vanish_after
        self._transferred = 0

    # -- identity ----------------------------------------------------------

    @property
    def name(self) -> str:
        return self._name

    @property
    def is_open(self) -> bool:
        return self._open

    @property
    def settings(self) -> SerialSettings:
        return self._settings

    # -- lifecycle ---------------------------------------------------------

    def open(self) -> None:
        if self._open:
            return
        if self._fail_open_with is not None:
            error = (
                self._fail_open_with() if callable(self._fail_open_with) else self._fail_open_with
            )
            raise error
        self.open_count += 1
        self._open = True
        self._cancelled = False
        self._transferred = 0

    def close(self) -> None:
        with self._data_ready:
            self._open = False
            self._data_ready.notify_all()

    # -- I/O ---------------------------------------------------------------

    def read(self, max_bytes: int = 4096, timeout: float | None = 0.1) -> bytes:
        self._require_open()
        with self._data_ready:
            if not self._buffer and not self._cancelled:
                self._data_ready.wait(timeout)
            if self._cancelled:
                self._cancelled = False
                return b""
            if not self._open:
                return b""
            chunk = bytes(self._buffer[:max_bytes])
            del self._buffer[: len(chunk)]
        self._account(len(chunk))
        return chunk

    def write(self, data: bytes) -> int:
        self._require_open()
        self.written.extend(data)
        if self._echo:
            self.feed(bytes(data))
        self._account(len(data))
        return len(data)

    def cancel_read(self) -> None:
        with self._data_ready:
            self._cancelled = True
            self._data_ready.notify_all()

    # -- control -----------------------------------------------------------

    def apply_settings(self, settings: SerialSettings) -> None:
        settings.validate()
        self._settings = settings

    def send_break(self, duration: float = DEFAULT_BREAK_DURATION) -> None:
        self._require_open()
        self.breaks.append(duration)

    def set_dtr(self, value: bool) -> bool:
        self._require_open()
        if not self._supports_control_lines:
            return False
        self.dtr = value
        return True

    def set_rts(self, value: bool) -> bool:
        self._require_open()
        if not self._supports_control_lines:
            return False
        self.rts = value
        return True

    def get_modem_lines(self) -> ModemLines:
        self._require_open()
        return self._modem_lines

    # -- helpers -----------------------------------------------------------

    def _require_open(self) -> None:
        if not self._open:
            raise TransportClosedError(f"{self._name} is not open", port=self._name)

    def _account(self, count: int) -> None:
        """Count traffic and trip the vanish fault when it is armed."""
        if self._vanish_after is None or count == 0:
            return
        self._transferred += count
        if self._transferred >= self._vanish_after:
            from portside.core.errors import DeviceVanishedError

            self._open = False
            raise DeviceVanishedError(
                f"{self._name} disappeared",
                port=self._name,
                remedy="Simulated unplug from LoopbackTransport.",
            )
