"""Linux platform backend.

Adds the two things Linux users actually need: the stable
``/dev/serial/by-id/...`` path, the kernel driver currently bound to the port,
and a permission remedy naming the group that really owns the device node
(``dialout`` on Debian/Ubuntu, ``uucp`` on Arch and friends).
"""

from __future__ import annotations

import grp
from collections.abc import Sequence
from pathlib import Path

from portside.core.ports import PortInfo
from portside.platform.base import PlatformSupport

__all__ = ["LinuxPlatform"]

_BY_ID = Path("/dev/serial/by-id")
_SYS_TTY = Path("/sys/class/tty")
#: Used when the device node cannot be inspected (it may not exist yet).
_DEFAULT_GROUP = "dialout"


class LinuxPlatform(PlatformSupport):
    """Port discovery and diagnostics for Linux."""

    name = "linux"

    def __init__(self, *, by_id: Path = _BY_ID, sys_tty: Path = _SYS_TTY) -> None:
        # Injectable so the tests can run this backend on any OS.
        self._by_id = by_id
        self._sys_tty = sys_tty

    def stable_path_candidates(self) -> Sequence[Path]:
        return (self._by_id,)

    def enrich_port(self, port: PortInfo) -> PortInfo:
        from dataclasses import replace

        return replace(
            port,
            stable_path=port.stable_path or self._stable_path(port.device),
            driver=port.driver or self._driver(port.device),
        )

    def _stable_path(self, device: str) -> str | None:
        """Find a /dev/serial/by-id symlink pointing at ``device``."""
        try:
            entries = sorted(self._by_id.iterdir())
        except OSError:
            return None
        target = Path(device).resolve()
        for entry in entries:
            try:
                if entry.resolve() == target:
                    return str(entry)
            except OSError:  # pragma: no cover - racing with a replug
                continue
        return None

    def _driver(self, device: str) -> str | None:
        """Read the kernel driver bound to the port from sysfs."""
        link = self._sys_tty / Path(device).name / "device" / "driver"
        try:
            return link.readlink().name
        except OSError:
            return None

    def device_group(self, port: str) -> str:
        """Group that owns the device node, or ``dialout`` if it can't be read."""
        try:
            stat = Path(port).stat()
            return grp.getgrgid(stat.st_gid).gr_name
        except (OSError, KeyError):
            return _DEFAULT_GROUP

    def permission_remedy(self, port: str) -> str:
        group = self.device_group(port)
        return (
            f"You are not a member of the group that owns {port}.\n"
            f"Add yourself to the '{group}' group and start a new login session:\n\n"
            f"    sudo usermod -aG {group} $USER\n\n"
            "Then log out and back in (or run 'newgrp "
            f"{group}' in this shell) and try again.\n"
            "Check the groups you are in with 'id -nG'."
        )

    def busy_remedy(self, port: str) -> str:
        return (
            f"{port} is open in another program.\n"
            "Find it with:\n\n"
            f"    sudo fuser -v {port}       # or: sudo lsof {port}\n\n"
            "Common culprits are minicom, screen, picocom, ModemManager and another "
            "Portside window.  ModemManager can be told to leave the port alone with a "
            'udev rule (ENV{ID_MM_DEVICE_IGNORE}="1").'
        )
