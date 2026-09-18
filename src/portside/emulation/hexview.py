"""Hex and mixed hex/ASCII rendering (PRD F10).

Used by the TUI's hex view and by the raw-mode log formatter.  The layout is
the classic ``hexdump -C`` shape, which most engineers can read at a glance.
"""

from __future__ import annotations

from collections.abc import Iterator
from dataclasses import dataclass
from enum import StrEnum

__all__ = ["HexRow", "ViewMode", "format_hex", "hex_rows"]

_PRINTABLE_LOW = 0x20
_PRINTABLE_HIGH = 0x7E


class ViewMode(StrEnum):
    """How received data is presented (F10)."""

    TEXT = "text"
    HEX = "hex"
    MIXED = "mixed"


@dataclass(frozen=True, slots=True)
class HexRow:
    """One rendered row: offset, the raw bytes, and their printable form."""

    offset: int
    data: bytes
    width: int = 16

    @property
    def hex_columns(self) -> str:
        """Space-separated hex pairs, padded so rows stay aligned."""
        pairs = [f"{byte:02x}" for byte in self.data]
        pairs += ["  "] * (self.width - len(self.data))
        # A gap in the middle makes counting bytes much easier by eye.
        half = self.width // 2
        return " ".join(pairs[:half]) + "  " + " ".join(pairs[half:])

    @property
    def ascii_columns(self) -> str:
        """Printable ASCII, non-printables shown as ``.`` (hexdump convention)."""
        return "".join(
            chr(byte) if _PRINTABLE_LOW <= byte <= _PRINTABLE_HIGH else "." for byte in self.data
        )

    def render(self) -> str:
        """``00000010  48 65 6c 6c ...  |Hello|`` — one complete line."""
        return f"{self.offset:08x}  {self.hex_columns}  |{self.ascii_columns}|"


def hex_rows(data: bytes, *, offset: int = 0, width: int = 16) -> Iterator[HexRow]:
    """Split ``data`` into rows of ``width`` bytes, numbered from ``offset``."""
    if width <= 0:
        raise ValueError("width must be positive")
    for start in range(0, len(data), width):
        yield HexRow(offset=offset + start, data=data[start : start + width], width=width)


def format_hex(
    data: bytes,
    *,
    offset: int = 0,
    width: int = 16,
    strip_high_bit: bool = False,
) -> str:
    """Render ``data`` as a hex dump.

    ``strip_high_bit`` masks bit 7 first, matching the decoder's option (F10) so
    the hex view and the text view agree about what the device sent.
    """
    if strip_high_bit:
        data = bytes(byte & 0x7F for byte in data)
    return "\n".join(row.render() for row in hex_rows(data, offset=offset, width=width))
