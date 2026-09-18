"""Paced paste and wait-for-prompt (F13).

Timing is injected everywhere, so these tests assert the *schedule* rather than
waiting for real clocks.
"""

from __future__ import annotations

import asyncio
from collections.abc import AsyncIterator

import pytest

from portside.core.errors import PortsideError
from portside.core.pacing import (
    PacePolicy,
    PasteAbortedError,
    PasteProgress,
    paste_text,
)
from portside.core.session import Session
from portside.core.textio import LineEnding, TextPolicy
from portside.transport.loopback import LoopbackTransport

CONFIG = "configure terminal\ninterface Gi1/0/1\n description uplink\nend"


class FakeClock:
    """A clock that only moves when something sleeps."""

    def __init__(self) -> None:
        self.time = 0.0
        self.sleeps: list[float] = []

    async def sleep(self, delay: float) -> None:
        self.sleeps.append(delay)
        self.time += delay
        await asyncio.sleep(0)  # let other tasks run, but consume no real time

    def now(self) -> float:
        return self.time


@pytest.fixture
async def session() -> AsyncIterator[Session]:
    transport = LoopbackTransport(echo=False)
    session = Session(transport)
    await session.open()
    yield session
    await session.close()


# -- policy validation ---------------------------------------------------


def test_defaults_are_gentle_but_not_glacial() -> None:
    policy = PacePolicy()
    assert policy.char_delay == 0.0
    assert policy.line_delay == 0.05
    assert policy.wait_for_prompt is None


@pytest.mark.parametrize(
    "kwargs",
    [
        {"char_delay": -0.1},
        {"line_delay": -1.0},
        {"prompt_timeout": 0.0},
        {"wait_for_prompt": "unclosed("},
    ],
)
def test_invalid_policies_are_rejected(kwargs: dict[str, object]) -> None:
    with pytest.raises(PortsideError):
        PacePolicy(**kwargs)  # type: ignore[arg-type]


# -- pacing --------------------------------------------------------------


async def test_every_line_arrives_in_order(session: Session) -> None:
    clock = FakeClock()
    result = await paste_text(session, CONFIG, PacePolicy(), sleep=clock.sleep, now=clock.now)
    written = bytes(session.transport.written)  # type: ignore[attr-defined]
    assert written == b"configure terminal\rinterface Gi1/0/1\r description uplink\rend\r"
    assert result.complete is True
    assert result.lines_sent == result.total_lines == 4


async def test_line_delay_is_applied_after_each_line(session: Session) -> None:
    clock = FakeClock()
    await paste_text(session, CONFIG, PacePolicy(line_delay=0.2), sleep=clock.sleep, now=clock.now)
    # Three gaps between four lines: nothing waits after the last one.
    assert clock.sleeps == [0.2] * 3
    assert clock.now() == pytest.approx(0.6)


async def test_char_delay_paces_every_character(session: Session) -> None:
    """Per-character pacing is what slow consoles actually need (F13)."""
    clock = FakeClock()
    text = "abc"
    await paste_text(
        session,
        text,
        PacePolicy(char_delay=0.01, line_delay=0.0),
        sleep=clock.sleep,
        now=clock.now,
    )
    # Three characters plus the line ending; no trailing line delay.
    assert clock.sleeps == [0.01] * 4


async def test_zero_delays_send_each_line_in_one_write(session: Session) -> None:
    clock = FakeClock()
    await paste_text(
        session, "one\ntwo", PacePolicy(line_delay=0.0), sleep=clock.sleep, now=clock.now
    )
    assert clock.sleeps == []
    assert bytes(session.transport.written) == b"one\rtwo\r"  # type: ignore[attr-defined]


async def test_the_send_line_ending_is_honoured(session: Session) -> None:
    clock = FakeClock()
    await paste_text(
        session,
        "one\ntwo",
        PacePolicy(line_delay=0.0),
        text_policy=TextPolicy(send_ending=LineEnding.CRLF),
        sleep=clock.sleep,
        now=clock.now,
    )
    assert bytes(session.transport.written) == b"one\r\ntwo\r\n"  # type: ignore[attr-defined]


async def test_progress_is_reported_per_line(session: Session) -> None:
    clock = FakeClock()
    seen: list[PasteProgress] = []
    await paste_text(
        session, CONFIG, PacePolicy(), on_progress=seen.append, sleep=clock.sleep, now=clock.now
    )
    assert [progress.line for progress in seen] == [1, 2, 3, 4]
    assert seen[-1].fraction == 1.0
    assert seen[-1].bytes_sent == len(CONFIG.replace("\n", "\r")) + 1


async def test_an_empty_paste_does_nothing(session: Session) -> None:
    result = await paste_text(session, "", PacePolicy())
    assert result.total_lines == 0
    assert result.complete is True


# -- wait for prompt -----------------------------------------------------


async def test_the_last_line_does_not_wait_for_another_prompt(session: Session) -> None:
    """Otherwise every paste ends in a pointless prompt timeout."""
    clock = FakeClock()
    policy = PacePolicy(wait_for_prompt=r"never-appears", prompt_timeout=1.0)
    result = await paste_text(session, "only one line", policy, sleep=clock.sleep, now=clock.now)
    assert result.complete is True
    assert result.timed_out_lines == ()
    assert clock.sleeps == []


async def test_no_line_is_sent_before_the_prompt_arrives() -> None:
    """The point of wait-for-prompt: never outrun the device (F13)."""
    transport = LoopbackTransport(echo=False)
    session = Session(transport)
    await session.open()
    try:
        policy = PacePolicy(wait_for_prompt=r"Switch\(config\)#", prompt_timeout=5.0)
        paste = asyncio.create_task(paste_text(session, "line one\nline two", policy))

        async with asyncio.timeout(5):
            while not transport.written:
                await asyncio.sleep(0.01)
        assert bytes(transport.written) == b"line one\r", "second line sent too early"

        transport.feed(b"Switch(config)#")
        async with asyncio.timeout(5):
            result = await paste
        assert bytes(transport.written) == b"line one\rline two\r"
        assert result.complete is True
    finally:
        await session.close()


async def test_a_prompt_split_across_reads_still_matches() -> None:
    transport = LoopbackTransport(echo=False)
    session = Session(transport)
    await session.open()
    try:
        policy = PacePolicy(wait_for_prompt=r"Switch>")
        paste = asyncio.create_task(paste_text(session, "show version\nshow clock", policy))
        await asyncio.sleep(0.05)
        transport.feed(b"Swi")
        await asyncio.sleep(0.05)
        transport.feed(b"tch>")
        async with asyncio.timeout(5):
            result = await paste
        assert result.complete is True
    finally:
        await session.close()


async def test_a_prompt_that_never_arrives_aborts_rather_than_hanging(
    session: Session,
) -> None:
    clock = FakeClock()
    policy = PacePolicy(wait_for_prompt=r"never-appears", prompt_timeout=1.0)
    result = await paste_text(session, CONFIG, policy, sleep=clock.sleep, now=clock.now)
    assert result.aborted is True
    assert result.lines_sent == 1
    assert result.timed_out_lines == (1,)
    assert "aborted" in result.describe()


async def test_a_timeout_can_be_tolerated_instead(session: Session) -> None:
    clock = FakeClock()
    policy = PacePolicy(
        wait_for_prompt=r"never-appears", prompt_timeout=0.5, abort_on_timeout=False
    )
    result = await paste_text(session, "one\ntwo", policy, sleep=clock.sleep, now=clock.now)
    assert result.aborted is False
    assert result.lines_sent == 2
    assert result.timed_out_lines == (1,), "the last line does not wait for a prompt"


async def test_the_watcher_does_not_steal_data_from_the_terminal() -> None:
    """A paste must not consume the bytes the screen and the log need."""
    transport = LoopbackTransport(echo=False)
    session = Session(transport)
    await session.open()
    try:
        policy = PacePolicy(wait_for_prompt=r"ok")
        paste = asyncio.create_task(paste_text(session, "cmd\nsecond", policy))
        await asyncio.sleep(0.05)
        transport.feed(b"ok")
        async with asyncio.timeout(5):
            await paste
        assert await session.receive(timeout=1) == b"ok"
    finally:
        await session.close()


# -- cancellation --------------------------------------------------------


async def test_cancelling_stops_the_paste_and_leaves_the_session_usable() -> None:
    transport = LoopbackTransport(echo=False)
    session = Session(transport)
    await session.open()
    try:
        policy = PacePolicy(wait_for_prompt=r"never", prompt_timeout=30.0)
        paste = asyncio.create_task(paste_text(session, CONFIG, policy))
        await asyncio.sleep(0.05)
        paste.cancel()
        with pytest.raises((PasteAbortedError, asyncio.CancelledError)):
            await paste
        assert session.is_connected
        await session.send(b"still works\r")
        assert b"still works" in bytes(transport.written)
    finally:
        await session.close()
