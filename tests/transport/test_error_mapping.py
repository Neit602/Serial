"""Driver errors become typed errors with a fix attached (F6).

Table-driven over synthetic exceptions, so every platform's wording is checked
on every CI runner — no real hardware and no real failure needed.
"""

from __future__ import annotations

import errno

import pytest
import serial

from portside.core.errors import (
    DeviceVanishedError,
    PermissionDeniedError,
    PortBusyError,
    PortNotFoundError,
    TransportError,
)
from portside.platform import platform_for
from portside.transport.errors import map_serial_exception

PLATFORMS = ["linux", "macos", "windows", "posix"]


def _oserror(err: int, message: str) -> OSError:
    return OSError(err, message)


def _windows_error(winerror: int, message: str) -> OSError:
    exc = OSError(0, message)
    exc.winerror = winerror  # type: ignore[attr-defined]
    return exc


@pytest.mark.parametrize("platform_name", PLATFORMS)
@pytest.mark.parametrize(
    ("exc", "expected"),
    [
        (_oserror(errno.EACCES, "Permission denied"), PermissionDeniedError),
        (_oserror(errno.EPERM, "Operation not permitted"), PermissionDeniedError),
        (_oserror(errno.EBUSY, "Device or resource busy"), PortBusyError),
        (_oserror(errno.EAGAIN, "Resource temporarily unavailable"), PortBusyError),
        (_oserror(errno.ENOENT, "No such file or directory"), PortNotFoundError),
        (_oserror(errno.ENODEV, "No such device"), DeviceVanishedError),
        (_oserror(errno.ENXIO, "Device not configured"), DeviceVanishedError),
        (_oserror(errno.EIO, "Input/output error"), DeviceVanishedError),
    ],
)
def test_posix_errnos_map_to_typed_errors(
    platform_name: str, exc: OSError, expected: type[TransportError]
) -> None:
    mapped = map_serial_exception(exc, "/dev/ttyUSB0", platform=platform_for(platform_name))
    assert isinstance(mapped, expected)
    assert mapped.port == "/dev/ttyUSB0"


@pytest.mark.parametrize(
    ("exc", "expected"),
    [
        (_windows_error(5, "Access is denied."), PermissionDeniedError),
        (_windows_error(2, "The system cannot find the file specified."), PortNotFoundError),
        (_windows_error(3, "The system cannot find the path specified."), PortNotFoundError),
        (_windows_error(32, "The process cannot access the file"), PortBusyError),
        (_windows_error(1167, "The device is not connected."), DeviceVanishedError),
    ],
)
def test_windows_error_codes_map_to_typed_errors(
    exc: OSError, expected: type[TransportError]
) -> None:
    mapped = map_serial_exception(exc, "COM3", platform=platform_for("windows"))
    assert isinstance(mapped, expected)


@pytest.mark.parametrize(
    ("message", "expected"),
    [
        ("could not open port /dev/ttyUSB0: [Errno 13] Permission denied", PermissionDeniedError),
        ("could not open port COM3: Access is denied.", PermissionDeniedError),
        (
            "Cannot configure port: device reports readiness to read but returned no data",
            DeviceVanishedError,
        ),
        (
            "could not open port /dev/ttyUSB9: [Errno 2] No such file or directory",
            PortNotFoundError,
        ),
        ("Sharing violation on COM4", PortBusyError),
    ],
)
def test_pyserial_message_text_is_understood(message: str, expected: type[TransportError]) -> None:
    """pySerial often loses the errno, leaving only prose to go on."""
    mapped = map_serial_exception(
        serial.SerialException(message), "port", platform=platform_for("linux")
    )
    assert isinstance(mapped, expected)


def test_chained_exceptions_are_inspected() -> None:
    """pySerial wraps the OSError; the errno must still be found."""
    cause = _oserror(errno.EACCES, "Permission denied")
    wrapper = serial.SerialException("could not open port")
    wrapper.__cause__ = cause
    mapped = map_serial_exception(wrapper, "/dev/ttyUSB0", platform=platform_for("linux"))
    assert isinstance(mapped, PermissionDeniedError)


def test_missing_device_while_open_means_it_vanished() -> None:
    """ENOENT before open means "wrong name"; during I/O it means "unplugged"."""
    exc = _oserror(errno.ENOENT, "No such file or directory")
    closed = map_serial_exception(exc, "/dev/ttyUSB0", platform=platform_for("linux"))
    opened = map_serial_exception(
        exc, "/dev/ttyUSB0", platform=platform_for("linux"), while_open=True
    )
    assert isinstance(closed, PortNotFoundError)
    assert isinstance(opened, DeviceVanishedError)


def test_unrecognised_failures_stay_typed_but_honest() -> None:
    mapped = map_serial_exception(
        RuntimeError("something odd"), "COM1", platform=platform_for("windows")
    )
    assert type(mapped) is TransportError
    assert "something odd" in str(mapped)


@pytest.mark.parametrize("platform_name", PLATFORMS)
def test_every_mapped_error_carries_a_remedy(platform_name: str) -> None:
    """The UI must always have something actionable to show (F6)."""
    support = platform_for(platform_name)
    for exc in (
        _oserror(errno.EACCES, "Permission denied"),
        _oserror(errno.EBUSY, "Device or resource busy"),
        _oserror(errno.ENOENT, "No such file or directory"),
        _oserror(errno.ENODEV, "No such device"),
    ):
        mapped = map_serial_exception(exc, "/dev/ttyUSB0", platform=support)
        assert mapped.remedy, f"{platform_name} has no remedy for {exc}"
        assert mapped.display() != str(mapped)
