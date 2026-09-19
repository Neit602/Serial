"""The app shell: tabs, windows, actions, errors, themes (F5, F6, F8, F22)."""

from __future__ import annotations

import time
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pytest
from PySide6.QtCore import QSettings
from PySide6.QtWidgets import QApplication, QMessageBox, QTreeWidgetItem
from pytestqt.qtbot import QtBot

from portside.core.errors import PermissionDeniedError, PortBusyError
from portside.gui.main_window import MainWindow
from portside.gui.palette import ThemeName
from portside.gui.session_view import SessionView
from portside.profiles.store import Profile, ProfileStore
from portside.transport.loopback import LoopbackTransport

pytestmark = pytest.mark.gui


@dataclass
class FakeComPort:
    """Stand-in for a discovered port."""

    device: str
    description: str | None = None
    vid: int | None = None
    pid: int | None = None
    serial_number: str | None = None
    manufacturer: str | None = None
    product: str | None = None
    location: str | None = None
    interface: str | None = None
    hwid: str | None = None


FTDI = FakeComPort(
    device="/dev/ttyUSB0",
    description="FT232R USB UART",
    vid=0x0403,
    pid=0x6001,
    serial_number="A50285BI",
)
CP2102 = FakeComPort(
    device="/dev/ttyUSB1",
    description="CP2102 USB to UART Bridge",
    vid=0x10C4,
    pid=0xEA60,
    serial_number="0001",
)


class ScriptedPorts:
    """A port list the test can change, to simulate hot-plug."""

    def __init__(self, *ports: FakeComPort) -> None:
        self.ports = list(ports)

    def __call__(self) -> list[FakeComPort]:
        return list(self.ports)


@pytest.fixture
def transports(monkeypatch: pytest.MonkeyPatch) -> dict[str, LoopbackTransport]:
    """Every session the window opens gets a loopback transport, not hardware."""
    made: dict[str, LoopbackTransport] = {}

    def fake_open_port(device: str, settings: Any, **kwargs: Any) -> LoopbackTransport:
        transport = LoopbackTransport(device, settings, echo=False)
        made[device] = transport
        return transport

    monkeypatch.setattr("portside.gui.main_window.open_port", fake_open_port)
    return made


@pytest.fixture
def window(
    qtbot: QtBot, tmp_path: Path, transports: dict[str, LoopbackTransport]
) -> Iterator[MainWindow]:
    settings = QSettings(str(tmp_path / "settings.ini"), QSettings.Format.IniFormat)
    made = MainWindow(
        store=ProfileStore(tmp_path / "profiles"),
        lister=ScriptedPorts(FTDI, CP2102),
        settings=settings,
    )
    qtbot.addWidget(made)
    made.show()
    try:
        yield made
    finally:
        made.close()


def _row(window: MainWindow, index: int) -> QTreeWidgetItem:
    """The device-list row at ``index``, asserted to exist."""
    item = window.device_list.tree.topLevelItem(index)
    assert item is not None, f"no device row at index {index}"
    return item


def _select(window: MainWindow, index: int) -> None:
    """Click a device-list row."""
    window.device_list.tree.setCurrentItem(_row(window, index))


def _connect_first(window: MainWindow) -> SessionView:
    _select(window, 0)
    view = window.connect_selected()
    assert view is not None
    return view


# -- the exit criterion, in the part a test can hold ---------------------


def test_a_new_user_reaches_a_connected_session_in_three_actions(
    qtbot: QtBot, window: MainWindow
) -> None:
    """PRD M3 exit criterion: connect in under 30 seconds, first time.

    The human half is docs/USABILITY-CHECK.md.  What is asserted here is the
    shape of the path: the device is already listed when the window opens, and
    reaching a connected terminal takes selecting it and activating it — no
    dialogs, no typing, no menu hunting.  If someone adds a step, this fails.
    """
    actions = 0

    assert window.device_list.tree.topLevelItemCount() == 2, "devices must be listed at startup"

    _select(window, 0)
    actions += 1  # click the device

    window.device_list.activate_selected()
    actions += 1  # press Connect (or double-click, which is the same action)

    assert actions <= 3
    view = window.current_view
    assert view is not None
    assert view.controller.is_connected is True
    assert view.view.hasFocus() or window.tabs.currentWidget() is view


def test_the_window_is_interactive_quickly(qtbot: QtBot, tmp_path: Path) -> None:
    """NFR: cold start under 2 s on a 2020-era laptop; measure our own part."""
    started = time.monotonic()
    made = MainWindow(
        store=ProfileStore(tmp_path / "profiles"),
        lister=ScriptedPorts(FTDI),
        settings=QSettings(str(tmp_path / "s.ini"), QSettings.Format.IniFormat),
    )
    qtbot.addWidget(made)
    made.show()
    elapsed = time.monotonic() - started
    try:
        assert elapsed < 2.0, f"window construction took {elapsed:.2f}s"
        assert made.device_list.tree.topLevelItemCount() == 1
    finally:
        made.close()


# -- device list (F1) ----------------------------------------------------


def test_devices_are_listed_with_their_details(window: MainWindow) -> None:
    item = _row(window, 0)
    assert item.text(0) == "/dev/ttyUSB0"
    assert "FT232R" in item.text(1)
    assert item.text(2) == "0403:6001"
    assert item.text(3) == "A50285BI"


def test_hot_plug_adds_and_removes_rows(window: MainWindow) -> None:
    """F1: the list must follow what is plugged in, within 2 s."""
    lister = window.device_list._lister
    assert isinstance(lister, ScriptedPorts)
    lister.ports.append(FakeComPort(device="/dev/ttyUSB2", vid=0x1A86, pid=0x7523))
    window.device_list.refresh()
    assert window.device_list.tree.topLevelItemCount() == 3

    lister.ports.clear()
    window.device_list.refresh()
    assert window.device_list.tree.topLevelItemCount() == 0
    assert window.device_list.empty_label.isVisible()


def test_the_empty_state_explains_what_to_check(qtbot: QtBot, tmp_path: Path) -> None:
    made = MainWindow(
        store=ProfileStore(tmp_path / "p"),
        lister=ScriptedPorts(),
        settings=QSettings(str(tmp_path / "s.ini"), QSettings.Format.IniFormat),
    )
    qtbot.addWidget(made)
    try:
        assert "driver" in made.device_list.empty_label.text()
        assert "permission" in made.device_list.empty_label.text()
    finally:
        made.close()


# -- tabs and windows (F22) ---------------------------------------------


def test_connecting_opens_a_tab(window: MainWindow) -> None:
    view = _connect_first(window)
    assert window.tabs.count() == 1
    assert "ttyUSB0" in window.tabs.tabText(0)
    assert view.controller.is_connected


def test_several_sessions_run_independently(
    window: MainWindow, transports: dict[str, LoopbackTransport]
) -> None:
    first = _connect_first(window)
    _select(window, 1)
    second = window.connect_selected()
    assert second is not None
    assert window.tabs.count() == 2

    first.controller.send(b"one").result(timeout=5)
    second.controller.send(b"two").result(timeout=5)
    assert bytes(transports["/dev/ttyUSB0"].written) == b"one"
    assert bytes(transports["/dev/ttyUSB1"].written) == b"two"


def test_closing_one_tab_leaves_the_others_connected(
    window: MainWindow, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(QMessageBox, "question", lambda *a, **k: QMessageBox.StandardButton.Yes)
    first = _connect_first(window)
    _select(window, 1)
    second = window.connect_selected()
    assert second is not None

    window.close_tab(0)
    assert window.tabs.count() == 1
    assert first.controller.is_connected is False
    assert second.controller.is_connected is True


def test_closing_a_connected_tab_asks_first(
    window: MainWindow, monkeypatch: pytest.MonkeyPatch
) -> None:
    asked: list[str] = []

    def question(*args: object, **kwargs: object) -> QMessageBox.StandardButton:
        asked.append("asked")
        return QMessageBox.StandardButton.No

    monkeypatch.setattr(QMessageBox, "question", question)
    _connect_first(window)
    window.close_tab(0)
    assert asked, "closing a live session must confirm"
    assert window.tabs.count() == 1, "answering No keeps the session"


def test_detaching_a_tab_keeps_the_session_alive(
    qtbot: QtBot, window: MainWindow, transports: dict[str, LoopbackTransport]
) -> None:
    """F22: a detached window is the same session, not a reconnect."""
    view = _connect_first(window)
    view.controller.send(b"before").result(timeout=5)
    opens_before = transports["/dev/ttyUSB0"].open_count

    detached = window.detach_current_tab()
    assert detached is not None
    qtbot.addWidget(detached)
    try:
        assert window.tabs.count() == 0
        assert detached.tabs.count() == 1
        assert view.controller.is_connected is True
        assert transports["/dev/ttyUSB0"].open_count == opens_before, "must not reopen the port"
        view.controller.send(b"after").result(timeout=5)
        assert bytes(transports["/dev/ttyUSB0"].written) == b"beforeafter"
    finally:
        detached.close()


def test_a_new_window_shares_the_runtime(qtbot: QtBot, window: MainWindow) -> None:
    second = window.open_new_window()
    qtbot.addWidget(second)
    try:
        assert second.runtime is window.runtime
    finally:
        second.close()


def test_the_status_bar_shows_the_session(window: MainWindow, qtbot: QtBot) -> None:
    view = _connect_first(window)
    view.controller.send(b"hello").result(timeout=5)
    qtbot.waitUntil(lambda: "out 5 B" in view.status_text(), timeout=5000)
    assert "/dev/ttyUSB0" in view.status_text()
    assert "115200 8N1" in view.status_text()


# -- session actions -----------------------------------------------------


def test_break_is_sent_from_the_toolbar(
    window: MainWindow, transports: dict[str, LoopbackTransport], qtbot: QtBot
) -> None:
    _connect_first(window)
    window.send_break()
    qtbot.waitUntil(lambda: bool(transports["/dev/ttyUSB0"].breaks), timeout=5000)
    assert list(transports["/dev/ttyUSB0"].breaks) == [0.25]


def test_logging_can_be_toggled(window: MainWindow, tmp_path: Path) -> None:
    view = _connect_first(window)
    from dataclasses import replace

    logger = view.controller.logger
    assert logger is not None
    logger.settings = replace(logger.settings, directory=tmp_path, flush_interval=0.0)
    window.toggle_logging()
    assert view.controller.is_logging is True
    assert window.log_action.isChecked() is True
    window.toggle_logging()
    assert view.controller.is_logging is False


def test_hex_view_can_be_toggled(window: MainWindow) -> None:
    from portside.emulation.hexview import ViewMode

    view = _connect_first(window)
    window.toggle_hex()
    assert view.view.view_mode is ViewMode.HEX
    assert window.hex_action.isChecked() is True


def test_paste_file_sends_the_file(
    window: MainWindow, transports: dict[str, LoopbackTransport], qtbot: QtBot, tmp_path: Path
) -> None:
    view = _connect_first(window)
    from portside.core.pacing import PacePolicy

    view.controller.pace = PacePolicy(line_delay=0.0)
    config = tmp_path / "switch.cfg"
    config.write_text("hostname lab-sw1\nend\n")
    window.paste_file(config)
    qtbot.waitUntil(
        lambda: bytes(transports["/dev/ttyUSB0"].written) == b"hostname lab-sw1\rend\r",
        timeout=10_000,
    )


def test_clear_empties_the_terminal(window: MainWindow) -> None:
    view = _connect_first(window)
    view.view.feed(b"noise\r\n")
    window.clear_terminal()
    assert all(not line for line in view.terminal.snapshot().text())


def test_find_opens_the_find_bar(window: MainWindow) -> None:
    view = _connect_first(window)
    window.open_find()
    assert view.find_bar.isVisible()


def test_copy_uses_the_clipboard(window: MainWindow) -> None:
    view = _connect_first(window)
    view.view.feed(b"copy me\r\n")
    view.view._selection_anchor = view.view._cell_at(view.view.rect().topLeft())
    from portside.gui.terminal_widget import CellPosition

    view.view._selection_head = CellPosition(0, 6)
    assert window.copy_selection() == "copy me"
    clipboard = QApplication.clipboard()
    assert clipboard is not None
    assert clipboard.text() == "copy me"


# -- errors (F6) ---------------------------------------------------------


def test_an_error_dialog_shows_the_remedy_verbatim(
    window: MainWindow, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The entire point of the typed-error design."""
    monkeypatch.setattr(QMessageBox, "exec", lambda self: QMessageBox.StandardButton.Close)
    error = PermissionDeniedError(
        "permission denied opening /dev/ttyUSB0",
        port="/dev/ttyUSB0",
        remedy="Add yourself to the 'dialout' group:\n\n    sudo usermod -aG dialout $USER\n",
    )
    window.show_error(error)
    assert "sudo usermod -aG dialout $USER" in window.last_message


def test_the_fix_can_be_copied_to_the_clipboard(window: MainWindow) -> None:
    """A command you cannot paste is only half a remedy."""
    window._copy_remedy("Add yourself:\n\n    sudo usermod -aG dialout $USER\n\nthen log out.")
    clipboard = QApplication.clipboard()
    assert clipboard is not None
    assert clipboard.text() == "sudo usermod -aG dialout $USER"


def test_a_failing_connect_reports_the_error(
    window: MainWindow, monkeypatch: pytest.MonkeyPatch
) -> None:
    shown: list[object] = []
    monkeypatch.setattr(MainWindow, "show_error", lambda self, error: shown.append(error))

    def refuse(device: str, settings: object, **kwargs: object) -> LoopbackTransport:
        return LoopbackTransport(
            device,
            fail_open_with=PortBusyError("busy", port=device, remedy="close the other app"),
        )

    monkeypatch.setattr("portside.gui.main_window.open_port", refuse)
    _select(window, 0)
    assert window.connect_selected() is None
    assert shown
    assert isinstance(shown[0], PortBusyError)
    assert window.tabs.count() == 0


# -- profiles (F20) ------------------------------------------------------


def test_a_bound_profile_is_applied_on_connect(window: MainWindow) -> None:
    """F2 + F20: plug in a known adapter, get its settings."""
    from portside.core.identity import DeviceIdentity
    from portside.core.ports import PortInfo
    from portside.core.settings import SerialSettings

    identity = DeviceIdentity.from_port(
        PortInfo(device="/dev/ttyUSB0", vid=0x0403, pid=0x6001, serial_number="A50285BI")
    )
    window.store.save(
        Profile(name="lab", serial=SerialSettings(baudrate=9600)).with_device(identity)
    )
    view = _connect_first(window)
    assert view.controller.settings.baudrate == 9600
    assert view.controller.profile is not None
    assert view.controller.profile.name == "lab"


def test_a_session_can_be_saved_as_a_profile(window: MainWindow) -> None:
    _connect_first(window)
    profile = window.save_session_as_profile("bench")
    assert profile is not None
    saved = window.store.load("bench")
    assert saved.serial.baudrate == 115200
    assert saved.device_keys, "saving from a session should bind the device"


# -- themes and accessibility -------------------------------------------


def theme_of(view: SessionView) -> ThemeName:
    """Read through a call so one assert does not narrow the next."""
    return view.view.palette_name


def test_themes_apply_to_chrome_and_terminal(window: MainWindow) -> None:
    view = _connect_first(window)
    window.apply_theme(ThemeName.LIGHT)
    assert theme_of(view) is ThemeName.LIGHT
    app = QApplication.instance()
    assert isinstance(app, QApplication)
    assert "background-color" in app.styleSheet()
    window.apply_theme(ThemeName.HIGH_CONTRAST)
    assert theme_of(view) is ThemeName.HIGH_CONTRAST


def test_the_theme_is_remembered(qtbot: QtBot, window: MainWindow, tmp_path: Path) -> None:
    window.apply_theme(ThemeName.LIGHT)
    assert window.settings.value("theme") == ThemeName.LIGHT.value


def test_every_toolbar_action_has_a_shortcut(window: MainWindow) -> None:
    """NFR accessibility: the whole app must be reachable from the keyboard."""
    missing = [
        action.iconText() for action in window.toolbar.actions() if action.shortcut().isEmpty()
    ]
    assert not missing, f"toolbar actions without shortcuts: {missing}"


def test_interactive_widgets_have_accessible_names(window: MainWindow) -> None:
    view = _connect_first(window)
    for widget in (
        window.tabs,
        window.device_list.tree,
        view.view,
        view.signals_panel.dtr,
        view.signals_panel.rts,
        view.find_input,
    ):
        assert widget.accessibleName(), f"{type(widget).__name__} has no accessible name"


def test_actions_are_disabled_without_a_session(window: MainWindow) -> None:
    assert window.break_action.isEnabled() is False
    assert window.log_action.isEnabled() is False
    _connect_first(window)
    assert window.break_action.isEnabled() is True


# -- shutdown ------------------------------------------------------------


def test_closing_the_window_closes_every_session(
    qtbot: QtBot, tmp_path: Path, transports: dict[str, LoopbackTransport]
) -> None:
    made = MainWindow(
        store=ProfileStore(tmp_path / "p"),
        lister=ScriptedPorts(FTDI, CP2102),
        settings=QSettings(str(tmp_path / "s.ini"), QSettings.Format.IniFormat),
    )
    qtbot.addWidget(made)
    made.show()
    made.device_list.tree.setCurrentItem(made.device_list.tree.topLevelItem(0) or QTreeWidgetItem())
    view = made.connect_selected()
    assert view is not None
    made.close()
    assert view.controller.is_connected is False
    assert made.runtime.running is False


def test_no_threads_are_leaked_across_windows(qtbot: QtBot, tmp_path: Path) -> None:
    import threading

    before = {thread.name for thread in threading.enumerate()}
    for index in range(3):
        made = MainWindow(
            store=ProfileStore(tmp_path / "p"),
            lister=ScriptedPorts(),
            settings=QSettings(str(tmp_path / f"s{index}.ini"), QSettings.Format.IniFormat),
        )
        qtbot.addWidget(made)
        made.close()
    time.sleep(0.3)
    after = {thread.name for thread in threading.enumerate()}
    assert not {name for name in after - before if "portside" in name}
