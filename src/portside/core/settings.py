"""Serial port settings and their validation (PRD F3)."""

from __future__ import annotations

from dataclasses import dataclass, replace
from enum import Enum, StrEnum
from typing import Any, Final

from portside.core.errors import InvalidSettingsError

__all__ = [
    "BAUD_MAX",
    "BAUD_MIN",
    "DATA_BITS_CHOICES",
    "FlowControl",
    "Parity",
    "SerialSettings",
    "StopBits",
]

BAUD_MIN: Final = 50
BAUD_MAX: Final = 4_000_000
DATA_BITS_CHOICES: Final = (5, 6, 7, 8)


class Parity(StrEnum):
    """Parity modes, spelled the way pySerial spells them."""

    NONE = "N"
    EVEN = "E"
    ODD = "O"
    MARK = "M"
    SPACE = "S"


class StopBits(float, Enum):
    """Stop bits; 1.5 is only meaningful with 5 data bits on most hardware."""

    ONE = 1.0
    ONE_POINT_FIVE = 1.5
    TWO = 2.0


class FlowControl(StrEnum):
    """Flow control modes."""

    NONE = "none"
    RTSCTS = "rtscts"
    DSRDTR = "dsrdtr"
    XONXOFF = "xonxoff"


@dataclass(frozen=True, slots=True)
class SerialSettings:
    """An immutable set of line settings.

    Validated on construction so an invalid combination can never reach a
    transport.  ``replace()`` returns a new validated instance.
    """

    baudrate: int = 115200
    data_bits: int = 8
    parity: Parity = Parity.NONE
    stop_bits: StopBits = StopBits.ONE
    flow_control: FlowControl = FlowControl.NONE

    def __post_init__(self) -> None:
        self.validate()

    def validate(self) -> None:
        """Raise :class:`InvalidSettingsError` if any field is out of range."""
        # Guards against untyped input (TOML profiles, CLI, plugins); bool is an
        # int subclass, so True would otherwise sneak through as baud rate 1.
        if isinstance(self.baudrate, bool) or not isinstance(self.baudrate, int):
            raise InvalidSettingsError(f"baud rate must be an integer, got {self.baudrate!r}")
        if not BAUD_MIN <= self.baudrate <= BAUD_MAX:
            raise InvalidSettingsError(
                f"baud rate {self.baudrate} is outside the supported range {BAUD_MIN}-{BAUD_MAX}"
            )
        if self.data_bits not in DATA_BITS_CHOICES:
            raise InvalidSettingsError(
                f"data bits must be one of {', '.join(str(c) for c in DATA_BITS_CHOICES)}, "
                f"got {self.data_bits!r}"
            )
        if not isinstance(self.parity, Parity):
            raise InvalidSettingsError(f"unknown parity {self.parity!r}")
        if not isinstance(self.stop_bits, StopBits):
            raise InvalidSettingsError(f"unknown stop bits {self.stop_bits!r}")
        if not isinstance(self.flow_control, FlowControl):
            raise InvalidSettingsError(f"unknown flow control {self.flow_control!r}")

    def evolve(self, **changes: Any) -> SerialSettings:
        """Return a copy with ``changes`` applied, validated."""
        return replace(self, **changes)

    def to_dict(self) -> dict[str, Any]:
        """Plain data, ready for TOML (M2 profiles)."""
        return {
            "baudrate": self.baudrate,
            "data_bits": self.data_bits,
            "parity": self.parity.value,
            "stop_bits": self.stop_bits.value,
            "flow_control": self.flow_control.value,
        }

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> SerialSettings:
        """Inverse of :meth:`to_dict`, with validation of unknown values."""
        try:
            parity = Parity(data.get("parity", Parity.NONE.value))
            stop_bits = StopBits(float(data.get("stop_bits", StopBits.ONE.value)))
            flow_control = FlowControl(data.get("flow_control", FlowControl.NONE.value))
        except ValueError as exc:
            raise InvalidSettingsError(str(exc)) from exc
        return cls(
            baudrate=int(data.get("baudrate", 115200)),
            data_bits=int(data.get("data_bits", 8)),
            parity=parity,
            stop_bits=stop_bits,
            flow_control=flow_control,
        )

    def to_pyserial_kwargs(self) -> dict[str, Any]:
        """Keyword arguments for :class:`serial.Serial`."""
        return {
            "baudrate": self.baudrate,
            "bytesize": self.data_bits,
            "parity": self.parity.value,
            "stopbits": self.stop_bits.value,
            "xonxoff": self.flow_control is FlowControl.XONXOFF,
            "rtscts": self.flow_control is FlowControl.RTSCTS,
            "dsrdtr": self.flow_control is FlowControl.DSRDTR,
        }

    def describe(self) -> str:
        """Short ``115200 8N1`` style summary for status bars and banners."""
        stop = (
            "1.5" if self.stop_bits is StopBits.ONE_POINT_FIVE else str(int(self.stop_bits.value))
        )
        return (
            f"{self.baudrate} {self.data_bits}{self.parity.value}{stop} {self.flow_control.value}"
        )
