"""Break signalling, native and emulated (PRD F12).

A break is a longer-than-a-character span of logical zero on the line.  Cisco,
Juniper and most boot monitors use it to interrupt boot or drop to ROMMON, so
it has to work even when the adapter's driver does not implement it.

The emulated fallback exploits how a UART frames a byte: at a low enough baud
rate, one start bit plus eight zero bits already lasts longer than the break
we want, so sending ``0x00`` at that rate looks like a break to the receiver.
"""

from __future__ import annotations

import math
import time
from collections.abc import Callable
from dataclasses import dataclass
from enum import StrEnum

from portside.core.errors import PortsideError
from portside.core.settings import BAUD_MIN, SerialSettings
from portside.transport.base import Transport

__all__ = [
    "BREAK_BITS",
    "BreakMode",
    "BreakResult",
    "baud_for_break",
    "send_break",
    "zero_bytes_for_break",
]

#: A byte on the wire is 1 start bit + 8 data bits + 1 stop bit.  Only the
#: start bit and the eight zero bits are low, which is what the receiver sees
#: as a break condition.
BREAK_BITS: int = 9


class BreakMode(StrEnum):
    """How :func:`send_break` should produce the break (F12)."""

    AUTO = "auto"
    NATIVE = "native"
    EMULATED = "emulated"


@dataclass(frozen=True, slots=True)
class BreakResult:
    """What actually happened, so the UI can say so."""

    emulated: bool
    duration: float
    baudrate: int | None = None
    #: Zero bytes sent; more than one when the lowest baud rate is still too
    #: fast to cover the requested duration with a single byte.
    bytes_sent: int = 0

    def describe(self) -> str:
        """One line for the status bar or the log."""
        if not self.emulated:
            return f"native break, {self.duration * 1000:.0f} ms"
        plural = "" if self.bytes_sent == 1 else "s"
        return (
            f"emulated break, {self.duration * 1000:.0f} ms at {self.baudrate} baud "
            f"({self.bytes_sent} zero byte{plural})"
        )


def baud_for_break(duration: float, *, minimum: int = BAUD_MIN) -> int:
    """Highest baud rate at which one zero byte lasts at least ``duration``.

    Lower is safer but slower to restore, so the fastest rate that still
    satisfies the requested duration is chosen, floored at ``minimum`` — no
    driver is obliged to accept an arbitrarily slow rate.
    """
    if duration <= 0:
        return minimum
    ideal = int(BREAK_BITS / duration)
    return max(minimum, ideal)


def zero_bytes_for_break(duration: float, baudrate: int) -> int:
    """How many zero bytes are needed to cover ``duration`` at ``baudrate``.

    At the 50 baud floor a single byte is only 180 ms, which is short of the
    PRD's 250 ms default, so the byte is repeated.  Each repeat is separated by
    one stop bit; receivers detect the break from the framing error either way,
    and erring long is safer than erring short — a break that is too short
    simply does not interrupt the device.
    """
    per_byte = BREAK_BITS / baudrate
    return max(1, math.ceil(duration / per_byte))


def send_break(
    transport: Transport,
    duration: float = 0.25,
    *,
    mode: BreakMode = BreakMode.AUTO,
    sleep: Callable[[float], None] = time.sleep,
) -> BreakResult:
    """Send a break on ``transport``.

    ``auto`` tries the driver's own break and falls back to the emulated one;
    ``native`` and ``emulated`` force a single strategy, which matters when a
    driver claims to support break but does not actually assert it.
    """
    if duration <= 0:
        raise PortsideError("break duration must be positive", port=transport.name)

    if mode in (BreakMode.AUTO, BreakMode.NATIVE):
        try:
            transport.send_break(duration)
        except NotImplementedError:
            if mode is BreakMode.NATIVE:
                raise PortsideError(
                    f"{transport.name} cannot send a native break",
                    port=transport.name,
                    remedy=(
                        "This adapter's driver does not implement a break condition.\n"
                        "Use the emulated break instead (break mode 'emulated', or "
                        "'auto' which falls back to it automatically)."
                    ),
                ) from None
        else:
            return BreakResult(emulated=False, duration=duration)

    return _emulated_break(transport, duration, sleep=sleep)


def _emulated_break(
    transport: Transport, duration: float, *, sleep: Callable[[float], None]
) -> BreakResult:
    """Drop to a low baud rate, send 0x00, then restore the line settings."""
    original: SerialSettings = transport.settings
    baudrate = baud_for_break(duration)
    count = zero_bytes_for_break(duration, baudrate)
    slow = original.evolve(baudrate=baudrate)
    try:
        transport.apply_settings(slow)
        transport.write(b"\x00" * count)
        transport.flush_output()
        # flush_output only guarantees the driver has the bytes; wait out the
        # time they take to clock onto the wire before restoring the baud rate.
        sleep(count * BREAK_BITS / baudrate)
    finally:
        # The line settings must come back even if the write failed, or the
        # session would be left at 50 baud.
        transport.apply_settings(original)
    return BreakResult(emulated=True, duration=duration, baudrate=baudrate, bytes_sent=count)
