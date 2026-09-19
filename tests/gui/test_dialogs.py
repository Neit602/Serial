"""Settings and profiles dialogs (F3, F9-F16, F20)."""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path

import pytest
from pytestqt.qtbot import QtBot

from portside.core.breaks import BreakMode
from portside.core.identity import DeviceIdentity
from portside.core.pacing import PacePolicy
from portside.core.ports import PortInfo
from portside.core.settings import FlowControl, Parity, SerialSettings, StopBits
from portside.core.textio import Backspace, LineEnding, TextPolicy
from portside.emulation.decoder import CharacterSet
from portside.gui.profiles_dialog import ProfilesDialog
from portside.gui.settings_dialog import SettingsDialog
from portside.logging.session_log import LogFormat, LogSettings
from portside.logging.timestamps import TimestampMode
from portside.profiles.store import Profile, ProfileStore, TerminalOptions

pytestmark = pytest.mark.gui

FTDI = PortInfo(
    device="/dev/ttyUSB0",
    description="FT232R USB UART",
    vid=0x0403,
    pid=0x6001,
    serial_number="A50285BI",
)


def rich_profile() -> Profile:
    return Profile(
        name="cisco-lab",
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
            font_family="Courier New",
            font_size=14,
        ),
        pacing=PacePolicy(
            char_delay=0.01,
            line_delay=0.2,
            wait_for_prompt=r"[>#]\s*$",
            prompt_timeout=10.0,
            abort_on_timeout=False,
        ),
        logging=LogSettings(
            template="{port}-{date}.log",
            log_format=LogFormat.RAW,
            timestamps=TimestampMode.DELTA,
            auto_start=True,
            max_bytes=1024,
            keep=3,
        ),
        break_mode=BreakMode.EMULATED,
        break_duration=0.5,
    )


# -- settings dialog -----------------------------------------------------


def test_every_setting_round_trips(qtbot: QtBot) -> None:
    """The dialog must not quietly lose a setting it displayed."""
    original = rich_profile()
    dialog = SettingsDialog(original)
    qtbot.addWidget(dialog)
    assert dialog.to_profile() == original


def test_defaults_round_trip(qtbot: QtBot) -> None:
    original = Profile(name="plain")
    dialog = SettingsDialog(original)
    qtbot.addWidget(dialog)
    assert dialog.to_profile() == original


def test_a_custom_baud_rate_can_be_typed(qtbot: QtBot) -> None:
    """F3 allows any rate in range, not just the common ones."""
    dialog = SettingsDialog(Profile(name="x"))
    qtbot.addWidget(dialog)
    dialog.baud.setCurrentText("31250")  # MIDI
    assert dialog.to_profile().serial.baudrate == 31250


def test_an_out_of_range_baud_is_refused_at_the_dialog(qtbot: QtBot) -> None:
    """Better to refuse here than to fail at connect time."""
    dialog = SettingsDialog(Profile(name="x"))
    qtbot.addWidget(dialog)
    dialog.baud.setCurrentText("7")
    dialog._accept_if_valid()
    assert dialog.isVisible() is False or dialog.result() == 0
    assert "baud" in dialog.error_label.text().lower()


def test_an_invalid_prompt_regex_is_flagged_as_it_is_typed(qtbot: QtBot) -> None:
    dialog = SettingsDialog(Profile(name="x"))
    qtbot.addWidget(dialog)
    dialog.wait_for_prompt.setText("unclosed(")
    assert "Invalid prompt pattern" in dialog.error_label.text()
    dialog.wait_for_prompt.setText(r"[>#]\s*$")
    assert dialog.error_label.text() == ""


def test_the_filename_template_has_a_live_preview(qtbot: QtBot) -> None:
    """F15: a template is unreadable until you see what it produces."""
    dialog = SettingsDialog(Profile(name="lab"))
    qtbot.addWidget(dialog)
    dialog.log_template.setText("{port}-{date}.log")
    preview = dialog.template_preview.text()
    assert preview.startswith("lab-")
    assert preview.endswith(".log")


def test_all_five_parities_and_three_stop_bits_are_offered(qtbot: QtBot) -> None:
    dialog = SettingsDialog(Profile(name="x"))
    qtbot.addWidget(dialog)
    assert dialog.parity.count() == 5
    assert dialog.stop_bits.count() == 3
    assert dialog.flow.count() == 4


def test_every_charset_is_offered(qtbot: QtBot) -> None:
    dialog = SettingsDialog(Profile(name="x"))
    qtbot.addWidget(dialog)
    assert dialog.charset.count() == len(CharacterSet)


# -- profiles dialog -----------------------------------------------------


@pytest.fixture
def store(tmp_path: Path) -> ProfileStore:
    return ProfileStore(tmp_path / "profiles")


def test_profiles_are_listed(qtbot: QtBot, store: ProfileStore) -> None:
    store.save(Profile(name="lab"))
    store.save(Profile(name="bench"))
    dialog = ProfilesDialog(store)
    qtbot.addWidget(dialog)
    assert dialog.list.count() == 2


def test_an_empty_store_says_where_profiles_live(qtbot: QtBot, store: ProfileStore) -> None:
    dialog = ProfilesDialog(store)
    qtbot.addWidget(dialog)
    assert str(store.directory) in dialog.detail.text()


def test_binding_a_profile_to_the_connected_device(qtbot: QtBot, store: ProfileStore) -> None:
    """F2 + F20: the next connect for this adapter applies the profile."""
    store.save(Profile(name="lab"))
    identity = DeviceIdentity.from_port(FTDI)
    dialog = ProfilesDialog(store, identity=identity)
    qtbot.addWidget(dialog)
    dialog.list.setCurrentRow(0)
    dialog.bind_selected()
    assert store.for_device(identity) is not None
    assert store.load("lab").matches(identity)


def test_a_bound_profile_is_marked_in_the_list(qtbot: QtBot, store: ProfileStore) -> None:
    identity = DeviceIdentity.from_port(FTDI)
    store.save(Profile(name="lab").with_device(identity))
    dialog = ProfilesDialog(store, identity=identity)
    qtbot.addWidget(dialog)
    assert "this device" in dialog.list.item(0).text()


def test_export_and_import_round_trip(qtbot: QtBot, store: ProfileStore, tmp_path: Path) -> None:
    store.save(rich_profile())
    dialog = ProfilesDialog(store)
    qtbot.addWidget(dialog)
    dialog.list.setCurrentRow(0)
    exported = dialog.export_selected(tmp_path / "shared.toml")
    assert exported is not None and exported.is_file()

    store.delete("cisco-lab")
    dialog.refresh()
    imported = dialog.import_profile(exported)
    assert imported is not None
    assert store.load("cisco-lab") == rich_profile()


def test_importing_a_broken_file_reports_the_problem(
    qtbot: QtBot, store: ProfileStore, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from PySide6.QtWidgets import QMessageBox

    monkeypatch.setattr(QMessageBox, "warning", lambda *args, **kwargs: None)
    broken = tmp_path / "broken.toml"
    broken.write_text("not = valid = toml")
    dialog = ProfilesDialog(store)
    qtbot.addWidget(dialog)
    assert dialog.import_profile(broken) is None
    assert "not valid TOML" in dialog.last_error


def test_deleting_a_profile(
    qtbot: QtBot, store: ProfileStore, monkeypatch: pytest.MonkeyPatch
) -> None:
    from PySide6.QtWidgets import QMessageBox

    monkeypatch.setattr(
        QMessageBox, "question", lambda *args, **kwargs: QMessageBox.StandardButton.Yes
    )
    store.save(Profile(name="temp"))
    dialog = ProfilesDialog(store)
    qtbot.addWidget(dialog)
    dialog.list.setCurrentRow(0)
    dialog.delete_selected()
    assert store.names() == []


def test_duplicating_a_profile(
    qtbot: QtBot, store: ProfileStore, monkeypatch: pytest.MonkeyPatch
) -> None:
    from PySide6.QtWidgets import QInputDialog

    monkeypatch.setattr(QInputDialog, "getText", lambda *args, **kwargs: ("copy", True))
    store.save(replace(rich_profile(), device_keys=("usb_0403_6001_A50285BI",)))
    dialog = ProfilesDialog(store)
    qtbot.addWidget(dialog)
    dialog.list.setCurrentRow(0)
    dialog.duplicate_selected()
    assert sorted(store.names()) == ["cisco-lab", "copy"]
    # A duplicate must not steal the original's device binding.
    assert store.load("copy").device_keys == ()


def test_buttons_are_disabled_without_a_selection(qtbot: QtBot, store: ProfileStore) -> None:
    dialog = ProfilesDialog(store)
    qtbot.addWidget(dialog)
    assert dialog.edit_button.isEnabled() is False
    assert dialog.bind_button.isEnabled() is False
