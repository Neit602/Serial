"""The interface every platform backend implements.

One class per operating system, all with the same typed surface, so the rest of
Portside never branches on ``sys.platform``.
"""

from __future__ import annotations

import re
from abc import ABC, abstractmethod
from collections.abc import Iterable, Sequence
from pathlib import Path

from portside.core.ports import PortInfo

__all__ = ["PlatformSupport"]


class PlatformSupport(ABC):
    """Platform-specific behaviour: port filtering, enrichment and remedies."""

    #: Short identifier used in messages and tests ("linux", "macos", "windows").
    name: str = "generic"

    # -- discovery ---------------------------------------------------------

    def filter_ports(self, ports: Iterable[PortInfo]) -> list[PortInfo]:
        """Drop ports the user should never be offered.  Default: keep all."""
        return list(ports)

    def enrich_port(self, port: PortInfo) -> PortInfo:
        """Add platform-only details (driver in use, stable path).  Default: none."""
        return port

    def prepare_ports(self, ports: Iterable[PortInfo]) -> list[PortInfo]:
        """Filter then enrich, the order discovery always wants."""
        return [self.enrich_port(port) for port in self.filter_ports(ports)]

    def sort_key(self, port: PortInfo) -> tuple[int, tuple[object, ...]]:
        """USB adapters first, then the rest; each in natural order.

        Natural order keeps COM9 before COM10 and ttyS2 before ttyS10, which
        plain string sorting gets wrong.
        """
        chunks: tuple[object, ...] = tuple(
            (1, int(part)) if part.isdigit() else (0, part)
            for part in re.split(r"(\d+)", port.device)
            if part
        )
        return (0 if port.vid_pid else 1, chunks)

    # -- diagnostics (F6) --------------------------------------------------

    @abstractmethod
    def permission_remedy(self, port: str) -> str:
        """What the user must do about "permission denied" on this platform."""

    @abstractmethod
    def busy_remedy(self, port: str) -> str:
        """What the user must do about a port that another program holds open."""

    def not_found_remedy(self, port: str) -> str:
        """What the user must do when the port name does not exist."""
        return (
            f"No serial port named {port!r} was found.\n"
            "Run 'portside --list' to see the ports that are currently present, and check "
            "that the adapter is plugged in and its driver is loaded."
        )

    def vanished_remedy(self, port: str) -> str:
        """What the user must do when a device disappears mid-session."""
        return (
            f"The device behind {port} disappeared (unplugged, reset, or the driver dropped it).\n"
            "Re-plug it and connect again, or use --reconnect to have Portside wait for it."
        )

    # -- paths -------------------------------------------------------------

    def config_dir(self) -> Path:
        """OS-correct configuration directory (profiles land here in M2)."""
        from platformdirs import user_config_path

        return user_config_path("Portside", appauthor=False, roaming=True)

    def log_dir(self) -> Path:
        """OS-correct directory for session logs (used from M2)."""
        from platformdirs import user_log_path

        return user_log_path("Portside", appauthor=False)

    # -- console -----------------------------------------------------------

    def stable_path_candidates(self) -> Sequence[Path]:
        """Directories holding stable symlinks to ports.  Empty except on Linux."""
        return ()
