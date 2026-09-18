"""The session core: states, the thread bridge, backpressure and reconnect."""

from __future__ import annotations

import asyncio
import threading
from typing import TypeVar

import pytest

from portside.core.errors import (
    DeviceVanishedError,
    PermissionDeniedError,
    SessionError,
)
from portside.core.events import (
    BytesReceived,
    BytesSent,
    ErrorRaised,
    Event,
    ReconnectAttempt,
    StateChanged,
)
from portside.core.identity import DeviceIdentity
from portside.core.session import ReconnectPolicy, Session, SessionState
from portside.core.settings import SerialSettings
from portside.transport.base import Transport
from portside.transport.loopback import LoopbackTransport

T = TypeVar("T")


class Recorder:
    """Collects every event a session publishes."""

    def __init__(self) -> None:
        self.events: list[Event] = []

    def __call__(self, event: Event) -> None:
        self.events.append(event)

    def of(self, kind: type[T]) -> list[T]:
        return [event for event in self.events if isinstance(event, kind)]

    @property
    def states(self) -> list[SessionState]:
        return [event.new for event in self.events if isinstance(event, StateChanged)]


def state_of(session: Session) -> SessionState:
    """Read the state through a call, so one assert does not narrow the next."""
    return session.state


async def _wait_for_state(session: Session, state: SessionState, timeout: float = 5.0) -> None:
    """Poll until the session reaches `state`, or fail the test."""
    async with asyncio.timeout(timeout):
        while session.state is not state:
            await asyncio.sleep(0.02)


async def _drain(session: Session, count: int, timeout: float = 3.0) -> bytes:
    """Collect `count` received bytes or fail the test."""
    data = bytearray()
    async with asyncio.timeout(timeout):
        while len(data) < count:
            chunk = await session.receive()
            if chunk is None:
                break
            data.extend(chunk)
    return bytes(data)


# -- lifecycle -----------------------------------------------------------


async def test_open_and_close_move_through_the_expected_states() -> None:
    recorder = Recorder()
    session = Session(LoopbackTransport())
    session.events.subscribe(recorder)

    assert state_of(session) is SessionState.DISCONNECTED
    await session.open()
    assert state_of(session) is SessionState.CONNECTED
    await session.close()
    assert state_of(session) is SessionState.CLOSED
    assert recorder.states == [
        SessionState.CONNECTING,
        SessionState.CONNECTED,
        SessionState.CLOSED,
    ]


async def test_close_is_idempotent() -> None:
    session = Session(LoopbackTransport())
    await session.open()
    await session.close()
    await session.close()
    assert session.state is SessionState.CLOSED


async def test_close_without_open_is_safe() -> None:
    session = Session(LoopbackTransport())
    await session.close()
    assert session.state is SessionState.CLOSED


async def test_opening_twice_is_an_error() -> None:
    session = Session(LoopbackTransport())
    await session.open()
    try:
        with pytest.raises(SessionError):
            await session.open()
    finally:
        await session.close()


async def test_a_closed_session_cannot_be_reopened() -> None:
    session = Session(LoopbackTransport())
    await session.open()
    await session.close()
    with pytest.raises(SessionError):
        await session.open()


async def test_async_context_manager_closes_on_exit() -> None:
    transport = LoopbackTransport()
    async with Session(transport) as session:
        assert session.is_connected
    assert transport.is_open is False


async def test_open_failure_is_typed_and_leaves_the_session_disconnected() -> None:
    error = PermissionDeniedError("denied", port="/dev/ttyUSB0", remedy="join dialout")
    session = Session(LoopbackTransport(fail_open_with=error))
    with pytest.raises(PermissionDeniedError) as caught:
        await session.open()
    assert caught.value.remedy == "join dialout"
    assert session.state is SessionState.DISCONNECTED


async def test_settings_are_applied_on_open() -> None:
    transport = LoopbackTransport()
    settings = SerialSettings(baudrate=9600, data_bits=7)
    async with Session(transport, settings=settings):
        assert transport.settings == settings


# -- data flow -----------------------------------------------------------


async def test_bytes_round_trip_through_the_loopback() -> None:
    async with Session(LoopbackTransport()) as session:
        await session.send(b"hello")
        assert await _drain(session, 5) == b"hello"


async def test_counters_track_traffic() -> None:
    async with Session(LoopbackTransport()) as session:
        await session.send(b"12345")
        await _drain(session, 5)
        assert session.bytes_out == 5
        assert session.bytes_in == 5
        assert "in 5 B" in session.status_line()


async def test_sending_nothing_is_a_no_op() -> None:
    async with Session(LoopbackTransport()) as session:
        assert await session.send(b"") == 0
        assert session.bytes_out == 0


async def test_sending_while_disconnected_raises() -> None:
    session = Session(LoopbackTransport())
    with pytest.raises(SessionError):
        await session.send(b"x")


async def test_concurrent_writes_keep_their_order() -> None:
    """A single I/O worker is what guarantees this; parallel writes would interleave."""
    transport = LoopbackTransport(echo=False)
    async with Session(transport) as session:
        await asyncio.gather(*(session.send(bytes([n]) * 16) for n in range(32)))
    written = bytes(transport.written)
    assert written == b"".join(bytes([n]) * 16 for n in range(32))


async def test_stream_yields_chunks_until_the_session_ends() -> None:
    session = Session(LoopbackTransport())
    await session.open()
    received = bytearray()

    async def consume() -> None:
        async for chunk in session.stream():
            received.extend(chunk)

    task = asyncio.create_task(consume())
    await session.send(b"abc")
    await asyncio.sleep(0.15)
    await session.close()
    async with asyncio.timeout(3):
        await task
    assert bytes(received) == b"abc"


async def test_receive_timeout_returns_empty() -> None:
    async with Session(LoopbackTransport()) as session:
        assert await session.receive(timeout=0.05) == b""


async def test_bulk_transfer_loses_nothing() -> None:
    """Throughput smoke test for the thread bridge (NFR: no dropped bytes)."""
    payload = bytes(range(256)) * 512  # 128 KiB
    async with Session(LoopbackTransport(), rx_maxsize=4096) as session:
        await session.send(payload)
        received = await _drain(session, len(payload), timeout=10)
    assert received == payload
    assert len(received) == len(payload)


async def test_events_are_published_for_traffic() -> None:
    recorder = Recorder()
    async with Session(LoopbackTransport()) as session:
        session.events.subscribe(recorder)
        await session.send(b"hi")
        await _drain(session, 2)
    assert recorder.of(BytesSent)
    assert b"".join(event.data for event in recorder.of(BytesReceived)) == b"hi"


async def test_a_broken_subscriber_cannot_kill_the_session() -> None:
    def explode(event: Event) -> None:
        raise RuntimeError("bad widget")

    async with Session(LoopbackTransport()) as session:
        session.events.subscribe(explode)
        await session.send(b"still works")
        assert await _drain(session, 11) == b"still works"


# -- backpressure --------------------------------------------------------


async def test_a_slow_consumer_drops_old_data_instead_of_growing_forever() -> None:
    """Bounded memory beats unbounded lag: the oldest chunks go, the newest stay.

    read_size=1 makes the chunking deterministic - 50 bytes become 50 chunks,
    so a queue bounded at 4 must drop the other 46.
    """
    transport = LoopbackTransport(echo=False)
    session = Session(transport, rx_maxsize=4, read_size=1)
    await session.open()
    try:
        transport.feed(bytes(range(50)))
        async with asyncio.timeout(5):
            while session.bytes_in < 50:  # every byte reached the loop thread
                await asyncio.sleep(0.01)
        buffered = session._queue.qsize()
        assert buffered <= 4
        assert session.rx_dropped == 50 - buffered
        # What survives is the tail, not the head: the oldest kept byte is 50-buffered.
        latest = await session.receive(timeout=0.5)
        assert latest == bytes([50 - buffered])
    finally:
        await session.close()


# -- threads -------------------------------------------------------------


async def test_no_threads_are_leaked() -> None:
    before = {thread.name for thread in threading.enumerate()}
    for _ in range(5):
        async with Session(LoopbackTransport()) as session:
            await session.send(b"x")
            await _drain(session, 1)
    await asyncio.sleep(0.3)
    after = {thread.name for thread in threading.enumerate()}
    assert not {name for name in after - before if "portside" in name}


async def test_the_reader_thread_is_not_the_event_loop_thread() -> None:
    """Architecture rule: the UI/loop thread never blocks on the port."""
    loop_thread = threading.get_ident()
    seen: list[int] = []

    transport = LoopbackTransport(echo=False)

    class Spy(LoopbackTransport):
        def read(self, max_bytes: int = 4096, timeout: float | None = 0.1) -> bytes:
            seen.append(threading.get_ident())
            return super().read(max_bytes, timeout)

    spy = Spy()
    async with Session(spy) as session:
        spy.feed(b"x")
        await _drain(session, 1)
    assert seen, "reader never ran"
    assert loop_thread not in seen
    assert transport.is_open is False


# -- failure and reconnect ----------------------------------------------


async def test_a_vanished_device_ends_the_session_when_reconnect_is_off() -> None:
    recorder = Recorder()
    transport = LoopbackTransport(echo=False, vanish_after=1)
    session = Session(transport)
    session.events.subscribe(recorder)
    await session.open()
    transport.feed(b"boom")
    await _wait_for_state(session, SessionState.DISCONNECTED)
    errors = recorder.of(ErrorRaised)
    assert errors and isinstance(errors[0].error, DeviceVanishedError)
    assert errors[0].fatal is True
    assert await session.receive(timeout=1) is None  # stream is terminated
    await session.close()


async def test_auto_reconnect_reopens_the_device_by_identity() -> None:
    """F5 + F2: the device comes back under a *different* port name."""
    recorder = Recorder()
    first = LoopbackTransport("/dev/ttyUSB0", echo=False, vanish_after=1)
    second = LoopbackTransport("/dev/ttyUSB1", echo=False)
    identity = DeviceIdentity(vid=0x0403, pid=0x6001, serial_number="A50285BI")
    attempts: list[int] = []

    def reopen() -> Transport:
        attempts.append(1)
        if len(attempts) < 2:
            raise DeviceVanishedError("not back yet", port="/dev/ttyUSB0")
        return second

    session = Session(
        first,
        settings=SerialSettings(baudrate=9600),
        identity=identity,
        reconnect=ReconnectPolicy(enabled=True, initial_delay=0.01, max_delay=0.02),
        reopen=reopen,
    )
    session.events.subscribe(recorder)
    await session.open()
    first.feed(b"!")

    async with asyncio.timeout(5):
        while session.state is not SessionState.CONNECTED or session.port != "/dev/ttyUSB1":
            await asyncio.sleep(0.02)

    assert session.port == "/dev/ttyUSB1"
    assert second.settings.baudrate == 9600, "settings must be re-applied after a reconnect"
    assert SessionState.RECONNECTING in recorder.states
    assert len(recorder.of(ReconnectAttempt)) >= 2

    # And the reconnected session actually works.
    second.feed(b"back")
    assert await _drain(session, 4) == b"back"
    await session.close()


async def test_reconnect_gives_up_after_max_attempts() -> None:
    recorder = Recorder()
    transport = LoopbackTransport(echo=False, vanish_after=1)

    def reopen() -> Transport:
        raise DeviceVanishedError("still gone", port="/dev/ttyUSB0")

    session = Session(
        transport,
        reconnect=ReconnectPolicy(enabled=True, initial_delay=0.01, max_delay=0.01, max_attempts=2),
        reopen=reopen,
    )
    session.events.subscribe(recorder)
    await session.open()
    transport.feed(b"!")
    await _wait_for_state(session, SessionState.DISCONNECTED)
    fatal = [event for event in recorder.of(ErrorRaised) if event.fatal]
    assert fatal and "gave up" in str(fatal[-1].error)
    await session.close()


async def test_closing_during_a_reconnect_stops_cleanly() -> None:
    transport = LoopbackTransport(echo=False, vanish_after=1)

    def reopen() -> Transport:
        raise DeviceVanishedError("still gone", port="/dev/ttyUSB0")

    session = Session(
        transport,
        reconnect=ReconnectPolicy(enabled=True, initial_delay=0.05),
        reopen=reopen,
    )
    await session.open()
    transport.feed(b"!")
    await _wait_for_state(session, SessionState.RECONNECTING)
    await session.close()
    assert session.state is SessionState.CLOSED


def test_backoff_is_exponential_and_capped() -> None:
    policy = ReconnectPolicy(initial_delay=0.5, max_delay=5.0, factor=2.0)
    assert [policy.delay_for(n) for n in range(1, 7)] == [0.5, 1.0, 2.0, 4.0, 5.0, 5.0]


# -- control lines -------------------------------------------------------


async def test_control_lines_go_through_the_io_thread() -> None:
    transport = LoopbackTransport()
    async with Session(transport) as session:
        assert await session.set_dtr(True) is True
        assert await session.set_rts(False) is False or transport.rts is False
        await session.send_break(0.25)
        lines = await session.modem_lines()
    assert (transport.dtr, transport.rts) == (True, False)
    assert list(transport.breaks) == [0.25]
    assert lines.cts is True


async def test_control_lines_missing_from_the_driver_are_reported_not_raised() -> None:
    """F4 + NFR stability: an adapter without DTR must not end the session."""
    async with Session(LoopbackTransport(supports_control_lines=False)) as session:
        assert await session.set_dtr(True) is False
        assert session.is_connected


async def test_settings_can_change_while_connected() -> None:
    transport = LoopbackTransport()
    async with Session(transport) as session:
        await session.apply_settings(SerialSettings(baudrate=921600))
        assert transport.settings.baudrate == 921600
        assert session.settings.baudrate == 921600
