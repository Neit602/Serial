"""The transport interface (PRD: "the only place OS or hardware differences leak in").

Transports are deliberately **blocking**: ``read`` and ``write`` are called from
a dedicated I/O thread owned by :class:`portside.core.session.Session`, which
bridges them to asyncio.  Keeping the interface synchronous is what lets a
pySerial port, an in-memory loopback and (later) a direct-USB backend share one
contract.
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass
from types import TracebackType
from typing import Self

from portside.core.settings import SerialSettings

__all__ = ["DEFAULT_BREAK_DURATION", "ModemLines", "Transport"]

#: PRD F12 default break length.
DEFAULT_BREAK_DURATION = 0.25


@dataclass(frozen=True, slots=True)
class ModemLines:
    """Input modem lines (PRD F4).

    A field is ``None`` when the driver does not report that line — several
    USB adapters and virtual ports do not.  ``None`` means "unknown", never
    "low", so a UI can grey the indicator out instead of lying about it.
    """

    cts: bool | None = None
    dsr: bool | None = None
    dcd: bool | None = None
    ri: bool | None = None

    def describe(self) -> str:
        """``CTS DSR dcd --`` style summary: upper = asserted, ``--`` = unknown."""
        parts = []
        for name, value in (
            ("cts", self.cts),
            ("dsr", self.dsr),
            ("dcd", self.dcd),
            ("ri", self.ri),
        ):
            if value is None:
                parts.append("--")
            else:
                parts.append(name.upper() if value else name)
        return " ".join(parts)


class Transport(ABC):
    """A byte pipe to one device, plus the control lines a serial port has."""

    @property
    @abstractmethod
    def name(self) -> str:
        """Port name or URL this transport talks to (for messages and logs)."""

    @property
    @abstractmethod
    def is_open(self) -> bool:
        """True between a successful :meth:`open` and :meth:`close`."""

    @property
    @abstractmethod
    def settings(self) -> SerialSettings:
        """The line settings currently in force."""

    @abstractmethod
    def open(self) -> None:
        """Open the port.

        Raises:
            PermissionDeniedError, PortBusyError, PortNotFoundError,
            DeviceVanishedError: mapped from the underlying driver error (F6).
        """

    @abstractmethod
    def close(self) -> None:
        """Close the port.  Idempotent and never raises."""

    @abstractmethod
    def read(self, max_bytes: int = 4096, timeout: float | None = 0.1) -> bytes:
        """Read up to ``max_bytes``, waiting at most ``timeout`` seconds.

        Returns ``b""`` on timeout — a timeout is not an error.  Raises
        :class:`DeviceVanishedError` if the device went away.
        """

    @abstractmethod
    def write(self, data: bytes) -> int:
        """Write ``data``, returning the number of bytes accepted."""

    @abstractmethod
    def apply_settings(self, settings: SerialSettings) -> None:
        """Apply new line settings, on an open or a closed port (F3)."""

    @abstractmethod
    def send_break(self, duration: float = DEFAULT_BREAK_DURATION) -> None:
        """Assert a break condition for ``duration`` seconds (F12).

        Raises :class:`NotImplementedError` if the driver cannot do it, which is
        the hook M2's emulated break hangs off.
        """

    @abstractmethod
    def set_dtr(self, value: bool) -> bool:
        """Drive DTR (F4).

        Returns False if the driver does not implement the line, so a UI can
        grey the control out.  Not being able to toggle DTR is a limitation of
        the adapter, not a session-ending error.
        """

    @abstractmethod
    def set_rts(self, value: bool) -> bool:
        """Drive RTS (F4).  Returns False if the driver does not implement it."""

    @abstractmethod
    def get_modem_lines(self) -> ModemLines:
        """Read CTS/DSR/DCD/RI; unsupported lines come back as ``None`` (F4)."""

    def cancel_read(self) -> None:
        """Unblock a :meth:`read` in progress, if the backend can.  Best effort."""

    def flush_output(self) -> None:
        """Block until buffered output has been handed to the driver."""

    def reset_buffers(self) -> None:
        """Discard buffered input and output."""

    def __enter__(self) -> Self:
        self.open()
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        self.close()

    def __repr__(self) -> str:
        state = "open" if self.is_open else "closed"
        return f"<{type(self).__name__} {self.name} {state}>"
