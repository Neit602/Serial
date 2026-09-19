"""The Portside main window (PRD F22, and the app shell for M3).

Tabs of sessions, a device list, the toolbar and menus.  The window owns the
:class:`~portside.gui.runtime.AsyncRuntime` that all its sessions share, and
closes every one of them on the way out.
"""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path

from PySide6.QtCore import QSettings, Qt, Signal
from PySide6.QtGui import QAction, QCloseEvent, QDragEnterEvent, QDropEvent, QKeySequence
from PySide6.QtWidgets import (
    QApplication,
    QDialog,
    QDockWidget,
    QFileDialog,
    QMainWindow,
    QMessageBox,
    QTabWidget,
    QWidget,
)

from portside.core.errors import PortsideError
from portside.core.identity import DeviceIdentity
from portside.core.ports import PortInfo
from portside.core.session import ReconnectPolicy, Session
from portside.gui.controller import SessionController
from portside.gui.device_list import DeviceList
from portside.gui.palette import ThemeName
from portside.gui.profiles_dialog import ProfilesDialog
from portside.gui.runtime import AsyncRuntime
from portside.gui.session_view import SessionView
from portside.gui.settings_dialog import SettingsDialog
from portside.gui.styles import stylesheet
from portside.logging.session_log import SessionLogger
from portside.profiles.store import Profile, ProfileStore
from portside.transport.discovery import ComPortLister
from portside.transport.factory import identity_reopener, open_port

__all__ = ["MainWindow"]

ORGANISATION = "Portside"
APPLICATION = "Portside"


class MainWindow(QMainWindow):
    """One window; any number of session tabs."""

    #: Emitted whenever a session is opened, for tests and for future plugins.
    sessionOpened = Signal(object)  # SessionView

    def __init__(
        self,
        *,
        runtime: AsyncRuntime | None = None,
        store: ProfileStore | None = None,
        lister: ComPortLister | None = None,
        settings: QSettings | None = None,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("Portside")
        self.runtime = runtime or AsyncRuntime()
        self.runtime.start()
        self._owns_runtime = runtime is None
        self.store = store or ProfileStore()
        self.settings = settings or QSettings(ORGANISATION, APPLICATION)
        self.theme = ThemeName(str(self.settings.value("theme", ThemeName.DARK.value)))
        self.last_message = ""

        self.tabs = QTabWidget(self)
        self.tabs.setTabsClosable(True)
        self.tabs.setMovable(True)
        self.tabs.setDocumentMode(True)
        self.tabs.tabCloseRequested.connect(self.close_tab)
        self.tabs.currentChanged.connect(lambda _index: self._update_actions())
        self.tabs.setAccessibleName(self.tr("Sessions"))
        self.setCentralWidget(self.tabs)

        self.device_list = DeviceList(lister=lister, parent=self)
        self.device_list.portActivated.connect(self.connect_to_port)
        self.device_list.portSelected.connect(lambda _port: self._update_actions())
        dock = QDockWidget(self.tr("Devices"), self)
        dock.setObjectName("devices")
        dock.setWidget(self.device_list)
        dock.setAllowedAreas(
            Qt.DockWidgetArea.LeftDockWidgetArea | Qt.DockWidgetArea.RightDockWidgetArea
        )
        self.addDockWidget(Qt.DockWidgetArea.LeftDockWidgetArea, dock)
        self.devices_dock = dock

        self._build_actions()
        self._build_menus()
        self._build_toolbar()
        self.statusBar().showMessage(self.tr("Ready. Pick a device and press Connect."))
        self.setAcceptDrops(True)
        self.apply_theme(self.theme)
        self._restore_geometry()
        self._update_actions()

    # -- actions and menus -------------------------------------------------

    def _action(self, text: str, slot: object, shortcut: str = "", *, tip: str = "") -> QAction:
        action = QAction(text, self)
        action.triggered.connect(slot)
        if shortcut:
            action.setShortcut(QKeySequence(shortcut))
        action.setToolTip(tip or text.replace("&", ""))
        # Every control needs a name a screen reader can read out.
        action.setIconText(text.replace("&", ""))
        return action

    def _build_actions(self) -> None:
        self.connect_action = self._action(
            self.tr("&Connect"),
            self.connect_selected,
            "Ctrl+N",
            tip=self.tr("Open a session for the selected device"),
        )
        self.connect_with_settings_action = self._action(
            self.tr("Connect &with settings…"), self.connect_with_settings, "Ctrl+Shift+N"
        )
        self.disconnect_action = self._action(
            self.tr("&Disconnect"), self.close_current_tab, "Ctrl+W"
        )
        self.new_window_action = self._action(
            self.tr("New &Window"), self.open_new_window, "Ctrl+Shift+W"
        )
        self.detach_action = self._action(self.tr("&Detach tab to window"), self.detach_current_tab)
        self.settings_action = self._action(
            self.tr("&Session settings…"), self.edit_session_settings, "Ctrl+,"
        )
        self.profiles_action = self._action(self.tr("&Profiles…"), self.open_profiles, "Ctrl+P")
        self.save_profile_action = self._action(
            self.tr("Save session as profile…"), self.save_session_as_profile
        )
        self.copy_action = self._action(self.tr("&Copy"), self.copy_selection, "Ctrl+Shift+C")
        self.paste_file_action = self._action(
            self.tr("Paste &file…"), self.paste_file, "Ctrl+Shift+V"
        )
        self.find_action = self._action(self.tr("&Find…"), self.open_find, "Ctrl+F")
        self.break_action = self._action(self.tr("Send &break"), self.send_break, "Ctrl+B")
        self.log_action = self._action(self.tr("&Logging"), self.toggle_logging, "Ctrl+L")
        self.log_action.setCheckable(True)
        self.hex_action = self._action(self.tr("&Hex view"), self.toggle_hex, "Ctrl+H")
        self.hex_action.setCheckable(True)
        self.clear_action = self._action(self.tr("C&lear"), self.clear_terminal, "Ctrl+K")
        self.quit_action = self._action(self.tr("&Quit"), self.close, "Ctrl+Q")

        self.theme_actions: dict[ThemeName, QAction] = {}
        for name, label in (
            (ThemeName.DARK, self.tr("&Dark")),
            (ThemeName.LIGHT, self.tr("&Light")),
            (ThemeName.HIGH_CONTRAST, self.tr("&High contrast")),
        ):
            action = self._action(label, lambda _checked=False, n=name: self.apply_theme(n))
            action.setCheckable(True)
            self.theme_actions[name] = action

    def _build_menus(self) -> None:
        bar = self.menuBar()
        file_menu = bar.addMenu(self.tr("&File"))
        file_menu.addAction(self.connect_action)
        file_menu.addAction(self.connect_with_settings_action)
        file_menu.addAction(self.disconnect_action)
        file_menu.addSeparator()
        file_menu.addAction(self.new_window_action)
        file_menu.addAction(self.detach_action)
        file_menu.addSeparator()
        file_menu.addAction(self.quit_action)

        edit_menu = bar.addMenu(self.tr("&Edit"))
        edit_menu.addAction(self.copy_action)
        edit_menu.addAction(self.paste_file_action)
        edit_menu.addAction(self.find_action)
        edit_menu.addSeparator()
        edit_menu.addAction(self.settings_action)
        edit_menu.addAction(self.profiles_action)
        edit_menu.addAction(self.save_profile_action)

        session_menu = bar.addMenu(self.tr("&Session"))
        session_menu.addAction(self.break_action)
        session_menu.addAction(self.log_action)
        session_menu.addAction(self.hex_action)
        session_menu.addAction(self.clear_action)

        view_menu = bar.addMenu(self.tr("&View"))
        theme_menu = view_menu.addMenu(self.tr("&Theme"))
        for action in self.theme_actions.values():
            theme_menu.addAction(action)
        view_menu.addAction(self.devices_dock.toggleViewAction())

        help_menu = bar.addMenu(self.tr("&Help"))
        help_menu.addAction(self._action(self.tr("&Shortcuts"), self.show_shortcuts, "F1"))
        help_menu.addAction(self._action(self.tr("&About Portside"), self.show_about))

    def _build_toolbar(self) -> None:
        bar = self.addToolBar(self.tr("Main"))
        bar.setObjectName("main-toolbar")
        bar.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextOnly)
        for action in (
            self.connect_action,
            self.disconnect_action,
            self.break_action,
            self.paste_file_action,
            self.log_action,
            self.hex_action,
            self.find_action,
        ):
            bar.addAction(action)
        self.toolbar = bar

    # -- sessions ----------------------------------------------------------

    @property
    def current_view(self) -> SessionView | None:
        """The session in the active tab, if any."""
        widget = self.tabs.currentWidget()
        return widget if isinstance(widget, SessionView) else None

    @property
    def views(self) -> list[SessionView]:
        """Every open session view."""
        return [
            widget
            for index in range(self.tabs.count())
            if isinstance(widget := self.tabs.widget(index), SessionView)
        ]

    def connect_selected(self) -> SessionView | None:
        """Connect to the highlighted device (the Connect button)."""
        port = self.device_list.selected_port()
        if port is None:
            self._notify(self.tr("Select a device in the list first."))
            return None
        return self.connect_to_port(port)

    def connect_to_port(self, port: PortInfo, profile: Profile | None = None) -> SessionView | None:
        """Open a session for ``port``, applying its profile (F1, F2, F20)."""
        # The device list already holds everything discovery knows about this
        # port, so its identity comes from there: re-resolving would cost a
        # second scan and would lose the identity for ports that enumeration
        # does not list (RFCOMM nodes, PTYs - see DECISIONS.md D5).
        identity = DeviceIdentity.from_port(port)
        chosen = profile or self.store.for_device(identity) or Profile(name=port.device)
        device = port.device
        try:
            transport = open_port(device, chosen.serial)
            # Reconnect by identity, so the adapter is found again even if it
            # comes back under a different port name (F2 + F5).
            reopen = identity_reopener(identity, chosen.serial)
            session = Session(
                transport,
                settings=chosen.serial,
                identity=identity,
                reconnect=ReconnectPolicy(enabled=True),
                reopen=reopen,
                break_mode=chosen.break_mode,
            )
            self.runtime.run(session.open())
        except PortsideError as error:
            self.show_error(error)
            return None

        logger = SessionLogger(settings=chosen.logging, port=device, profile=chosen.name)
        controller = SessionController(
            session, self.runtime, profile=chosen, logger=logger, parent=self
        )
        if chosen.logging.auto_start:
            controller.start_logging()
        view = SessionView(controller, theme=self.theme, parent=self)
        view.statusChanged.connect(self._on_status)
        view.errorRaised.connect(self.show_error)
        index = self.tabs.addTab(view, self._tab_title(device, chosen))
        self.tabs.setCurrentIndex(index)
        view.view.setFocus()
        self._update_actions()
        self.sessionOpened.emit(view)
        return view

    @staticmethod
    def _tab_title(device: str, profile: Profile) -> str:
        name = Path(device).name or device
        return f"{name} — {profile.serial.baudrate}"

    def connect_with_settings(self) -> SessionView | None:
        """Ask for settings first, then connect (F3)."""
        port = self.device_list.selected_port()
        if port is None:
            self._notify(self.tr("Select a device in the list first."))
            return None
        base = self.store.for_device(DeviceIdentity.from_port(port)) or Profile(name=port.device)
        dialog = SettingsDialog(base, self)
        if dialog.exec() != QDialog.DialogCode.Accepted:
            return None
        return self.connect_to_port(port, dialog.to_profile())

    def close_tab(self, index: int) -> None:
        """Close one tab, confirming if its session is still connected."""
        widget = self.tabs.widget(index)
        if not isinstance(widget, SessionView):
            return
        if widget.controller.is_connected:
            answer = QMessageBox.question(
                self,
                self.tr("Close session"),
                self.tr("{0} is still connected. Close it?").format(widget.controller.port),
            )
            if answer != QMessageBox.StandardButton.Yes:
                return
        self.tabs.removeTab(index)
        widget.close_session()
        widget.deleteLater()
        self._update_actions()

    def close_current_tab(self) -> None:
        """Close the active tab."""
        if self.tabs.currentIndex() >= 0:
            self.close_tab(self.tabs.currentIndex())

    def detach_current_tab(self) -> MainWindow | None:
        """Move the active tab into its own window (F22)."""
        view = self.current_view
        if view is None:
            return None
        index = self.tabs.indexOf(view)
        title = self.tabs.tabText(index)
        self.tabs.removeTab(index)
        window = MainWindow(
            runtime=self.runtime, store=self.store, settings=self.settings, parent=None
        )
        window.adopt(view, title)
        window.show()
        self._update_actions()
        return window

    def adopt(self, view: SessionView, title: str) -> None:
        """Take over a session view from another window (F22)."""
        view.setParent(self.tabs)
        view.statusChanged.connect(self._on_status)
        view.errorRaised.connect(self.show_error)
        index = self.tabs.addTab(view, title)
        self.tabs.setCurrentIndex(index)
        self._update_actions()

    def open_new_window(self) -> MainWindow:
        """Open a second empty window sharing this one's runtime (F22)."""
        window = MainWindow(runtime=self.runtime, store=self.store, settings=self.settings)
        window.show()
        return window

    # -- session actions ---------------------------------------------------

    def send_break(self) -> None:
        """Send a break on the active session (F12)."""
        view = self.current_view
        if view is None:
            return
        future = view.controller.send_break(
            view.controller.profile.break_duration if view.controller.profile else 0.25
        )

        def report(done: object) -> None:
            try:
                result = done.result(timeout=0)  # type: ignore[attr-defined]
            except PortsideError as error:
                self.show_error(error)
                return
            except Exception:
                return
            self._notify(result.describe())

        future.add_done_callback(report)

    def toggle_logging(self) -> None:
        """Start or stop the session log (F15)."""
        view = self.current_view
        if view is None:
            return
        if view.controller.is_logging:
            view.controller.stop_logging()
            self._notify(self.tr("Logging stopped."))
        else:
            path = view.controller.start_logging()
            self._notify(self.tr("Logging to {0}").format(path) if path else "")
        self._update_actions()

    def toggle_hex(self) -> None:
        """Switch the active session between text and hex (F10)."""
        view = self.current_view
        if view is not None:
            view.toggle_hex()
            self._update_actions()

    def clear_terminal(self) -> None:
        """Clear the active session's screen and scrollback."""
        view = self.current_view
        if view is not None:
            view.clear()

    def copy_selection(self) -> str:
        """Copy the terminal selection to the clipboard (F8)."""
        view = self.current_view
        return view.copy_selection() if view is not None else ""

    def open_find(self) -> None:
        """Show the find bar (F8)."""
        view = self.current_view
        if view is not None:
            view.open_find()

    def paste_file(self, path: Path | None = None) -> None:
        """Paste a file into the active session at its configured pace (F13)."""
        view = self.current_view
        if view is None:
            return
        target = path
        if target is None:  # pragma: no cover - native dialog
            chosen, _ = QFileDialog.getOpenFileName(self, self.tr("Paste file"))
            if not chosen:
                return
            target = Path(chosen)
        view.paste_file(target)

    def edit_session_settings(self) -> None:
        """Change the active session's settings, live (F3)."""
        view = self.current_view
        if view is None:
            return
        profile = view.controller.profile or Profile(name=view.controller.port)
        dialog = SettingsDialog(profile, self)
        if dialog.exec() != QDialog.DialogCode.Accepted:
            return
        updated = dialog.to_profile()
        view.controller.profile = updated
        view.controller.text_policy = updated.text
        view.controller.pace = updated.pacing
        view.controller.apply_settings(updated.serial)
        view.view.set_font(updated.terminal.font_family, updated.terminal.font_size)
        self._notify(self.tr("Settings applied."))

    def open_profiles(self) -> ProfilesDialog:
        """Open the profiles manager (F20)."""
        view = self.current_view
        dialog = ProfilesDialog(
            self.store,
            identity=view.controller.identity if view is not None else None,
            parent=self,
        )
        dialog.exec()
        return dialog

    def save_session_as_profile(self, name: str | None = None) -> Profile | None:
        """Save the active session's settings as a named profile (F20)."""
        view = self.current_view
        if view is None:
            return None
        chosen = name
        if chosen is None:  # pragma: no cover - native dialog
            from PySide6.QtWidgets import QInputDialog

            chosen, ok = QInputDialog.getText(
                self, self.tr("Save profile"), self.tr("Profile name:"), text=view.controller.port
            )
            if not ok or not chosen.strip():
                return None
        base = view.controller.profile or Profile(name=chosen)
        profile = replace(base, name=chosen, serial=view.controller.settings)
        identity = view.controller.identity
        if identity is not None:
            profile = profile.with_device(identity)
        self.store.save(profile)
        self._notify(self.tr("Saved profile {0}").format(chosen))
        return profile

    # -- appearance --------------------------------------------------------

    def apply_theme(self, name: ThemeName) -> None:
        """Switch theme for the chrome and every open terminal (F8)."""
        self.theme = name
        app = QApplication.instance()
        if app is not None:
            app.setStyleSheet(stylesheet(name))  # type: ignore[attr-defined]
        for view in self.views:
            view.set_theme(name)
        for theme_name, action in self.theme_actions.items():
            action.setChecked(theme_name is name)
        self.settings.setValue("theme", name.value)

    # -- errors and status -------------------------------------------------

    def show_error(self, error: PortsideError) -> None:
        """Show a typed error with its remedy, verbatim (F6)."""
        self.last_message = error.display()
        box = QMessageBox(self)
        box.setIcon(QMessageBox.Icon.Warning)
        box.setWindowTitle(self.tr("Serial port problem"))
        box.setText(str(error))
        if error.remedy:
            box.setInformativeText(error.remedy)
            copy_button = box.addButton(self.tr("Copy fix"), QMessageBox.ButtonRole.ActionRole)
            copy_button.clicked.connect(lambda: self._copy_remedy(error.remedy or ""))
        box.addButton(QMessageBox.StandardButton.Close)
        box.exec()

    @staticmethod
    def _copy_remedy(remedy: str) -> None:
        """Put the fix on the clipboard so it can be pasted into a shell."""
        clipboard = QApplication.clipboard()
        if clipboard is not None:
            # The command lines are the indented lines of the remedy text.
            commands = [line.strip() for line in remedy.splitlines() if line.startswith("    ")]
            clipboard.setText("\n".join(commands) if commands else remedy)

    def _notify(self, message: str) -> None:
        self.last_message = message
        if message:
            self.statusBar().showMessage(message, 8000)

    def _on_status(self, text: str) -> None:
        sender = self.sender()
        if sender is self.current_view:
            self.statusBar().showMessage(text)

    def _update_actions(self) -> None:
        view = self.current_view
        connected = view is not None
        for action in (
            self.disconnect_action,
            self.detach_action,
            self.settings_action,
            self.save_profile_action,
            self.copy_action,
            self.paste_file_action,
            self.find_action,
            self.break_action,
            self.log_action,
            self.hex_action,
            self.clear_action,
        ):
            action.setEnabled(connected)
        self.connect_action.setEnabled(self.device_list.selected_port() is not None)
        if view is not None:
            self.log_action.setChecked(view.controller.is_logging)
            from portside.emulation.hexview import ViewMode

            self.hex_action.setChecked(view.view.view_mode is ViewMode.HEX)

    def show_shortcuts(self) -> None:
        """List every shortcut, because a GUI should not need a manual."""
        lines = [
            f"{action.shortcut().toString()}\t{action.iconText()}"
            for action in self.findChildren(QAction)
            if not action.shortcut().isEmpty()
        ]
        self.last_message = "\n".join(sorted(lines))
        QMessageBox.information(self, self.tr("Keyboard shortcuts"), self.last_message)

    def show_about(self) -> None:
        """About box."""
        from portside import __version__

        self.last_message = self.tr(
            "Portside {0}\nA cross-platform serial terminal.\nMIT licensed."
        ).format(__version__)
        QMessageBox.about(self, self.tr("About Portside"), self.last_message)

    # -- drag and drop (F13's file half) -----------------------------------

    def dragEnterEvent(self, event: QDragEnterEvent) -> None:
        if event.mimeData().hasUrls() and self.current_view is not None:
            event.acceptProposedAction()

    def dropEvent(self, event: QDropEvent) -> None:
        """Dropping a file pastes it at the session's pace."""
        urls = event.mimeData().urls()
        if not urls:
            return
        path = Path(urls[0].toLocalFile())
        if path.is_file():
            self.paste_file(path)
            event.acceptProposedAction()

    # -- geometry and shutdown --------------------------------------------

    def _restore_geometry(self) -> None:
        geometry = self.settings.value("geometry")
        if isinstance(geometry, (bytes, bytearray)):
            self.restoreGeometry(geometry)
        state = self.settings.value("windowState")
        if isinstance(state, (bytes, bytearray)):
            self.restoreState(state)

    def closeEvent(self, event: QCloseEvent) -> None:
        """Close every session and stop the runtime this window owns."""
        self.settings.setValue("geometry", self.saveGeometry())
        self.settings.setValue("windowState", self.saveState())
        self.device_list.stop()
        for view in self.views:
            view.close_session()
        self.tabs.clear()
        if self._owns_runtime:
            self.runtime.stop()
        super().closeEvent(event)
