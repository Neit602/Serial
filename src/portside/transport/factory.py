"""Building transports from a port name or a device identity.

Keeping this out of :mod:`portside.core.session` lets the session stay
transport-agnostic: it is handed a callable and never learns what a COM port is.
"""

from __future__ import annotations

from collections.abc import Callable

from portside.core.errors import PortNotFoundError
from portside.core.identity import DeviceIdentity
from portside.core.settings import SerialSettings
from portside.platform import PlatformSupport, get_platform
from portside.transport.base import Transport
from portside.transport.discovery import ComPortLister, find_port
from portside.transport.serial_transport import PySerialTransport

__all__ = ["identity_reopener", "open_port", "resolve_port"]


def resolve_port(
    name: str,
    *,
    lister: ComPortLister | None = None,
    platform: PlatformSupport | None = None,
) -> tuple[str, DeviceIdentity | None]:
    """Resolve a user-supplied port name to a device name and its identity.

    Accepts a device name (``COM3``, ``/dev/ttyUSB0``), a Linux stable path
    (``/dev/serial/by-id/...``) or a pySerial URL (``loop://``), which is passed
    through untouched and has no identity.

    Enumeration is advisory, not a gate: ports that ``list_ports`` does not
    report — Bluetooth RFCOMM nodes, PTYs, some on-board UARTs — are handed to
    the driver anyway, which then produces the real (and typed) error if the
    port truly is not there.  Only ports found by discovery carry an identity,
    so only they can be auto-reconnected by device (F2, F5).
    """
    if "://" in name:
        return name, None
    port = find_port(name, lister=lister, platform=platform)
    if port is None:
        return name, None
    return port.device, DeviceIdentity.from_port(port)


def open_port(
    name: str,
    settings: SerialSettings | None = None,
    *,
    platform: PlatformSupport | None = None,
) -> PySerialTransport:
    """Create (but do not open) a transport for ``name``."""
    return PySerialTransport(name, settings, platform=platform)


def identity_reopener(
    identity: DeviceIdentity,
    settings: SerialSettings,
    *,
    lister: ComPortLister | None = None,
    platform: PlatformSupport | None = None,
    factory: Callable[[str, SerialSettings], Transport] | None = None,
) -> Callable[[], Transport]:
    """A reopener that finds the device again *by identity*, not by port name.

    This is what makes auto-reconnect survive the adapter coming back as
    ``/dev/ttyUSB1`` or ``COM9`` (PRD F2 + F5).
    """
    support = platform or get_platform()
    make = factory or (lambda device, cfg: PySerialTransport(device, cfg, platform=support))

    def reopen() -> Transport:
        port = find_port(identity, lister=lister, platform=support)
        if port is None:
            raise PortNotFoundError(
                f"device {identity.describe()} is not present",
                remedy="Re-plug the adapter; Portside is waiting for it to come back.",
            )
        return make(port.device, settings)

    return reopen
