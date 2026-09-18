"""macOS platform backend.

The important rule here is the PRD's: list the ``/dev/cu.*`` callout devices and
hide the matching ``/dev/tty.*`` dial-in devices, which block on open until DCD
is asserted and so appear to hang.
"""

from __future__ import annotations

from collections.abc import Iterable

from portside.core.ports import PortInfo
from portside.platform.base import PlatformSupport

__all__ = ["MacOSPlatform"]

_DIALIN_PREFIX = "/dev/tty."


class MacOSPlatform(PlatformSupport):
    """Port discovery and diagnostics for macOS."""

    name = "macos"

    def filter_ports(self, ports: Iterable[PortInfo]) -> list[PortInfo]:
        """Keep callout devices; drop the dial-in twins (they block on DCD)."""
        return [port for port in ports if not port.device.startswith(_DIALIN_PREFIX)]

    def permission_remedy(self, port: str) -> str:
        return (
            f"macOS refused access to {port}.\n"
            "This is unusual on macOS; the likely causes are:\n"
            "  * another program (screen, minicom, an IDE's serial monitor) holds the port;\n"
            "  * the adapter's vendor driver and Apple's built-in driver both claim the "
            "device — remove the vendor kext/driver extension and re-plug;\n"
            "  * the terminal app lacks permission to access a USB accessory: check "
            "System Settings → Privacy & Security."
        )

    def busy_remedy(self, port: str) -> str:
        return (
            f"{port} is open in another program.\n"
            "Find it with:\n\n"
            f"    lsof {port}\n\n"
            "Quit that program (or close the other Portside session) and try again.\n"
            "If you opened the matching /dev/tty.* device by mistake, use the /dev/cu.* "
            "name instead — Portside only lists /dev/cu.*."
        )
