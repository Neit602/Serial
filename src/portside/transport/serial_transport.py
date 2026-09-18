"""pySerial-backed transport — the primary path on all three platforms.

Blocking by design; :class:`portside.core.session.Session` runs it on its own
I/O thread.  Every call that can fail goes through
:func:`portside.transport.errors.map_serial_exception`, so callers above this
module never see a :class:`serial.SerialException`.
"""

from __future__ import annotations

import contextlib
import errno

import serial

from portside.core.errors import TransportClosedError
from portside.core.settings import SerialSettings
from portside.platform import PlatformSupport, get_platform
from portside.transport.base import DEFAULT_BREAK_DURATION, ModemLines, Transport
from portside.transport.errors import map_serial_exception

__all__ = ["PySerialTransport"]

#: Read timeout used when the caller passes ``None`` — short enough that
#: ``close()`` never waits long, long enough not to spin the I/O thread.
_DEFAULT_READ_TIMEOUT = 0.1

#: errnos that mean "this driver does not implement that control line" rather
#: than "the device is gone".  PTYs, many USB bridges and Bluetooth SPP ports
#: raise these for DTR/RTS/break.
_UNSUPPORTED_ERRNOS = frozenset({errno.ENOTTY, errno.EINVAL, errno.ENOSYS, errno.EOPNOTSUPP})


def _is_unsupported(exc: BaseException) -> bool:
    """True if ``exc`` means the driver lacks the feature, not that it failed."""
    if isinstance(exc, NotImplementedError):
        return True
    err = getattr(exc, "errno", None)
    if err is None:
        cause = exc.__cause__ or exc.__context__
        err = getattr(cause, "errno", None)
    return err in _UNSUPPORTED_ERRNOS


class PySerialTransport(Transport):
    """A real serial port (or any pySerial URL, such as ``loop://``)."""

    def __init__(
        self,
        port: str,
        settings: SerialSettings | None = None,
        *,
        platform: PlatformSupport | None = None,
    ) -> None:
        self._port = port
        self._settings = settings or SerialSettings()
        self._platform = platform or get_platform()
        self._serial: serial.Serial | None = None

    # -- identity ----------------------------------------------------------

    @property
    def name(self) -> str:
        return self._port

    @property
    def is_open(self) -> bool:
        return self._serial is not None and bool(self._serial.is_open)

    @property
    def settings(self) -> SerialSettings:
        return self._settings

    @property
    def is_url(self) -> bool:
        """True for pySerial URL handles (``loop://``, ``socket://``, ...)."""
        return "://" in self._port

    # -- lifecycle ---------------------------------------------------------

    def open(self) -> None:
        if self.is_open:
            return
        kwargs = self._settings.to_pyserial_kwargs()
        kwargs["timeout"] = _DEFAULT_READ_TIMEOUT
        kwargs["write_timeout"] = None
        try:
            if self.is_url:
                self._serial = serial.serial_for_url(self._port, **kwargs)
            else:
                self._serial = serial.Serial(self._port, **kwargs)
        except Exception as exc:
            self._serial = None
            raise self._map(exc) from exc

    def close(self) -> None:
        port, self._serial = self._serial, None
        if port is None:
            return
        with contextlib.suppress(Exception):  # closing must never raise
            port.close()

    # -- I/O ---------------------------------------------------------------

    def read(self, max_bytes: int = 4096, timeout: float | None = 0.1) -> bytes:
        port = self._require_open()
        try:
            if timeout != port.timeout:
                port.timeout = timeout if timeout is not None else _DEFAULT_READ_TIMEOUT
            waiting = port.in_waiting
            if waiting:
                return bytes(port.read(min(waiting, max_bytes)))
            # Block for at most `timeout`, returning as soon as one byte lands.
            first = port.read(1)
            if not first:
                return b""
            waiting = port.in_waiting
            if waiting:
                return bytes(first) + bytes(port.read(min(waiting, max_bytes - 1)))
            return bytes(first)
        except Exception as exc:
            raise self._map(exc, while_open=True) from exc

    def write(self, data: bytes) -> int:
        port = self._require_open()
        try:
            written = port.write(data)
        except Exception as exc:
            raise self._map(exc, while_open=True) from exc
        return int(written or 0)

    def flush_output(self) -> None:
        port = self._require_open()
        try:
            port.flush()
        except Exception as exc:
            raise self._map(exc, while_open=True) from exc

    def reset_buffers(self) -> None:
        port = self._require_open()
        try:
            port.reset_input_buffer()
            port.reset_output_buffer()
        except Exception as exc:
            raise self._map(exc, while_open=True) from exc

    def cancel_read(self) -> None:
        port = self._serial
        if port is None:
            return
        cancel = getattr(port, "cancel_read", None)
        if cancel is None:
            return
        with contextlib.suppress(Exception):  # best effort; not every backend has it
            cancel()

    # -- control -----------------------------------------------------------

    def apply_settings(self, settings: SerialSettings) -> None:
        settings.validate()
        self._settings = settings
        port = self._serial
        if port is None:
            return
        try:
            for key, value in settings.to_pyserial_kwargs().items():
                setattr(port, key, value)
        except Exception as exc:
            raise self._map(exc, while_open=True) from exc

    def send_break(self, duration: float = DEFAULT_BREAK_DURATION) -> None:
        port = self._require_open()
        try:
            port.send_break(duration)
        except Exception as exc:
            if _is_unsupported(exc):
                # M2 catches this and falls back to an emulated break (F12).
                raise NotImplementedError(
                    f"{self._port}: this driver cannot send a native break"
                ) from exc
            raise self._map(exc, while_open=True) from exc

    def set_dtr(self, value: bool) -> bool:
        return self._set_line("dtr", value)

    def set_rts(self, value: bool) -> bool:
        return self._set_line("rts", value)

    def _set_line(self, attr: str, value: bool) -> bool:
        """Drive an output modem line, reporting drivers that do not have one."""
        port = self._require_open()
        try:
            setattr(port, attr, value)
        except Exception as exc:
            if _is_unsupported(exc):
                return False
            raise self._map(exc, while_open=True) from exc
        return True

    def get_modem_lines(self) -> ModemLines:
        port = self._require_open()

        def line(attr: str) -> bool | None:
            # Virtual ports and some drivers raise or omit these entirely;
            # "unknown" is reported as None rather than guessed as False.
            try:
                value = getattr(port, attr)
            except (NotImplementedError, AttributeError, OSError, serial.SerialException):
                return None
            return None if value is None else bool(value)

        return ModemLines(cts=line("cts"), dsr=line("dsr"), dcd=line("cd"), ri=line("ri"))

    # -- helpers -----------------------------------------------------------

    def _require_open(self) -> serial.Serial:
        port = self._serial
        if port is None or not port.is_open:
            raise TransportClosedError(f"{self._port} is not open", port=self._port)
        return port

    def _map(self, exc: BaseException, *, while_open: bool = False) -> BaseException:
        if isinstance(exc, TransportClosedError):
            return exc
        return map_serial_exception(exc, self._port, platform=self._platform, while_open=while_open)
