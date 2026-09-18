"""The raw console abstraction the CLI pass-through sits on."""

from __future__ import annotations

import os
import sys

import pytest

from portside.platform.console import (
    PosixRawConsole,
    WindowsRawConsole,
    create_console,
)

posix_only = pytest.mark.skipif(sys.platform.startswith("win"), reason="POSIX only")


def test_create_console_picks_the_platform_implementation() -> None:
    assert isinstance(create_console("windows"), WindowsRawConsole)
    assert isinstance(create_console("linux"), PosixRawConsole)
    assert isinstance(create_console("macos"), PosixRawConsole)


def test_console_for_this_machine_is_constructible() -> None:
    assert create_console() is not None


@posix_only
def test_piped_input_is_read_then_reports_eof(tmp_path: object) -> None:
    read_fd, write_fd = os.pipe()
    with os.fdopen(read_fd, "rb", buffering=0) as reader:
        os.write(write_fd, b"piped data")
        console = PosixRawConsole(reader)
        assert console.is_tty is False
        assert console.read(1.0) == b"piped data"
        os.close(write_fd)
        assert console.read(1.0) is None


@posix_only
def test_read_times_out_with_empty_bytes() -> None:
    read_fd, write_fd = os.pipe()
    try:
        with os.fdopen(read_fd, "rb", buffering=0) as reader:
            assert PosixRawConsole(reader).read(0.05) == b""
    finally:
        os.close(write_fd)


@posix_only
def test_raw_mode_is_entered_and_always_restored() -> None:
    """The terminal must survive an exception in the pass-through loop."""
    import termios

    primary, secondary = os.openpty()
    try:
        with os.fdopen(secondary, "rb", buffering=0) as tty_file:
            console = PosixRawConsole(tty_file)
            assert console.is_tty is True
            before = termios.tcgetattr(tty_file.fileno())
            with pytest.raises(RuntimeError), console:
                during = termios.tcgetattr(tty_file.fileno())
                assert during != before, "raw mode did not change the terminal"
                raise RuntimeError("boom")
            assert termios.tcgetattr(tty_file.fileno()) == before
    finally:
        os.close(primary)


@posix_only
def test_non_tty_console_does_not_try_to_set_raw_mode() -> None:
    read_fd, write_fd = os.pipe()
    try:
        with os.fdopen(read_fd, "rb", buffering=0) as reader:
            console = PosixRawConsole(reader)
            with console:
                pass
    finally:
        os.close(write_fd)


def test_windows_console_reads_piped_input_without_msvcrt() -> None:
    """The non-tty path must work on every OS, which is what CI drives."""
    import io

    class FakeStdin(io.BytesIO):
        def fileno(self) -> int:
            raise OSError("not a real fd")

    console = WindowsRawConsole(FakeStdin(b"piped"))
    assert console.is_tty is False
    assert console.read(0.1) == b"piped"
    assert console.read(0.1) is None


def test_windows_console_polls_the_keyboard_when_attached_to_a_tty(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Drives the msvcrt path with a stub so it is covered off Windows too."""
    import io

    keys = [b"h", b"i"]

    class StubMsvcrt:
        @staticmethod
        def kbhit() -> bool:
            return bool(keys)

        @staticmethod
        def getch() -> bytes:
            return keys.pop(0)

    console = WindowsRawConsole(io.BytesIO())
    monkeypatch.setattr(console, "_msvcrt", StubMsvcrt)
    monkeypatch.setattr(type(console), "is_tty", property(lambda self: True))
    assert console.read(0.5) == b"hi"
    assert console.read(0.05) == b""
