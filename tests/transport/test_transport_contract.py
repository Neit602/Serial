"""One contract, every transport.

Both concrete transports are held to the same behaviour, so the loopback used in
the rest of the suite cannot drift from the real pySerial one.
"""

from __future__ import annotations

import time
from collections.abc import Callable, Iterator

import pytest

from portside.core.errors import TransportClosedError
from portside.core.settings import FlowControl, Parity, SerialSettings
from portside.transport.base import ModemLines, Transport
from portside.transport.loopback import LoopbackTransport
from portside.transport.serial_transport import PySerialTransport

TransportFactory = Callable[[], Transport]


def _loopback() -> Transport:
    return LoopbackTransport()


def _pyserial_loop() -> Transport:
    return PySerialTransport("loop://", SerialSettings(baudrate=115200))


@pytest.fixture(params=[_loopback, _pyserial_loop], ids=["loopback", "pyserial-loop"])
def transport(request: pytest.FixtureRequest) -> Iterator[Transport]:
    made: Transport = request.param()
    try:
        yield made
    finally:
        made.close()


def _read_until(transport: Transport, count: int, timeout: float = 2.0) -> bytes:
    """Reads can arrive in pieces; gather `count` bytes or give up."""
    data = bytearray()
    deadline = time.monotonic() + timeout
    while len(data) < count and time.monotonic() < deadline:
        data.extend(transport.read(4096, 0.1))
    return bytes(data)


def test_starts_closed_and_opens(transport: Transport) -> None:
    assert transport.is_open is False
    transport.open()
    assert transport.is_open is True


def test_open_is_idempotent(transport: Transport) -> None:
    transport.open()
    transport.open()
    assert transport.is_open is True


def test_close_is_idempotent_and_never_raises(transport: Transport) -> None:
    transport.open()
    transport.close()
    transport.close()
    assert transport.is_open is False


def test_write_then_read_round_trips(transport: Transport) -> None:
    transport.open()
    assert transport.write(b"hello portside") == 14
    assert _read_until(transport, 14) == b"hello portside"


def test_read_timeout_returns_empty_not_an_error(transport: Transport) -> None:
    transport.open()
    started = time.monotonic()
    assert transport.read(16, 0.05) == b""
    assert time.monotonic() - started < 1.0


def test_io_on_a_closed_transport_raises_typed_error(transport: Transport) -> None:
    with pytest.raises(TransportClosedError):
        transport.read(16, 0.01)
    with pytest.raises(TransportClosedError):
        transport.write(b"x")


def test_settings_can_be_applied_before_and_after_open(transport: Transport) -> None:
    settings = SerialSettings(baudrate=9600, data_bits=7, parity=Parity.EVEN)
    transport.apply_settings(settings)
    assert transport.settings == settings
    transport.open()
    changed = settings.evolve(baudrate=57600, flow_control=FlowControl.RTSCTS)
    transport.apply_settings(changed)
    assert transport.settings == changed


def test_modem_lines_report_bool_or_unknown(transport: Transport) -> None:
    transport.open()
    lines = transport.get_modem_lines()
    assert isinstance(lines, ModemLines)
    for value in (lines.cts, lines.dsr, lines.dcd, lines.ri):
        assert value is None or isinstance(value, bool)


def test_dtr_and_rts_report_whether_the_driver_has_them(transport: Transport) -> None:
    """F4: a driver without DTR/RTS reports it; it never kills the session."""
    transport.open()
    assert isinstance(transport.set_dtr(True), bool)
    assert isinstance(transport.set_rts(False), bool)


def test_context_manager_opens_and_closes(transport: Transport) -> None:
    with transport as opened:
        assert opened.is_open is True
    assert transport.is_open is False


def test_cancel_read_unblocks_a_waiting_read(transport: Transport) -> None:
    import threading

    transport.open()
    result: list[bytes] = []

    def reader() -> None:
        result.append(transport.read(16, 5.0))

    thread = threading.Thread(target=reader)
    thread.start()
    time.sleep(0.1)
    transport.cancel_read()
    thread.join(timeout=5)
    assert not thread.is_alive(), "cancel_read did not unblock the read"


def test_repr_mentions_name_and_state(transport: Transport) -> None:
    assert transport.name in repr(transport)
    assert "closed" in repr(transport)
