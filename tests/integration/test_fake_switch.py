"""End-to-end against a scripted console over a real serial pair.

This is the closest CI can get to the PRD's M2 exit criterion ("configure a
real Cisco switch from the TUI on each OS").  The switch here is scripted, but
everything between it and the user is real: a socat serial pair, pySerial, the
session core, the emulator, pacing, logging and the TUI.

The hardware half of the criterion is in docs/HARDWARE-CHECK.md.
"""

from __future__ import annotations

import asyncio
import re
from pathlib import Path

import pytest

from portside.core.breaks import BreakMode
from portside.core.pacing import PacePolicy, paste_text
from portside.core.session import Session
from portside.core.settings import SerialSettings
from portside.core.textio import LineEnding, TextPolicy
from portside.logging.session_log import LogSettings, SessionLogger
from portside.logging.timestamps import TimestampMode
from portside.transport.serial_transport import PySerialTransport
from portside.tui.app import PortsideTUI

from ..conftest import SocatPair, requires_socat
from .fake_switch import FakeSwitch

pytestmark = [pytest.mark.socat, requires_socat]

SETTINGS = SerialSettings(baudrate=115200)
CISCO = TextPolicy(send_ending=LineEnding.CR)


async def _wait_for(predicate: object, timeout: float = 10.0) -> None:
    """Poll until ``predicate()`` is true, or fail the test."""
    async with asyncio.timeout(timeout):
        while not predicate():  # type: ignore[operator]
            await asyncio.sleep(0.02)


async def _session_for(pair: SocatPair) -> Session:
    session = Session(PySerialTransport(pair.left, SETTINGS), settings=SETTINGS)
    await session.open()
    return session


async def test_a_session_talks_to_the_scripted_console(socat_pair: SocatPair) -> None:
    with FakeSwitch(socat_pair.right):
        session = await _session_for(socat_pair)
        try:
            await session.send(b"\r")
            await _wait_for(lambda: b"Switch>" in _collected(session))
        finally:
            await session.close()


#: Received bytes are recorded on the session itself.  Keying a module-level
#: dict by id() would be wrong: CPython reuses ids, so a later session can
#: inherit a dead one's buffer and make an assertion pass without ever having
#: received anything.
_RX_ATTRIBUTE = "_test_received"


def _collected(session: Session) -> bytes:
    """Everything the session has received so far (tests only)."""
    return bytes(getattr(session, _RX_ATTRIBUTE, bytearray()))


@pytest.fixture(autouse=True)
def _collect_rx(monkeypatch: pytest.MonkeyPatch) -> None:
    """Record received bytes off the event bus, as the TUI would."""
    from portside.core.events import BytesReceived
    from portside.core.session import Session as _Session

    original = _Session.__init__

    def patched(self: _Session, *args: object, **kwargs: object) -> None:
        original(self, *args, **kwargs)  # type: ignore[arg-type]
        buffer = bytearray()
        setattr(self, _RX_ATTRIBUTE, buffer)
        self.events.subscribe(
            lambda event: buffer.extend(event.data) if isinstance(event, BytesReceived) else None
        )

    monkeypatch.setattr(_Session, "__init__", patched)


async def test_configuring_the_switch_with_paced_paste(socat_pair: SocatPair) -> None:
    """The M2 workflow end to end: prompt, enable, paste a config, verify it."""
    with FakeSwitch(socat_pair.right, prompt_delay=0.05) as switch:
        session = await _session_for(socat_pair)
        try:
            await session.send(b"\r")
            await _wait_for(lambda: b"Switch>" in _collected(session))

            config = "\n".join(
                [
                    "enable",
                    "cisco",
                    "configure terminal",
                    "hostname lab-sw1",
                    "interface GigabitEthernet1/0/1",
                    " description uplink to core",
                    " switchport mode trunk",
                    "end",
                ]
            )
            policy = PacePolicy(
                char_delay=0.0,
                line_delay=0.0,
                wait_for_prompt=r"(>|#|Password: )\s*$",
                prompt_timeout=5.0,
            )
            result = await paste_text(session, config, policy, text_policy=CISCO)

            assert result.complete is True, result.describe()
            await _wait_for(lambda: switch.hostname == "lab-sw1")
            assert switch.config_lines == [
                "hostname lab-sw1",
                "interface GigabitEthernet1/0/1",
                "description uplink to core",
                "switchport mode trunk",
            ]
        finally:
            await session.close()


async def test_pacing_waits_for_a_slow_device(socat_pair: SocatPair) -> None:
    """A device that takes its time must not have lines pushed at it (F13)."""
    with FakeSwitch(socat_pair.right, prompt_delay=0.3) as switch:
        session = await _session_for(socat_pair)
        try:
            await session.send(b"\r")
            await _wait_for(lambda: b"Switch>" in _collected(session))
            policy = PacePolicy(line_delay=0.0, wait_for_prompt=r"[>#]\s*$", prompt_timeout=5.0)
            result = await paste_text(
                session, "show version\nshow clock\nshow inventory", policy, text_policy=CISCO
            )
            assert result.complete is True
            # The last line is sent without waiting for another prompt, so the
            # slow device may still be chewing when the paste returns.
            await _wait_for(lambda: len(switch.commands) == 3)
            assert switch.commands == ["show version", "show clock", "show inventory"]
        finally:
            await session.close()


async def test_a_break_reaches_the_device(socat_pair: SocatPair) -> None:
    """F12 over a real port: the emulated break is what a PTY can carry."""
    with FakeSwitch(socat_pair.right) as switch:
        session = await _session_for(socat_pair)
        try:
            result = await session.send_break(0.05, mode=BreakMode.EMULATED)
            assert result.emulated is True
            await _wait_for(lambda: switch.breaks_seen >= 1)
            await _wait_for(lambda: b"break detected" in _collected(session))
            assert session.settings.baudrate == 115200, "settings must be restored"
        finally:
            await session.close()


async def test_the_session_log_captures_the_whole_exchange(
    socat_pair: SocatPair, tmp_path: Path
) -> None:
    """F15/F16: what you saw on screen is what ends up in the file."""
    with FakeSwitch(socat_pair.right):
        session = await _session_for(socat_pair)
        logger = SessionLogger(
            settings=LogSettings(
                directory=tmp_path, timestamps=TimestampMode.ELAPSED, flush_interval=0.0
            ),
            port=socat_pair.left,
        )
        detach = logger.attach(session)
        path = logger.start()
        try:
            await session.send(b"\r")
            await _wait_for(lambda: b"Switch>" in _collected(session))
            await session.send(b"show version\r")
            await _wait_for(lambda: b"show version" in _collected(session))
            # The logger and the collector are both bus subscribers, but the
            # file is only guaranteed complete once the logger has been closed.
        finally:
            await session.close()
            detach()

        logged = path.read_text()
        assert "Switch>" in logged, logged
        assert "show version" in logged, logged
        stamped = [line for line in logged.splitlines() if line]
        assert stamped, logged
        assert all(re.match(r"^\[\d{2}:\d{2}:\d{2}\.\d{3}\] ", line) for line in stamped), (
            f"every line must carry an elapsed stamp:\n{logged}"
        )


async def test_the_tui_drives_the_switch(socat_pair: SocatPair, tmp_path: Path) -> None:
    """The exit criterion's shape: configure the device from the TUI itself."""
    config = tmp_path / "switch.cfg"
    config.write_text("configure terminal\nhostname from-the-tui\nend\n")

    with FakeSwitch(socat_pair.right, prompt_delay=0.02) as switch:
        session = await _session_for(socat_pair)
        app = PortsideTUI(
            session,
            text_policy=CISCO,
            pace=PacePolicy(line_delay=0.0, wait_for_prompt=r"[>#]\s*$", prompt_timeout=5.0),
        )
        try:
            async with app.run_test() as pilot:
                # Wake the console up and check the prompt is rendered.
                await pilot.press("enter")
                await _wait_for(
                    lambda: any("Switch>" in line for line in app.terminal.all_text_lines())
                )

                # Type a command by hand, as a user would.
                for key in "enable":
                    await pilot.press(key)
                await pilot.press("enter")
                await _wait_for(
                    lambda: any("Password" in line for line in app.terminal.all_text_lines())
                )
                for key in "cisco":
                    await pilot.press(key)
                await pilot.press("enter")
                await _wait_for(lambda: switch._mode == "enable")

                # Then paste a config file through the TUI's paste command.
                await pilot.press("f4")
                app.prompt.value = str(config)
                await pilot.press("enter")
                await _wait_for(lambda: switch.hostname == "from-the-tui", timeout=15)

            assert switch.config_lines == ["hostname from-the-tui"]
        finally:
            await session.close()


async def test_a_break_from_the_tui_is_reported(socat_pair: SocatPair) -> None:
    with FakeSwitch(socat_pair.right) as switch:
        session = await _session_for(socat_pair)
        session.break_mode = BreakMode.EMULATED
        app = PortsideTUI(session, text_policy=CISCO, break_duration=0.05)
        try:
            async with app.run_test() as pilot:
                await pilot.press("f2")
                await _wait_for(lambda: switch.breaks_seen >= 1)
                await _wait_for(lambda: "break" in app.last_message)
        finally:
            await session.close()
