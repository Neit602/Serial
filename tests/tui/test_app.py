"""Headless pilot tests for the TUI (F7, F8, F10, F12, F13, F15, F22).

Textual's ``run_test`` drives the real app without a terminal, so these run on
all three CI platforms.
"""

from __future__ import annotations

import asyncio
from pathlib import Path
from typing import Any

import pytest

from portside.core.session import Session
from portside.core.textio import LineEnding, TextPolicy
from portside.emulation.hexview import ViewMode
from portside.logging.session_log import LogSettings, SessionLogger
from portside.logging.timestamps import TimestampMode
from portside.transport.loopback import LoopbackTransport
from portside.tui.app import PortsideTUI


class Harness:
    """A session on a loopback transport, plus the app under test."""

    def __init__(self, app: PortsideTUI, transport: LoopbackTransport, session: Session):
        self.app = app
        self.transport = transport
        self.session = session

    async def feed(self, data: bytes, *, expect: int | None = None) -> None:
        """Send data from the 'device' and wait for the app to consume it."""
        target = (expect if expect is not None else len(data)) + self.session.bytes_in
        self.transport.feed(data)
        async with asyncio.timeout(5):
            while self.session.bytes_in < target:
                await asyncio.sleep(0.01)
        await asyncio.sleep(0.05)  # let the frame timer paint


def logging_now(logger: SessionLogger) -> bool:
    """Read through a call so one assert does not narrow the next."""
    return logger.is_logging


def following(harness: Harness) -> bool:
    """Read through a call so one assert does not narrow the next."""
    return harness.app.terminal.following


def view_mode(harness: Harness) -> ViewMode:
    """Read through a call so one assert does not narrow the next."""
    return harness.app.view.view_mode


def stamp_mode(logger: SessionLogger) -> TimestampMode:
    """Read through a call so one assert does not narrow the next."""
    return logger.settings.timestamps


async def make_app(**kwargs: object) -> Harness:
    transport = LoopbackTransport(echo=False)
    session = Session(transport)
    await session.open()
    app = PortsideTUI(session, **kwargs)  # type: ignore[arg-type]
    return Harness(app, transport, session)


async def close(harness: Harness) -> None:
    await harness.session.close()


# -- rendering (F7, F22) -------------------------------------------------


async def test_device_output_reaches_the_screen() -> None:
    harness = await make_app()
    async with harness.app.run_test() as pilot:
        await harness.feed(b"Switch> show version\r\n")
        await pilot.pause()
        assert "Switch> show version" in harness.app.terminal.snapshot().text()[0]
    await close(harness)


async def test_the_status_bar_shows_port_framing_and_counters() -> None:
    harness = await make_app()
    async with harness.app.run_test() as pilot:
        await harness.feed(b"hello")
        await pilot.pause()
        status = harness.app.status.text
        assert harness.session.port in status
        assert "115200 8N1" in status
        assert "in 5 B" in status
    await close(harness)


async def test_typing_is_sent_to_the_device() -> None:
    harness = await make_app()
    async with harness.app.run_test() as pilot:
        await pilot.press("s", "h", "o", "w")
        await pilot.pause()
        assert bytes(harness.transport.written) == b"show"
    await close(harness)


async def test_enter_sends_the_configured_line_ending() -> None:
    harness = await make_app(text_policy=TextPolicy(send_ending=LineEnding.CRLF))
    async with harness.app.run_test() as pilot:
        await pilot.press("a", "enter")
        await pilot.pause()
        assert bytes(harness.transport.written) == b"a\r\n"
    await close(harness)


async def test_control_keys_reach_the_device() -> None:
    """Ctrl-C must interrupt the switch, not the terminal."""
    harness = await make_app()
    async with harness.app.run_test() as pilot:
        await pilot.press("ctrl+c")
        await pilot.pause()
        assert bytes(harness.transport.written) == b"\x03"
    await close(harness)


async def test_local_echo_shows_typed_characters() -> None:
    harness = await make_app(text_policy=TextPolicy(local_echo=True))
    async with harness.app.run_test() as pilot:
        await pilot.press("h", "i")
        await pilot.pause()
        assert harness.app.terminal.snapshot().text()[0] == "hi"
    await close(harness)


async def test_rendering_is_batched_not_per_byte(monkeypatch: pytest.MonkeyPatch) -> None:
    """PRD risk: a repaint per byte would spend the whole session drawing."""
    harness = await make_app()
    async with harness.app.run_test() as pilot:
        painted = 0
        original = harness.app.view.refresh

        def counting_refresh(*args: Any, **kwargs: Any) -> Any:
            nonlocal painted
            painted += 1
            return original(*args, **kwargs)

        monkeypatch.setattr(harness.app.view, "refresh", counting_refresh)
        for _ in range(200):
            harness.transport.feed(b"x")
        async with asyncio.timeout(5):
            while harness.session.bytes_in < 200:
                await asyncio.sleep(0.01)
        await pilot.pause()
        assert painted < 100, f"{painted} repaints for 200 bytes is per-byte rendering"
    await close(harness)


# -- commands ------------------------------------------------------------


async def test_f6_toggles_the_hex_view() -> None:
    harness = await make_app()
    async with harness.app.run_test() as pilot:
        await harness.feed(b"AB")
        await pilot.press("f6")
        await pilot.pause()
        assert view_mode(harness) is ViewMode.HEX
        assert "HEX" in harness.app.status.text
        rendered = harness.app.view.render()
        assert "41 42" in rendered.plain  # type: ignore[union-attr]
        await pilot.press("f6")
        assert view_mode(harness) is ViewMode.TEXT
    await close(harness)


async def test_f2_sends_a_break() -> None:
    harness = await make_app()
    async with harness.app.run_test() as pilot:
        await pilot.press("f2")
        await pilot.pause()
        assert list(harness.transport.breaks) == [0.25]
    await close(harness)


async def test_f5_starts_and_stops_logging(tmp_path: Path) -> None:
    logger = SessionLogger(settings=LogSettings(directory=tmp_path, flush_interval=0.0))
    harness = await make_app(logger=logger)
    async with harness.app.run_test() as pilot:
        await pilot.press("f5")
        await pilot.pause()
        assert logging_now(logger)
        assert "REC" in harness.app.status.text
        await harness.feed(b"logged output\r\n")
        await pilot.press("f5")
        await pilot.pause()
        assert not logging_now(logger)
    await close(harness)
    assert logger.path is not None
    assert "logged output" in logger.path.read_text()


async def test_f7_cycles_timestamp_modes(tmp_path: Path) -> None:
    logger = SessionLogger(settings=LogSettings(directory=tmp_path, flush_interval=0.0))
    harness = await make_app(logger=logger)
    async with harness.app.run_test() as pilot:
        assert stamp_mode(logger) is TimestampMode.OFF
        await pilot.press("f7")
        await pilot.pause()
        assert stamp_mode(logger) is TimestampMode.ABSOLUTE
        # A full cycle returns to where it started.
        for _ in range(len(TimestampMode)):
            await pilot.press("f7")
        await pilot.pause()
        assert stamp_mode(logger) is TimestampMode.ABSOLUTE
    await close(harness)


async def test_f9_clears_the_screen() -> None:
    harness = await make_app()
    async with harness.app.run_test() as pilot:
        await harness.feed(b"noise\r\n")
        await pilot.press("f9")
        await pilot.pause()
        assert all(not line for line in harness.app.terminal.snapshot().text())
    await close(harness)


async def test_f1_help_lists_every_command() -> None:
    harness = await make_app()
    async with harness.app.run_test() as pilot:
        await pilot.press("f1")
        await pilot.pause()
        assert "break" in harness.app.last_message
        assert "quit" in harness.app.last_message
    await close(harness)


# -- scrollback (F8) -----------------------------------------------------


async def test_pageup_scrolls_back_and_pagedown_returns() -> None:
    harness = await make_app()
    async with harness.app.run_test() as pilot:
        for n in range(80):
            harness.transport.feed(f"line{n}\r\n".encode())
        async with asyncio.timeout(5):
            while harness.session.bytes_in < sum(len(f"line{n}\r\n") for n in range(80)):
                await asyncio.sleep(0.01)
        await pilot.pause()
        await pilot.press("pageup")
        await pilot.pause()
        assert not following(harness)
        assert "scrollback" in harness.app.status.text
        await pilot.press("pagedown")
        await pilot.pause()
        assert harness.app.terminal.scroll_offset == 0
    await close(harness)


async def test_typing_returns_to_the_live_tail() -> None:
    harness = await make_app()
    async with harness.app.run_test() as pilot:
        for n in range(60):
            harness.transport.feed(f"line{n}\r\n".encode())
        await asyncio.sleep(0.2)
        await pilot.press("pageup")
        await pilot.pause()
        assert not following(harness)
        await pilot.press("x")
        await pilot.pause()
        assert following(harness)
    await close(harness)


# -- find (F8) -----------------------------------------------------------


async def test_find_highlights_matches() -> None:
    harness = await make_app()
    async with harness.app.run_test() as pilot:
        await harness.feed(b"interface Gi1/0/1 is up\r\n")
        await pilot.press("f3")
        await pilot.pause()
        assert harness.app.prompt.has_class("visible")
        harness.app.prompt.value = "Gi1/0/1"
        await pilot.press("enter")
        await pilot.pause()
        assert len(harness.app._matches) == 1
        assert not harness.app.prompt.has_class("visible")
    await close(harness)


async def test_find_reports_when_nothing_matches() -> None:
    harness = await make_app()
    async with harness.app.run_test() as pilot:
        await pilot.press("f3")
        harness.app.prompt.value = "not-in-the-buffer"
        await pilot.press("enter")
        await pilot.pause()
        assert "no match" in harness.app.last_message
    await close(harness)


async def test_escape_closes_the_prompt_without_acting() -> None:
    harness = await make_app()
    async with harness.app.run_test() as pilot:
        await pilot.press("f3")
        await pilot.pause()
        await pilot.press("escape")
        await pilot.pause()
        assert not harness.app.prompt.has_class("visible")
        assert harness.app._matches == ()
    await close(harness)


# -- paste (F13) ---------------------------------------------------------


async def test_f4_pastes_a_file(tmp_path: Path) -> None:
    config = tmp_path / "switch.cfg"
    config.write_text("configure terminal\nhostname lab-sw1\nend\n")
    harness = await make_app()
    async with harness.app.run_test() as pilot:
        await pilot.press("f4")
        await pilot.pause()
        harness.app.prompt.value = str(config)
        await pilot.press("enter")
        async with asyncio.timeout(10):
            while b"end" not in bytes(harness.transport.written):
                await asyncio.sleep(0.02)
        assert bytes(harness.transport.written) == b"configure terminal\rhostname lab-sw1\rend\r"
    await close(harness)


async def test_pasting_a_missing_file_reports_the_error(tmp_path: Path) -> None:
    harness = await make_app()
    async with harness.app.run_test() as pilot:
        await pilot.press("f4")
        harness.app.prompt.value = str(tmp_path / "nope.cfg")
        await pilot.press("enter")
        await pilot.pause()
        assert "cannot read" in harness.app.last_message
    await close(harness)


# -- themes (F8, accessibility) -----------------------------------------


async def test_f8_cycles_themes() -> None:
    harness = await make_app()
    async with harness.app.run_test() as pilot:
        first = harness.app.theme
        await pilot.press("f8")
        await pilot.pause()
        assert harness.app.theme != first
    await close(harness)


# -- session events ------------------------------------------------------


async def test_a_session_error_is_shown_to_the_user() -> None:
    from portside.core.errors import DeviceVanishedError
    from portside.core.events import ErrorRaised

    harness = await make_app()
    async with harness.app.run_test() as pilot:
        harness.session.events.publish(
            ErrorRaised(DeviceVanishedError("unplugged", port="/dev/ttyUSB0", remedy="replug it"))
        )
        await pilot.pause()
        assert "replug it" in harness.app.last_message
    await close(harness)


async def test_the_app_stops_cleanly_and_leaves_no_tasks() -> None:
    harness = await make_app()
    async with harness.app.run_test() as pilot:
        await harness.feed(b"data")
        await pilot.pause()
    await close(harness)
    assert harness.app._pump is None or harness.app._pump.done()
