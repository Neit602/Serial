"""The loopback transport's own behaviour, including fault injection."""

from __future__ import annotations

import pytest

from portside.core.errors import DeviceVanishedError, PermissionDeniedError
from portside.transport.base import ModemLines
from portside.transport.loopback import LoopbackTransport


def test_echo_can_be_switched_off() -> None:
    transport = LoopbackTransport(echo=False)
    transport.open()
    transport.write(b"silent")
    assert transport.read(16, 0.01) == b""
    assert bytes(transport.written) == b"silent"


def test_feed_makes_data_readable() -> None:
    transport = LoopbackTransport(echo=False)
    transport.open()
    transport.feed(b"from the device")
    assert transport.read(64, 0.1) == b"from the device"


def test_open_can_be_made_to_fail() -> None:
    error = PermissionDeniedError("nope", port="loopback://", remedy="join a group")
    transport = LoopbackTransport(fail_open_with=error)
    with pytest.raises(PermissionDeniedError) as caught:
        transport.open()
    assert caught.value.remedy == "join a group"
    assert transport.is_open is False


def test_device_can_be_made_to_vanish() -> None:
    transport = LoopbackTransport(vanish_after=4, echo=False)
    transport.open()
    with pytest.raises(DeviceVanishedError):
        transport.write(b"1234")
    assert transport.is_open is False


def test_breaks_and_control_lines_are_recorded() -> None:
    transport = LoopbackTransport(modem_lines=ModemLines(cts=True, dsr=None, dcd=True, ri=False))
    transport.open()
    transport.send_break(0.25)
    assert transport.set_dtr(True) is True
    assert transport.set_rts(False) is True
    assert list(transport.breaks) == [0.25]
    assert (transport.dtr, transport.rts) == (True, False)
    assert transport.get_modem_lines().describe() == "CTS -- DCD ri"


def test_open_count_tracks_reconnects() -> None:
    transport = LoopbackTransport()
    transport.open()
    transport.close()
    transport.open()
    assert transport.open_count == 2


def test_a_transport_without_control_lines_reports_it() -> None:
    """PTYs and minimal bridges have no DTR/RTS; that is a limitation, not an error."""
    transport = LoopbackTransport(supports_control_lines=False)
    transport.open()
    assert transport.set_dtr(True) is False
    assert transport.set_rts(True) is False
    assert transport.dtr is None
