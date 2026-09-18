"""Session logging: formats, templates, rotation, timestamps (F15, F16)."""

from __future__ import annotations

import asyncio
from datetime import datetime
from pathlib import Path

import pytest

from portside.core.session import Session
from portside.emulation.decoder import CharacterSet
from portside.logging.session_log import (
    LogFormat,
    LogSettings,
    SessionLogger,
    resolve_template,
)
from portside.logging.timestamps import TimestampMode
from portside.transport.loopback import LoopbackTransport

WHEN = datetime.fromisoformat("2026-09-18T14:30:05+00:00")


class Clock:
    """A clock the test drives by hand."""

    def __init__(self, start: float = WHEN.timestamp()) -> None:
        self.time = start

    def __call__(self) -> float:
        return self.time

    def advance(self, seconds: float) -> None:
        self.time += seconds


def logging_now(logger: SessionLogger) -> bool:
    """Read through a call so one assert does not narrow the next."""
    return logger.is_logging


def make_logger(tmp_path: Path, clock: Clock, **overrides: object) -> SessionLogger:
    # Flush on every write by default, so assertions can read the file directly.
    options: dict[str, object] = {"flush_interval": 0.0, **overrides}
    settings = LogSettings(directory=tmp_path, **options)  # type: ignore[arg-type]
    return SessionLogger(settings=settings, port="/dev/ttyUSB0", now=clock)


# -- filename templates (F15) -------------------------------------------


def test_every_placeholder_is_expanded() -> None:
    name = resolve_template(
        "{port}-{date}-{time}-{profile}-{n}.log",
        port="ttyUSB0",
        profile="cisco",
        when=WHEN,
        index=3,
    )
    assert name == "ttyUSB0-20260918-143005-cisco-3.log"


@pytest.mark.parametrize("port", ["/dev/ttyUSB0", "COM3:", "/dev/cu.usbserial-A50285BI"])
def test_port_names_are_made_safe_for_every_filesystem(port: str) -> None:
    """A template must produce a legal name on Windows as well as POSIX."""
    name = resolve_template("{port}.log", port=port, when=WHEN)
    assert not set(name) & set('<>:"/\\|?*')
    assert name.endswith(".log")


def test_device_is_an_alias_for_port() -> None:
    assert resolve_template("{device}.log", port="COM3", when=WHEN) == "COM3.log"


def test_an_unknown_placeholder_falls_back_instead_of_failing() -> None:
    """Losing a session to a typo in a template would be unforgivable."""
    name = resolve_template("{nonsense}.log", port="COM3", when=WHEN)
    assert name.startswith("portside-COM3-")


def test_missing_profile_becomes_default() -> None:
    assert resolve_template("{profile}.log", when=WHEN) == "default.log"


# -- text logging (F15) --------------------------------------------------


def test_text_log_records_what_the_device_sent(tmp_path: Path) -> None:
    clock = Clock()
    logger = make_logger(tmp_path, clock)
    path = logger.start()
    logger.write(b"Switch> show version\r\n")
    logger.stop()
    assert path.read_text() == "Switch> show version\n"


def test_text_logs_use_lf_on_every_platform(tmp_path: Path) -> None:
    """PRD: logs are UTF-8 with LF internally, Windows included."""
    clock = Clock()
    logger = make_logger(tmp_path, clock)
    path = logger.start()
    logger.write(b"one\r\ntwo\rthree\n")
    logger.stop()
    assert path.read_bytes() == b"one\ntwo\nthree\n"


def test_an_unterminated_final_line_is_still_written(tmp_path: Path) -> None:
    clock = Clock()
    logger = make_logger(tmp_path, clock)
    path = logger.start()
    logger.write(b"no newline at the end")
    logger.stop()
    assert path.read_text() == "no newline at the end\n"


def test_text_logs_decode_the_configured_charset(tmp_path: Path) -> None:
    clock = Clock()
    logger = make_logger(tmp_path, clock, charset=CharacterSet.CP437)
    path = logger.start()
    logger.write(bytes([0xDB, 0x0A]))
    logger.stop()
    assert path.read_text(encoding="utf-8") == "█\n"


def test_a_character_split_across_reads_is_logged_once(tmp_path: Path) -> None:
    clock = Clock()
    logger = make_logger(tmp_path, clock)
    path = logger.start()
    logger.write(b"caf\xc3")
    logger.write(b"\xa9\n")
    logger.stop()
    assert path.read_text() == "café\n"


# -- raw logging (F15) ---------------------------------------------------


def test_raw_log_is_byte_exact(tmp_path: Path) -> None:
    clock = Clock()
    logger = make_logger(tmp_path, clock, log_format=LogFormat.RAW)
    path = logger.start()
    payload = bytes(range(256))
    logger.write(payload)
    logger.stop()
    assert path.read_bytes() == payload


def test_raw_logs_ignore_timestamps_and_notes(tmp_path: Path) -> None:
    """Stamping a binary capture would corrupt it."""
    clock = Clock()
    logger = make_logger(
        tmp_path, clock, log_format=LogFormat.RAW, timestamps=TimestampMode.ABSOLUTE
    )
    path = logger.start()
    logger.write(b"\x01\x02")
    logger.write_note("break sent")
    logger.stop()
    assert path.read_bytes() == b"\x01\x02"


# -- timestamps in logs (F16) -------------------------------------------


def test_absolute_timestamps_prefix_each_line(tmp_path: Path) -> None:
    clock = Clock()
    logger = make_logger(tmp_path, clock, timestamps=TimestampMode.ABSOLUTE)
    path = logger.start()
    logger.write(b"first\n")
    clock.advance(1.0)
    logger.write(b"second\n")
    logger.stop()
    lines = path.read_text().splitlines()
    assert len(lines) == 2
    assert all(line.startswith("[2026-09-18T") for line in lines)
    assert lines[0].endswith("first")
    assert lines[1].endswith("second")


def test_elapsed_timestamps_start_at_zero(tmp_path: Path) -> None:
    clock = Clock()
    logger = make_logger(tmp_path, clock, timestamps=TimestampMode.ELAPSED)
    path = logger.start()
    logger.write(b"boot\n")
    clock.advance(2.5)
    logger.write(b"ready\n")
    logger.stop()
    assert path.read_text() == "[00:00:00.000] boot\n[00:00:02.500] ready\n"


def test_delta_timestamps_show_the_gap(tmp_path: Path) -> None:
    clock = Clock()
    logger = make_logger(tmp_path, clock, timestamps=TimestampMode.DELTA)
    path = logger.start()
    logger.write(b"a\n")
    clock.advance(0.25)
    logger.write(b"b\n")
    logger.stop()
    assert path.read_text() == "[+00:00:00.000] a\n[+00:00:00.250] b\n"


def test_a_line_split_across_reads_gets_one_timestamp(tmp_path: Path) -> None:
    """The stamp is the arrival of the line's first byte, not its last (F16)."""
    clock = Clock()
    logger = make_logger(tmp_path, clock, timestamps=TimestampMode.ELAPSED)
    path = logger.start()
    logger.write(b"half a ")
    clock.advance(5.0)
    logger.write(b"line\n")
    logger.stop()
    assert path.read_text() == "[00:00:00.000] half a line\n"


def test_the_elapsed_clock_can_be_reset_mid_session(tmp_path: Path) -> None:
    clock = Clock()
    logger = make_logger(tmp_path, clock, timestamps=TimestampMode.ELAPSED)
    path = logger.start()
    clock.advance(60)
    logger.write(b"before\n")
    logger.reset_elapsed()
    clock.advance(1)
    logger.write(b"after\n")
    logger.stop()
    assert path.read_text().splitlines()[1].startswith("[00:00:01.000]")


def test_portside_notes_are_stamped_too(tmp_path: Path) -> None:
    clock = Clock()
    logger = make_logger(tmp_path, clock, timestamps=TimestampMode.ELAPSED)
    path = logger.start()
    logger.write(b"output")
    logger.write_note("break sent")
    logger.stop()
    text = path.read_text()
    assert "[portside] break sent" in text
    assert text.splitlines()[0].endswith("output"), "the open line is closed first"


# -- rotation (F15) ------------------------------------------------------


def test_rotation_splits_the_file_and_keeps_every_byte(tmp_path: Path) -> None:
    clock = Clock()
    logger = make_logger(tmp_path, clock, log_format=LogFormat.RAW, max_bytes=100)
    logger.start()  # the parts are found by globbing, not by this one path
    payload = bytes(range(10)) * 50  # 500 bytes
    for index in range(0, len(payload), 10):
        logger.write(payload[index : index + 10])
    logger.stop()

    parts = sorted(tmp_path.glob("*.log*"), key=lambda item: item.stat().st_mtime)
    assert len(parts) > 1, "no rotation happened"
    recovered = b"".join(part.read_bytes() for part in parts)
    assert recovered == payload
    assert logger.rotations >= 4


def test_rotation_keeps_only_the_configured_number_of_files(tmp_path: Path) -> None:
    clock = Clock()
    logger = make_logger(tmp_path, clock, log_format=LogFormat.RAW, max_bytes=50, keep=2)
    logger.start()  # rotation is asserted through the directory, not this path
    for _ in range(20):
        logger.write(b"x" * 25)
        clock.advance(1)
    logger.stop()
    rotated = list(tmp_path.glob("*.log.*"))
    assert len(rotated) <= 2


def test_rotation_can_be_disabled(tmp_path: Path) -> None:
    clock = Clock()
    logger = make_logger(tmp_path, clock, log_format=LogFormat.RAW, max_bytes=0)
    logger.start()
    logger.write(b"y" * 10_000)
    logger.stop()
    assert list(tmp_path.glob("*.log.*")) == []
    assert logger.rotations == 0


# -- lifecycle -----------------------------------------------------------


def test_writing_while_stopped_is_a_no_op(tmp_path: Path) -> None:
    clock = Clock()
    logger = make_logger(tmp_path, clock)
    logger.write(b"dropped")
    assert list(tmp_path.iterdir()) == []
    assert not logger.is_logging


def test_start_and_stop_are_idempotent(tmp_path: Path) -> None:
    clock = Clock()
    logger = make_logger(tmp_path, clock)
    first = logger.start()
    assert logger.start() == first
    logger.stop()
    logger.stop()
    assert not logging_now(logger)


def test_restarting_appends_rather_than_truncating(tmp_path: Path) -> None:
    """A reconnect must not throw away what was already captured."""
    clock = Clock()
    logger = make_logger(tmp_path, clock)
    path = logger.start()
    logger.write(b"before the unplug\n")
    logger.stop()
    logger.start(path)
    logger.write(b"after the replug\n")
    logger.stop()
    assert path.read_text() == "before the unplug\nafter the replug\n"


def test_the_log_directory_is_created(tmp_path: Path) -> None:
    clock = Clock()
    settings = LogSettings(directory=tmp_path / "deep" / "nested", flush_interval=0.0)
    logger = SessionLogger(settings=settings, port="COM3", now=clock)
    path = logger.start()
    logger.write(b"x\n")
    logger.stop()
    assert path.exists()


# -- attached to a live session -----------------------------------------


async def test_logging_a_real_session_captures_everything(tmp_path: Path) -> None:
    clock = Clock()
    logger = make_logger(tmp_path, clock, log_format=LogFormat.RAW)
    transport = LoopbackTransport(echo=False)
    session = Session(transport)
    detach = logger.attach(session)
    await session.open()
    path = logger.start()
    try:
        transport.feed(b"Switch> ")
        async with asyncio.timeout(5):
            while session.bytes_in < 8:
                await asyncio.sleep(0.01)
    finally:
        await session.close()
        detach()
    assert path.read_bytes() == b"Switch> "


async def test_auto_start_opens_and_closes_with_the_session(tmp_path: Path) -> None:
    clock = Clock()
    logger = make_logger(tmp_path, clock, auto_start=True)
    transport = LoopbackTransport(echo=False)
    session = Session(transport)
    detach = logger.attach(session)
    await session.open()
    assert logger.is_logging is True
    transport.feed(b"hello\n")
    async with asyncio.timeout(5):
        while session.bytes_in < 6:
            await asyncio.sleep(0.01)
    await session.close()
    assert not logging_now(logger), "the file should close with the session"
    detach()
    assert logger.path is not None
    assert logger.path.read_text() == "hello\n"


async def test_a_slow_log_does_not_stall_the_session(tmp_path: Path) -> None:
    """NFR: logging must never become the bottleneck on the I/O path."""
    clock = Clock()
    logger = make_logger(tmp_path, clock, log_format=LogFormat.RAW, flush_interval=1.0)
    transport = LoopbackTransport(echo=False)
    session = Session(transport, rx_maxsize=4096)
    detach = logger.attach(session)
    await session.open()
    path = logger.start()
    payload = bytes(range(256)) * 200  # 51 KiB
    try:
        transport.feed(payload)
        async with asyncio.timeout(20):
            while session.bytes_in < len(payload):
                await asyncio.sleep(0.01)
    finally:
        await session.close()
        detach()
    assert path.read_bytes() == payload
