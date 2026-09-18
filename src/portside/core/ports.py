"""The port description handed to UIs (PRD F1).

This lives in :mod:`portside.core` rather than in :mod:`portside.transport` so
that both the platform layer and the discovery code can depend on it without a
circular import.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

__all__ = ["PortInfo"]


@dataclass(frozen=True, slots=True)
class PortInfo:
    """Everything known about one serial port.

    Every field except ``device`` may be ``None``: plain motherboard UARTs and
    Bluetooth ports carry no USB identity, and drivers differ in what they
    report.  Nothing here is ever guessed.
    """

    device: str
    description: str | None = None
    vid: int | None = None
    pid: int | None = None
    serial_number: str | None = None
    manufacturer: str | None = None
    product: str | None = None
    chip_family: str | None = None
    driver: str | None = None
    stable_path: str | None = None
    extra: dict[str, str] = field(default_factory=dict)

    @property
    def vid_pid(self) -> str | None:
        """``"0403:6001"`` style identifier, or ``None`` for non-USB ports."""
        if self.vid is None or self.pid is None:
            return None
        return f"{self.vid:04x}:{self.pid:04x}"

    def label(self) -> str:
        """One-line friendly name, e.g. ``/dev/ttyUSB0 — FT232R (0403:6001)``."""
        bits = [self.chip_family or self.description or "serial port"]
        if self.vid_pid:
            bits.append(f"({self.vid_pid})")
        if self.serial_number:
            bits.append(f"SN {self.serial_number}")
        return f"{self.device} — {' '.join(bits)}"

    def to_dict(self) -> dict[str, Any]:
        """JSON-friendly mapping for ``portside --list --json``."""
        return {
            "device": self.device,
            "description": self.description,
            "vid": self.vid,
            "pid": self.pid,
            "vid_pid": self.vid_pid,
            "serial_number": self.serial_number,
            "manufacturer": self.manufacturer,
            "product": self.product,
            "chip_family": self.chip_family,
            "driver": self.driver,
            "stable_path": self.stable_path,
            "extra": dict(self.extra),
        }
