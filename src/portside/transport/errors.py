"""Translate driver errors into Portside's typed errors (PRD F6).

pySerial reports failures as :class:`serial.SerialException` (often wrapping an
``OSError``) with platform-dependent wording.  This module is the single place
that understands that wording, so that everything above it only ever sees
:class:`PermissionDeniedError`, :class:`PortBusyError`,
:class:`PortNotFoundError` or :class:`DeviceVanishedError` — each already
carrying the platform's fix.
"""

from __future__ import annotations

import errno as _errno

from portside.core.errors import (
    DeviceVanishedError,
    PermissionDeniedError,
    PortBusyError,
    PortNotFoundError,
    TransportError,
)
from portside.platform import PlatformSupport, get_platform

__all__ = ["map_serial_exception"]

# Windows system error codes, named so the table below reads.
_WIN_ACCESS_DENIED = 5
_WIN_FILE_NOT_FOUND = 2
_WIN_PATH_NOT_FOUND = 3
_WIN_SHARING_VIOLATION = 32
_WIN_DEVICE_NOT_CONNECTED = 1167

_PERMISSION_TEXT = ("permission denied", "access is denied", "access denied")
_BUSY_TEXT = (
    "resource busy",
    "device or resource busy",
    "resource temporarily unavailable",
    "sharing violation",
    "in use by another",
    "already open",
)
_NOT_FOUND_TEXT = (
    "no such file or directory",
    "could not find",
    "file not found",
    "cannot find the path",
    "port not found",
)
_VANISHED_TEXT = (
    "device not configured",
    "no such device",
    "device disconnected",
    "device reports readiness to read but returned no data",
    "input/output error",
    "the device does not recognize the command",
    "the handle is invalid",
)


def _errnos(exc: BaseException) -> tuple[int | None, int | None]:
    """Extract ``errno`` and Windows ``winerror`` from an exception chain."""
    seen: list[BaseException] = []
    current: BaseException | None = exc
    while current is not None and current not in seen:
        seen.append(current)
        err = getattr(current, "errno", None)
        win = getattr(current, "winerror", None)
        if err is not None or win is not None:
            return err, win
        current = current.__cause__ or current.__context__
    return None, None


def map_serial_exception(
    exc: BaseException,
    port: str,
    *,
    platform: PlatformSupport | None = None,
    while_open: bool = False,
) -> TransportError:
    """Map a driver exception to a typed Portside error.

    Args:
        exc: the original exception from pySerial or the OS.
        port: the port name, used in the message and the remedy.
        platform: platform backend supplying remedies (defaults to this OS).
        while_open: True if the failure happened during I/O on an already open
            port, where "no such device" means the device was unplugged rather
            than never present.
    """
    support = platform or get_platform()
    text = str(exc).lower()
    err, win = _errnos(exc)

    def permission() -> TransportError:
        return PermissionDeniedError(
            f"permission denied opening {port}",
            port=port,
            remedy=support.permission_remedy(port),
        )

    def busy() -> TransportError:
        return PortBusyError(f"{port} is busy", port=port, remedy=support.busy_remedy(port))

    def missing() -> TransportError:
        return PortNotFoundError(
            f"no such port: {port}", port=port, remedy=support.not_found_remedy(port)
        )

    def vanished() -> TransportError:
        return DeviceVanishedError(
            f"{port} disappeared", port=port, remedy=support.vanished_remedy(port)
        )

    if win is not None:
        if win == _WIN_ACCESS_DENIED:
            return busy() if while_open else permission()
        if win in {_WIN_FILE_NOT_FOUND, _WIN_PATH_NOT_FOUND}:
            return vanished() if while_open else missing()
        if win == _WIN_SHARING_VIOLATION:
            return busy()
        if win == _WIN_DEVICE_NOT_CONNECTED:
            return vanished()

    if err is not None:
        if err == _errno.EACCES or err == _errno.EPERM:
            return permission()
        if err in {_errno.EBUSY, _errno.EAGAIN, _errno.ETXTBSY}:
            return busy()
        if err == _errno.ENOENT:
            return vanished() if while_open else missing()
        if err in {_errno.ENODEV, _errno.ENXIO, _errno.EIO, _errno.EBADF, _errno.ESHUTDOWN}:
            return vanished()

    if any(token in text for token in _PERMISSION_TEXT):
        return busy() if while_open else permission()
    if any(token in text for token in _BUSY_TEXT):
        return busy()
    if any(token in text for token in _VANISHED_TEXT):
        return vanished()
    if any(token in text for token in _NOT_FOUND_TEXT):
        return vanished() if while_open else missing()

    return TransportError(f"{type(exc).__name__}: {exc}", port=port)
