"""CLI additions in M2: TUI selection, profiles, logging flags (F15, F20, F23)."""

from __future__ import annotations

import argparse
from pathlib import Path

import pytest

from portside.cli import main as cli
from portside.core.breaks import BreakMode
from portside.core.settings import FlowControl, Parity, SerialSettings
from portside.core.textio import Backspace, LineEnding, TextPolicy
from portside.logging.session_log import LogFormat, LogSettings
from portside.logging.timestamps import TimestampMode
from portside.platform.console import RawConsole
from portside.profiles.store import Profile, ProfileStore


def parse(argv: list[str]) -> argparse.Namespace:
    return cli.build_parser().parse_args(argv)


class FakeConsole(RawConsole):
    def __init__(self, *, is_tty: bool) -> None:
        super().__init__()
        self._is_tty = is_tty

    @property
    def is_tty(self) -> bool:
        return self._is_tty

    def read(self, timeout: float = 0.1) -> bytes | None:
        return None


# -- TUI vs raw (F23) ----------------------------------------------------


def test_raw_is_forced_by_the_flag(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("sys.stdout.isatty", lambda: True)
    assert cli.use_tui(parse(["connect", "COM3", "--raw"]), FakeConsole(is_tty=True)) is False


def test_the_tui_is_used_for_an_interactive_terminal(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr("sys.stdout.isatty", lambda: True)
    assert cli.use_tui(parse(["connect", "COM3"]), FakeConsole(is_tty=True)) is True


def test_piped_input_selects_raw_pass_through(monkeypatch: pytest.MonkeyPatch) -> None:
    """Every M1-era script must keep working with no flags added."""
    monkeypatch.setattr("sys.stdout.isatty", lambda: True)
    assert cli.use_tui(parse(["connect", "COM3"]), FakeConsole(is_tty=False)) is False


def test_piped_output_selects_raw_pass_through(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("sys.stdout.isatty", lambda: False)
    assert cli.use_tui(parse(["connect", "COM3"]), FakeConsole(is_tty=True)) is False


# -- profiles applied to a connection (F20) ------------------------------


@pytest.fixture
def store(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> ProfileStore:
    store = ProfileStore(tmp_path / "profiles")
    monkeypatch.setattr(cli, "ProfileStore", lambda *a, **k: store)
    return store


def test_a_profile_supplies_the_settings(store: ProfileStore) -> None:
    store.save(
        Profile(
            name="lab",
            serial=SerialSettings(baudrate=9600, parity=Parity.EVEN, data_bits=7),
        )
    )
    argv = ["connect", "COM3", "--profile", "lab"]
    profile = cli.load_profile("lab")
    settings = cli.settings_from_args(parse(argv), profile, argv)
    assert (settings.baudrate, settings.parity, settings.data_bits) == (9600, Parity.EVEN, 7)


def test_explicit_flags_beat_the_profile(store: ProfileStore) -> None:
    """`--profile lab --baud 115200` must do the obvious thing."""
    store.save(Profile(name="lab", serial=SerialSettings(baudrate=9600)))
    argv = ["connect", "COM3", "--profile", "lab", "--baud", "115200"]
    profile = cli.load_profile("lab")
    settings = cli.settings_from_args(parse(argv), profile, argv)
    assert settings.baudrate == 115200


def test_without_a_profile_the_defaults_apply() -> None:
    argv = ["connect", "COM3"]
    settings = cli.settings_from_args(parse(argv), None, argv)
    assert settings.baudrate == 115200
    assert settings.flow_control is FlowControl.NONE


def test_an_unknown_profile_exits_with_a_usage_code(
    store: ProfileStore, capsys: pytest.CaptureFixture[str]
) -> None:
    assert cli.main(["connect", "loop://", "--profile", "nonexistent"]) == cli.EXIT_USAGE
    assert "no profile named" in capsys.readouterr().err


def test_line_discipline_comes_from_the_profile(store: ProfileStore) -> None:
    store.save(
        Profile(
            name="lab",
            text=TextPolicy(send_ending=LineEnding.CRLF, backspace=Backspace.BS, local_echo=True),
        )
    )
    policy = cli.text_policy_from_args(parse(["connect", "COM3"]), cli.load_profile("lab"))
    assert policy.send_ending is LineEnding.CRLF
    assert policy.backspace is Backspace.BS
    assert policy.local_echo is True


def test_line_discipline_flags_override_the_profile(store: ProfileStore) -> None:
    store.save(Profile(name="lab", text=TextPolicy(send_ending=LineEnding.CRLF)))
    args = parse(["connect", "COM3", "--send-ending", "lf", "--backspace", "bs"])
    policy = cli.text_policy_from_args(args, cli.load_profile("lab"))
    assert policy.send_ending is LineEnding.LF
    assert policy.backspace is Backspace.BS


def test_break_mode_can_be_forced() -> None:
    args = parse(["connect", "COM3", "--break-mode", "emulated"])
    assert BreakMode(args.break_mode) is BreakMode.EMULATED


# -- logging flags (F15, F16) -------------------------------------------


def test_log_flag_builds_a_logger(tmp_path: Path) -> None:
    target = tmp_path / "session.log"
    args = parse(["connect", "COM3", "--log", str(target), "--timestamps", "elapsed"])
    logger = cli.logger_from_args(args, None, "COM3")
    assert logger is not None
    assert logger.settings.auto_start is True
    assert logger.settings.timestamps is TimestampMode.ELAPSED
    assert logger.settings.directory == tmp_path
    assert logger.settings.template == "session.log"


def test_raw_log_format_is_available(tmp_path: Path) -> None:
    args = parse(["connect", "COM3", "--log", str(tmp_path / "capture.bin"), "--log-format", "raw"])
    logger = cli.logger_from_args(args, None, "COM3")
    assert logger is not None and logger.settings.log_format is LogFormat.RAW


def test_no_logging_is_configured_by_default() -> None:
    assert cli.logger_from_args(parse(["connect", "COM3"]), None, "COM3") is None


def test_a_profile_can_turn_logging_on(tmp_path: Path) -> None:
    profile = Profile(name="lab", logging=LogSettings(directory=tmp_path, auto_start=True))
    logger = cli.logger_from_args(parse(["connect", "COM3"]), profile, "COM3")
    assert logger is not None and logger.settings.auto_start is True


# -- portside profiles (F20) --------------------------------------------


def test_profiles_list_is_empty_to_begin_with(
    store: ProfileStore, capsys: pytest.CaptureFixture[str]
) -> None:
    assert cli.main(["profiles"]) == cli.EXIT_OK
    out = capsys.readouterr().out
    assert "No profiles yet" in out
    assert str(store.directory) in out


def test_profiles_list_shows_settings_and_devices(
    store: ProfileStore, capsys: pytest.CaptureFixture[str]
) -> None:
    store.save(
        Profile(name="lab", serial=SerialSettings(baudrate=9600), device_keys=("usb_0403_6001_A",))
    )
    assert cli.main(["profiles", "list"]) == cli.EXIT_OK
    out = capsys.readouterr().out
    assert "lab" in out and "9600" in out and "usb_0403_6001_A" in out


def test_profiles_show_prints_the_settings(
    store: ProfileStore, capsys: pytest.CaptureFixture[str]
) -> None:
    store.save(Profile(name="lab", description="Catalyst"))
    assert cli.main(["profiles", "show", "lab"]) == cli.EXIT_OK
    assert "Catalyst" in capsys.readouterr().out


def test_profiles_export_and_import_round_trip(
    store: ProfileStore, tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    store.save(Profile(name="lab", serial=SerialSettings(baudrate=19200)))
    shared = tmp_path / "shared.toml"
    assert cli.main(["profiles", "export", "lab", "--file", str(shared)]) == cli.EXIT_OK
    assert shared.is_file()
    store.delete("lab")
    assert cli.main(["profiles", "import", str(shared)]) == cli.EXIT_OK
    capsys.readouterr()
    # The profile keeps the name it was exported under, not the filename.
    assert store.load("lab").serial.baudrate == 19200


def test_profiles_delete(store: ProfileStore, capsys: pytest.CaptureFixture[str]) -> None:
    store.save(Profile(name="temp"))
    assert cli.main(["profiles", "delete", "temp"]) == cli.EXIT_OK
    assert store.names() == []


def test_profiles_commands_needing_a_name_say_so(
    store: ProfileStore, capsys: pytest.CaptureFixture[str]
) -> None:
    assert cli.main(["profiles", "show"]) == cli.EXIT_USAGE
    assert "needs a profile name" in capsys.readouterr().err
