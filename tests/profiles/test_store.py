"""TOML profiles and auto-apply by device identity (F20, F2)."""

from __future__ import annotations

from pathlib import Path

import pytest

from portside.core.breaks import BreakMode
from portside.core.identity import DeviceIdentity
from portside.core.pacing import PacePolicy
from portside.core.ports import PortInfo
from portside.core.settings import FlowControl, Parity, SerialSettings, StopBits
from portside.core.textio import Backspace, LineEnding, TextPolicy
from portside.emulation.decoder import CharacterSet
from portside.logging.session_log import LogFormat, LogSettings
from portside.logging.timestamps import TimestampMode
from portside.profiles.store import (
    SCHEMA_VERSION,
    Profile,
    ProfileError,
    ProfileStore,
    TerminalOptions,
)

FTDI = PortInfo(
    device="/dev/ttyUSB0",
    description="FT232R USB UART",
    vid=0x0403,
    pid=0x6001,
    serial_number="A50285BI",
)


@pytest.fixture
def store(tmp_path: Path) -> ProfileStore:
    return ProfileStore(tmp_path / "profiles")


def rich_profile() -> Profile:
    """A profile with every field set away from its default."""
    return Profile(
        name="cisco-lab",
        description="Catalyst 2960 console",
        serial=SerialSettings(
            baudrate=9600,
            data_bits=7,
            parity=Parity.EVEN,
            stop_bits=StopBits.TWO,
            flow_control=FlowControl.RTSCTS,
        ),
        text=TextPolicy(
            send_ending=LineEnding.CRLF,
            receive_ending=LineEnding.LF,
            local_echo=True,
            backspace=Backspace.BS,
        ),
        terminal=TerminalOptions(
            scrollback=5000,
            charset=CharacterSet.CP437,
            strip_high_bit=True,
            columns=132,
            rows=43,
        ),
        pacing=PacePolicy(
            char_delay=0.01,
            line_delay=0.2,
            wait_for_prompt=r"[>#]\s*$",
            prompt_timeout=10.0,
            abort_on_timeout=False,
        ),
        logging=LogSettings(
            directory=Path("/tmp/portside-logs"),
            template="{port}-{date}.log",
            log_format=LogFormat.RAW,
            timestamps=TimestampMode.DELTA,
            charset=CharacterSet.LATIN1,
            auto_start=True,
            max_bytes=1024,
            keep=3,
        ),
        break_mode=BreakMode.EMULATED,
        break_duration=0.5,
        device_keys=("usb_0403_6001_A50285BI",),
    )


# -- round trip ----------------------------------------------------------


def test_every_setting_survives_a_round_trip(store: ProfileStore) -> None:
    profile = rich_profile()
    store.save(profile)
    assert store.load("cisco-lab") == profile


def test_a_default_profile_round_trips(store: ProfileStore) -> None:
    profile = Profile(name="plain")
    store.save(profile)
    assert store.load("plain") == profile


def test_the_file_is_readable_toml(store: ProfileStore) -> None:
    """Profiles are meant to be edited by hand and diffed in git."""
    import tomllib

    path = store.save(rich_profile())
    data = tomllib.loads(path.read_text())
    assert data["schema_version"] == SCHEMA_VERSION
    assert data["serial"]["baudrate"] == 9600
    assert data["pacing"]["wait_for_prompt"] == r"[>#]\s*$"


def test_saving_twice_overwrites_rather_than_duplicating(store: ProfileStore) -> None:
    store.save(Profile(name="one", serial=SerialSettings(baudrate=9600)))
    store.save(Profile(name="one", serial=SerialSettings(baudrate=115200)))
    assert store.names() == ["one"]
    assert store.load("one").serial.baudrate == 115200


# -- listing, deleting, errors ------------------------------------------


def test_an_empty_store_lists_nothing(store: ProfileStore) -> None:
    assert store.names() == []
    assert store.all() == []


def test_profiles_are_listed_alphabetically(store: ProfileStore) -> None:
    for name in ("zulu", "alpha", "mike"):
        store.save(Profile(name=name))
    assert store.names() == ["alpha", "mike", "zulu"]


def test_loading_a_missing_profile_says_where_to_look(store: ProfileStore) -> None:
    with pytest.raises(ProfileError) as caught:
        store.load("nonexistent")
    assert caught.value.remedy is not None
    assert str(store.directory) in caught.value.remedy


def test_deleting_is_idempotent(store: ProfileStore) -> None:
    store.save(Profile(name="temp"))
    store.delete("temp")
    store.delete("temp")
    assert store.names() == []


def test_a_profile_name_with_awkward_characters_is_stored_safely(
    store: ProfileStore,
) -> None:
    """A name is a user string; a filename has rules, on Windows especially."""
    path = store.save(Profile(name="lab: switch/1"))
    assert not set(path.name) & set('<>:"/\\|?*')
    assert path.is_file()


def test_malformed_toml_is_a_typed_error(store: ProfileStore) -> None:
    store.directory.mkdir(parents=True, exist_ok=True)
    broken = store.directory / "broken.toml"
    broken.write_text("this is not = valid = toml")
    with pytest.raises(ProfileError, match="not valid TOML"):
        store.load("broken")


def test_an_unreadable_profile_does_not_hide_the_others(store: ProfileStore) -> None:
    store.save(Profile(name="good"))
    (store.directory / "bad.toml").write_text("nonsense = = =")
    assert [profile.name for profile in store.all()] == ["good"]


def test_a_newer_schema_version_is_refused_not_misread(store: ProfileStore) -> None:
    """Silently misreading a future profile is worse than refusing it."""
    store.directory.mkdir(parents=True, exist_ok=True)
    future = store.directory / "future.toml"
    future.write_text(f'schema_version = {SCHEMA_VERSION + 1}\nname = "future"\n')
    with pytest.raises(ProfileError) as caught:
        store.load("future")
    assert "schema version" in str(caught.value)
    assert caught.value.remedy is not None


def test_invalid_values_are_rejected(store: ProfileStore) -> None:
    store.directory.mkdir(parents=True, exist_ok=True)
    (store.directory / "bad-baud.toml").write_text('name = "bad-baud"\n[serial]\nbaudrate = 7\n')
    with pytest.raises(ProfileError):
        store.load("bad-baud")


# -- device binding (F2 + F20) ------------------------------------------


def test_a_profile_auto_applies_to_its_device(store: ProfileStore) -> None:
    identity = DeviceIdentity.from_port(FTDI)
    store.save(Profile(name="lab").with_device(identity))
    found = store.for_device(identity)
    assert found is not None and found.name == "lab"


def test_auto_apply_follows_the_device_across_a_port_rename(store: ProfileStore) -> None:
    """The whole point of F2: same adapter, different port name, same profile."""
    from dataclasses import replace

    identity = DeviceIdentity.from_port(FTDI)
    store.save(Profile(name="lab").with_device(identity))
    on_windows = DeviceIdentity.from_port(replace(FTDI, device="COM9"))
    found = store.for_device(on_windows)
    assert found is not None and found.name == "lab"


def test_a_different_adapter_gets_no_profile(store: ProfileStore) -> None:
    from dataclasses import replace

    store.save(Profile(name="lab").with_device(DeviceIdentity.from_port(FTDI)))
    other = DeviceIdentity.from_port(replace(FTDI, serial_number="OTHER"))
    assert store.for_device(other) is None


def test_binding_a_device_twice_does_not_duplicate_it() -> None:
    identity = DeviceIdentity.from_port(FTDI)
    profile = Profile(name="lab").with_device(identity).with_device(identity)
    assert profile.device_keys == (identity.key(),)


def test_one_profile_can_serve_several_adapters() -> None:
    from dataclasses import replace

    first = DeviceIdentity.from_port(FTDI)
    second = DeviceIdentity.from_port(replace(FTDI, serial_number="SECOND"))
    profile = Profile(name="lab").with_device(first).with_device(second)
    assert profile.matches(first) and profile.matches(second)


def test_no_identity_matches_nothing(store: ProfileStore) -> None:
    store.save(Profile(name="lab").with_device(DeviceIdentity.from_port(FTDI)))
    assert store.for_device(None) is None
    assert Profile(name="x").matches(None) is False


def test_a_weak_identity_still_binds() -> None:
    """Serial-less adapters (most CH340s) must still get their profile."""
    weak = DeviceIdentity.from_port(
        PortInfo(device="/dev/ttyUSB2", description="USB Serial", vid=0x1A86, pid=0x7523)
    )
    assert weak.weak is True
    assert Profile(name="ch340").with_device(weak).matches(weak)


# -- import / export -----------------------------------------------------


def test_export_and_import_round_trip(store: ProfileStore, tmp_path: Path) -> None:
    profile = rich_profile()
    exported = store.export(profile, tmp_path / "shared.toml")
    assert store.import_file(exported) == profile


def test_an_imported_file_without_a_name_uses_its_filename(
    store: ProfileStore, tmp_path: Path
) -> None:
    path = tmp_path / "from-a-colleague.toml"
    path.write_text("[serial]\nbaudrate = 19200\n")
    imported = store.import_file(path)
    assert imported.name == "from-a-colleague"
    assert imported.serial.baudrate == 19200


def test_exporting_to_an_unwritable_place_is_a_typed_error(
    store: ProfileStore, tmp_path: Path
) -> None:
    with pytest.raises(ProfileError, match="could not write"):
        store.export(Profile(name="x"), tmp_path / "missing" / "deep" / "x.toml")


def test_importing_a_missing_file_is_a_typed_error(store: ProfileStore, tmp_path: Path) -> None:
    with pytest.raises(ProfileError, match="could not read"):
        store.import_file(tmp_path / "not-here.toml")
