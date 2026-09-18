"""Serial port discovery (PRD F1, F2).

``serial.tools.list_ports`` gives the cross-platform base (device, description,
VID, PID, serial number); the platform backend then filters the list (macOS
``/dev/cu.*`` only) and enriches it (Linux stable path and bound driver).
"""

from __future__ import annotations

from collections.abc import Callable, Iterable, Sequence
from typing import Any

from portside.core.identity import DeviceIdentity
from portside.core.ports import PortInfo
from portside.platform import PlatformSupport, get_platform
from portside.transport.chips import chip_family

__all__ = ["ComPortLister", "find_port", "list_ports", "port_from_comport"]

#: Signature of ``serial.tools.list_ports.comports``; injected in tests.
ComPortLister = Callable[[], Iterable[Any]]


def _default_lister() -> Iterable[Any]:
    from serial.tools import list_ports as _list_ports

    entries: Iterable[Any] = _list_ports.comports(include_links=False)
    return entries


def _clean(value: object) -> str | None:
    """pySerial reports unknown fields as ``None`` or the string ``"n/a"``."""
    if value is None:
        return None
    text = str(value).strip()
    if not text or text.lower() in {"n/a", "none", "unknown"}:
        return None
    return text


def port_from_comport(entry: Any) -> PortInfo:
    """Convert one ``serial.tools.list_ports_common.ListPortInfo``."""
    vid = entry.vid if isinstance(getattr(entry, "vid", None), int) else None
    pid = entry.pid if isinstance(getattr(entry, "pid", None), int) else None
    extra: dict[str, str] = {}
    location = _clean(getattr(entry, "location", None))
    if location:
        extra["location"] = location
    interface = _clean(getattr(entry, "interface", None))
    if interface:
        extra["interface"] = interface
    hwid = _clean(getattr(entry, "hwid", None))
    if hwid:
        extra["hwid"] = hwid
    return PortInfo(
        device=str(entry.device),
        description=_clean(getattr(entry, "description", None)),
        vid=vid,
        pid=pid,
        serial_number=_clean(getattr(entry, "serial_number", None)),
        manufacturer=_clean(getattr(entry, "manufacturer", None)),
        product=_clean(getattr(entry, "product", None)),
        chip_family=chip_family(vid, pid),
        extra=extra,
    )


def list_ports(
    *,
    lister: ComPortLister | None = None,
    platform: PlatformSupport | None = None,
) -> list[PortInfo]:
    """All serial ports the user may connect to, on any OS.

    The platform backend decides what to hide (macOS dial-in devices) and what
    to add (Linux ``/dev/serial/by-id`` path and kernel driver).
    """
    support = platform or get_platform()
    raw = (lister or _default_lister)()
    ports = [port_from_comport(entry) for entry in raw]
    prepared = support.prepare_ports(ports)
    return sorted(prepared, key=support.sort_key)


def find_port(
    name_or_identity: str | DeviceIdentity,
    *,
    lister: ComPortLister | None = None,
    platform: PlatformSupport | None = None,
) -> PortInfo | None:
    """Find a port by device name, stable path, or device identity (F2).

    Matching by :class:`DeviceIdentity` is what makes auto-reconnect survive a
    port rename: the adapter that comes back as ``/dev/ttyUSB1`` or ``COM9`` is
    still recognised as the same device.
    """
    ports = list_ports(lister=lister, platform=platform)
    if isinstance(name_or_identity, DeviceIdentity):
        for port in ports:
            if name_or_identity.matches(port):
                return port
        return None
    for port in ports:
        if port.device == name_or_identity or port.stable_path == name_or_identity:
            return port
    return None


def identities(ports: Sequence[PortInfo]) -> list[DeviceIdentity]:
    """Device identities for ``ports``, in the same order."""
    return [DeviceIdentity.from_port(port) for port in ports]
