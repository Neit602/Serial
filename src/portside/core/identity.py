"""Stable device identity (PRD F2).

A USB adapter shows up as ``/dev/ttyUSB0`` today, ``/dev/ttyUSB1`` after the
next replug and ``COM7`` on a colleague's laptop.  Profiles must follow the
*device*, so they are keyed by VID:PID plus serial number.

Adapters without a serial number (most CH340s, many PL2303 clones) get a
``weak`` identity derived from VID:PID and the port description: still useful
for auto-applying a profile, but not unique if two identical adapters are
plugged in at once, which is why the flag is exposed rather than hidden.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

from portside.core.ports import PortInfo

__all__ = ["DeviceIdentity"]

_UNSAFE = re.compile(r"[^A-Za-z0-9_.-]+")


def _slug(value: str) -> str:
    """Reduce ``value`` to characters that are safe in a TOML bare key."""
    return _UNSAFE.sub("_", value).strip("_") or "unknown"


@dataclass(frozen=True, slots=True)
class DeviceIdentity:
    """Identity of the *device* behind a port, independent of the port name."""

    vid: int | None
    pid: int | None
    serial_number: str | None
    fallback: str | None = None
    weak: bool = False

    @classmethod
    def from_port(cls, port: PortInfo) -> DeviceIdentity:
        """Derive an identity from a discovered port."""
        if port.vid is not None and port.pid is not None and port.serial_number:
            return cls(port.vid, port.pid, port.serial_number, weak=False)
        if port.vid is not None and port.pid is not None:
            return cls(port.vid, port.pid, None, fallback=port.description, weak=True)
        # Non-USB port: nothing but the port name identifies it.
        return cls(None, None, None, fallback=port.device, weak=True)

    def key(self) -> str:
        """Stable string key, safe to use as a TOML table name."""
        if self.vid is None or self.pid is None:
            return f"port_{_slug(self.fallback or 'unknown')}"
        vid_pid = f"{self.vid:04x}_{self.pid:04x}"
        if self.serial_number:
            return f"usb_{vid_pid}_{_slug(self.serial_number)}"
        if self.fallback:
            return f"usb_{vid_pid}_desc_{_slug(self.fallback)}"
        return f"usb_{vid_pid}"

    def matches(self, port: PortInfo) -> bool:
        """True if ``port`` is (as far as can be told) this same device."""
        return self.from_port(port) == self

    def describe(self) -> str:
        """Human readable identity for status lines and logs."""
        if self.vid is None or self.pid is None:
            return f"port {self.fallback or 'unknown'}"
        vid_pid = f"{self.vid:04x}:{self.pid:04x}"
        if self.serial_number:
            return f"USB {vid_pid} serial {self.serial_number}"
        return f"USB {vid_pid} (no serial number)"
