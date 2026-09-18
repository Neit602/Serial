"""Integration tests over a real pair of virtual serial ports.

These use socat on Linux and macOS and skip cleanly on Windows (com0com is not
installable unattended in CI) and wherever socat is absent, which is exactly
what the M1 exit criterion asks for.
"""

from __future__ import annotations

import asyncio
import time

import pytest

from portside.core.errors import DeviceVanishedError, PortsideError
from portside.core.session import ReconnectPolicy, Session, SessionState
from portside.core.settings import Parity, SerialSettings
from portside.transport.serial_transport import PySerialTransport

from ..conftest import SocatPair, requires_socat

pytestmark = [pytest.mark.socat, requires_socat]


async def _wait_for_state(session: Session, state: SessionState, timeout: float = 10.0) -> None:
    """Poll until the session reaches `state`, or fail the test."""
    async with asyncio.timeout(timeout):
        while session.state is not state:
            await asyncio.sleep(0.05)


def _read_until(transport: PySerialTransport, count: int, timeout: float = 5.0) -> bytes:
    data = bytearray()
    deadline = time.monotonic() + timeout
    while len(data) < count and time.monotonic() < deadline:
        data.extend(transport.read(4096, 0.1))
    return bytes(data)


def test_a_real_port_opens_and_round_trips(socat_pair: SocatPair) -> None:
    settings = SerialSettings(baudrate=115200)
    left = PySerialTransport(socat_pair.left, settings)
    right = PySerialTransport(socat_pair.right, settings)
    with left, right:
        left.write(b"hello over the wire\n")
        assert _read_until(right, 20) == b"hello over the wire\n"
        right.write(b"and back\n")
        assert _read_until(left, 9) == b"and back\n"


def test_settings_apply_to_a_real_port(socat_pair: SocatPair) -> None:
    transport = PySerialTransport(socat_pair.left, SerialSettings(baudrate=9600))
    with transport:
        transport.apply_settings(SerialSettings(baudrate=115200, data_bits=7, parity=Parity.EVEN))
        assert transport.settings.baudrate == 115200
        transport.write(b"x")


def test_modem_lines_and_control_signals_do_not_explode(socat_pair: SocatPair) -> None:
    """PTYs support little of this; the point is that it degrades, not crashes."""
    with PySerialTransport(socat_pair.left) as transport:
        transport.set_dtr(True)
        transport.set_rts(True)
        lines = transport.get_modem_lines()
        assert lines.describe()


def test_full_duplex_bulk_transfer_loses_nothing(socat_pair: SocatPair) -> None:
    payload = bytes(range(256)) * 64  # 16 KiB
    settings = SerialSettings(baudrate=115200)
    with (
        PySerialTransport(socat_pair.left, settings) as left,
        PySerialTransport(socat_pair.right, settings) as right,
    ):
        left.write(payload)
        left.flush_output()
        assert _read_until(right, len(payload), timeout=15) == payload


async def test_session_over_a_real_port(socat_pair: SocatPair) -> None:
    device = PySerialTransport(socat_pair.right, SerialSettings(baudrate=115200))
    with device:
        async with Session(
            PySerialTransport(socat_pair.left, SerialSettings(baudrate=115200))
        ) as session:
            await session.send(b"ping\n")
            assert _read_until(device, 5) == b"ping\n"
            device.write(b"pong\n")
            received = bytearray()
            async with asyncio.timeout(5):
                while len(received) < 5:
                    chunk = await session.receive()
                    if chunk is None:
                        break
                    received.extend(chunk)
            assert bytes(received) == b"pong\n"


async def test_unplugging_mid_session_is_reported_not_crashed(socat_pair: SocatPair) -> None:
    """NFR stability: killing socat is as close to a yanked cable as CI gets."""
    session = Session(PySerialTransport(socat_pair.left, SerialSettings(baudrate=115200)))
    await session.open()
    try:
        socat_pair.kill()
        await _wait_for_state(session, SessionState.DISCONNECTED)
    finally:
        await session.close()
    assert session.state is SessionState.CLOSED


async def test_auto_reconnect_waits_for_the_device_to_return(socat_pair: SocatPair) -> None:
    """F5 over real ports: the session survives the device going away."""
    settings = SerialSettings(baudrate=115200)
    port_name = socat_pair.left
    attempts: list[int] = []

    def reopen() -> PySerialTransport:
        attempts.append(1)
        raise DeviceVanishedError("still gone", port=port_name)

    session = Session(
        PySerialTransport(port_name, settings),
        settings=settings,
        reconnect=ReconnectPolicy(enabled=True, initial_delay=0.05, max_delay=0.1),
        reopen=reopen,
    )
    await session.open()
    try:
        socat_pair.kill()
        await _wait_for_state(session, SessionState.RECONNECTING)
        async with asyncio.timeout(10):
            while not attempts:
                await asyncio.sleep(0.05)
    finally:
        await session.close()
    assert session.state is SessionState.CLOSED


def test_opening_a_missing_port_gives_a_typed_error_with_a_remedy() -> None:
    """Runs everywhere, socat or not: a nonexistent path must be diagnosed."""
    transport = PySerialTransport("/dev/portside-does-not-exist")
    with pytest.raises(PortsideError) as caught:
        transport.open()
    assert caught.value.remedy
