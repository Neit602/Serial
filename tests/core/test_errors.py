"""The typed error model UIs display (F6)."""

from __future__ import annotations

import pytest

from portside.core.errors import (
    DeviceVanishedError,
    InvalidSettingsError,
    PermissionDeniedError,
    PortBusyError,
    PortNotFoundError,
    PortsideError,
    SessionError,
    TransportClosedError,
    TransportError,
)

TRANSPORT_ERRORS = [
    PermissionDeniedError,
    PortBusyError,
    PortNotFoundError,
    DeviceVanishedError,
    TransportClosedError,
]


@pytest.mark.parametrize("kind", [*TRANSPORT_ERRORS, InvalidSettingsError, SessionError])
def test_every_error_is_a_portside_error(kind: type[PortsideError]) -> None:
    assert issubclass(kind, PortsideError)


@pytest.mark.parametrize("kind", TRANSPORT_ERRORS)
def test_transport_errors_share_a_base(kind: type[TransportError]) -> None:
    assert issubclass(kind, TransportError)


def test_error_carries_port_and_remedy() -> None:
    error = PermissionDeniedError("permission denied", port="/dev/ttyUSB0", remedy="do the thing")
    assert error.port == "/dev/ttyUSB0"
    assert str(error) == "/dev/ttyUSB0: permission denied"
    assert error.display() == "/dev/ttyUSB0: permission denied\n\ndo the thing"


def test_error_without_port_or_remedy_still_reads_well() -> None:
    error = PortsideError("something broke")
    assert str(error) == "something broke"
    assert error.display() == "something broke"
