"""``portside connect`` argument handling, error reporting and pass-through (F3, F6, F23)."""

from __future__ import annotations

import argparse
import asyncio
import errno
from collections.abc import Sequence

import pytest
import serial

from portside.cli import main as cli
from portside.core.errors import PermissionDeniedError, PortBusyError, PortNotFoundError
from portside.core.settings import FlowControl, Parity, StopBits
from portside.platform.console import RawConsole


class FakeConsole(RawConsole):
    """A scripted terminal: feeds `inputs` then reports EOF, records output."""

    def __init__(self, inputs: Sequence[bytes], *, is_tty: bool = False) -> None:
        super().__init__()
        self._inputs = list(inputs)
        self._is_tty = is_tty
        self.output = bytearray()
        self.raw_entered = False
        self.restored = False

    @property
    def is_tty(self) -> bool:
        return self._is_tty

    def read(self, timeout: float = 0.1) -> bytes | None:
        if self._inputs:
            return self._inputs.pop(0)
        return None

    def write(self, data: bytes) -> None:
        self.output.extend(data)

    def enter_raw(self) -> None:
        self.raw_entered = True

    def restore(self) -> None:
        self.restored = True


def parse(argv: list[str]) -> argparse.Namespace:
    return cli.build_parser().parse_args(argv)


# -- argument parsing ----------------------------------------------------


def test_defaults_are_115200_8n1_no_flow() -> None:
    settings = cli.settings_from_args(parse(["connect", "COM3"]))
    assert settings.describe() == "115200 8N1 none"


def test_every_line_setting_can_be_given() -> None:
    args = parse(
        [
            "connect",
            "/dev/ttyUSB0",
            "--baud",
            "921600",
            "--data-bits",
            "7",
            "--parity",
            "E",
            "--stop-bits",
            "2",
            "--flow",
            "rtscts",
        ]
    )
    settings = cli.settings_from_args(args)
    assert settings.baudrate == 921600
    assert settings.data_bits == 7
    assert settings.parity is Parity.EVEN
    assert settings.stop_bits is StopBits.TWO
    assert settings.flow_control is FlowControl.RTSCTS


def test_one_and_a_half_stop_bits_is_available() -> None:
    settings = cli.settings_from_args(parse(["connect", "COM3", "--stop-bits", "1.5"]))
    assert settings.stop_bits is StopBits.ONE_POINT_FIVE


def test_baud_accepts_short_and_hex_forms() -> None:
    assert cli.settings_from_args(parse(["connect", "COM3", "-b", "9600"])).baudrate == 9600


@pytest.mark.parametrize(
    "argv",
    [
        ["connect", "COM3", "--baud", "0"],
        ["connect", "COM3", "--baud", "5000000"],
        ["connect", "COM3", "--baud", "fast"],
        ["connect", "COM3", "--data-bits", "9"],
        ["connect", "COM3", "--parity", "Z"],
        ["connect", "COM3", "--stop-bits", "3"],
        ["connect", "COM3", "--flow", "hardware"],
        ["connect"],
    ],
)
def test_invalid_arguments_exit_with_the_usage_code(argv: list[str]) -> None:
    with pytest.raises(SystemExit) as caught:
        cli.build_parser().parse_args(argv)
    assert caught.value.code == cli.EXIT_USAGE


# -- exit codes (F6) -----------------------------------------------------


@pytest.mark.parametrize(
    ("error", "code"),
    [
        (
            PermissionDeniedError("denied", port="/dev/ttyUSB0", remedy="join dialout"),
            cli.EXIT_PERMISSION,
        ),
        (PortBusyError("busy", port="COM3", remedy="close the other app"), cli.EXIT_BUSY),
        (PortNotFoundError("missing", port="COM9", remedy="run --list"), cli.EXIT_NOT_FOUND),
    ],
)
def test_typed_errors_become_exit_codes_and_printed_remedies(
    error: Exception,
    code: int,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    def explode(*args: object, **kwargs: object) -> None:
        raise error

    monkeypatch.setattr("portside.cli.main.resolve_port", explode)
    assert cli.main(["connect", "COM3", "--baud", "9600"]) == code
    err = capsys.readouterr().err
    assert error.remedy in err  # type: ignore[attr-defined]


def test_permission_error_from_the_driver_is_reported_with_the_group_fix(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """The whole chain: pySerial error -> typed error -> remedy on stderr."""
    from portside.platform import platform_for
    from portside.transport.serial_transport import PySerialTransport

    cause = OSError(errno.EACCES, "Permission denied")
    failure = serial.SerialException("could not open port /dev/ttyUSB0")
    failure.__cause__ = cause

    def fail(self: object, *args: object, **kwargs: object) -> None:
        raise failure

    monkeypatch.setattr(serial, "Serial", fail)
    monkeypatch.setattr("portside.cli.main.resolve_port", lambda name, **kw: ("/dev/ttyUSB0", None))
    monkeypatch.setattr(
        "portside.cli.main.open_port",
        lambda device, settings, **kw: PySerialTransport(
            device, settings, platform=platform_for("linux")
        ),
    )
    assert cli.main(["connect", "/dev/ttyUSB0", "--baud", "9600"]) == cli.EXIT_PERMISSION
    assert "sudo usermod -aG dialout $USER" in capsys.readouterr().err


# -- pass-through --------------------------------------------------------


async def test_pass_through_sends_stdin_and_prints_what_comes_back() -> None:
    """End to end over pySerial's loop:// URL — bytes in, same bytes out."""
    console = FakeConsole([b"show version\r"])
    args = parse(["connect", "loop://", "--baud", "115200", "--quiet", "--drain", "0.3"])
    assert await cli.run_connect(args, console=console) == cli.EXIT_OK
    assert bytes(console.output) == b"show version\r"


async def test_the_terminal_is_always_restored() -> None:
    console = FakeConsole([b"hi"], is_tty=True)
    args = parse(["connect", "loop://", "--quiet", "--drain", "0.2"])
    await cli.run_connect(args, console=console)
    assert console.raw_entered and console.restored


async def test_ctrl_bracket_quits_and_sends_what_came_before_it() -> None:
    console = FakeConsole([b"abc" + bytes([cli.ESCAPE_BYTE]) + b"ignored"], is_tty=True)
    args = parse(["connect", "loop://", "--quiet", "--drain", "0.2"])
    assert await cli.run_connect(args, console=console) == cli.EXIT_OK
    assert bytes(console.output) == b"abc"


async def test_escape_byte_is_data_when_input_is_piped() -> None:
    """A pipe has no user pressing Ctrl-]; 0x1d is just a byte."""
    console = FakeConsole([bytes([cli.ESCAPE_BYTE, 0x41])], is_tty=False)
    args = parse(["connect", "loop://", "--quiet", "--drain", "0.3"])
    await cli.run_connect(args, console=console)
    assert bytes(console.output) == bytes([cli.ESCAPE_BYTE, 0x41])


async def test_status_messages_go_to_stderr_not_stdout(
    capsys: pytest.CaptureFixture[str],
) -> None:
    console = FakeConsole([b"x"])
    args = parse(["connect", "loop://", "--drain", "0.2"])
    await cli.run_connect(args, console=console)
    captured = capsys.readouterr()
    assert captured.out == ""
    assert "connected to loop://" in captured.err
    assert "closed loop://" in captured.err


async def test_reconnect_on_a_url_warns_and_continues(
    capsys: pytest.CaptureFixture[str],
) -> None:
    """URLs have no device identity, so there is nothing to reconnect *to*."""
    console = FakeConsole([b"x"])
    args = parse(["connect", "loop://", "--reconnect", "--drain", "0.2"])
    assert await cli.run_connect(args, console=console) == cli.EXIT_OK
    assert "--reconnect needs a real port" in capsys.readouterr().err


async def test_keyboard_interrupt_exits_cleanly(monkeypatch: pytest.MonkeyPatch) -> None:
    def interrupt(coro: object, *args: object, **kwargs: object) -> None:
        coro.close()  # type: ignore[attr-defined]
        raise KeyboardInterrupt

    monkeypatch.setattr(asyncio, "run", interrupt)
    assert cli.main(["connect", "loop://"]) == cli.EXIT_OK
