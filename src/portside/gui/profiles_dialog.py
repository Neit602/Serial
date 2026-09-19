"""Managing saved profiles (PRD F20, F2).

The list, the bind-to-device action and import/export — the GUI half of the
profile store M2 built.
"""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from portside.core.identity import DeviceIdentity
from portside.profiles.store import Profile, ProfileError, ProfileStore

__all__ = ["ProfilesDialog"]


class ProfilesDialog(QDialog):
    """List, edit, bind, import and export profiles."""

    def __init__(
        self,
        store: ProfileStore,
        *,
        identity: DeviceIdentity | None = None,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle(self.tr("Profiles"))
        self.store = store
        self.identity = identity
        self.last_error = ""

        layout = QVBoxLayout(self)
        self.list = QListWidget(self)
        self.list.setAccessibleName(self.tr("Saved profiles"))
        self.list.currentItemChanged.connect(lambda *_: self._update_buttons())
        layout.addWidget(self.list)

        self.detail = QLabel("", self)
        self.detail.setWordWrap(True)
        layout.addWidget(self.detail)

        buttons = QHBoxLayout()
        self.edit_button = self._button(buttons, self.tr("&Edit…"), self.edit_selected)
        self.bind_button = self._button(buttons, self.tr("&Bind to device"), self.bind_selected)
        self.duplicate_button = self._button(
            buttons, self.tr("D&uplicate"), self.duplicate_selected
        )
        self.delete_button = self._button(buttons, self.tr("&Delete"), self.delete_selected)
        self.export_button = self._button(buttons, self.tr("E&xport…"), self.export_selected)
        self.import_button = self._button(buttons, self.tr("&Import…"), self.import_profile)
        layout.addLayout(buttons)

        close = QDialogButtonBox(QDialogButtonBox.StandardButton.Close, parent=self)
        close.rejected.connect(self.reject)
        close.accepted.connect(self.accept)
        layout.addWidget(close)

        self.refresh()

    def _button(self, layout: QHBoxLayout, text: str, slot: object) -> QPushButton:
        button = QPushButton(text, self)
        button.clicked.connect(slot)
        button.setAccessibleName(text.replace("&", ""))
        layout.addWidget(button)
        return button

    # -- data --------------------------------------------------------------

    def refresh(self) -> None:
        """Reload the list from disk."""
        selected = self.selected_name()
        self.list.clear()
        for profile in self.store.all():
            item = QListWidgetItem(profile.name)
            item.setData(Qt.ItemDataRole.UserRole, profile)
            if self.identity is not None and profile.matches(self.identity):
                item.setText(self.tr("{0}  (this device)").format(profile.name))
            self.list.addItem(item)
            if profile.name == selected:
                self.list.setCurrentItem(item)
        self._update_buttons()

    def selected_profile(self) -> Profile | None:
        """The highlighted profile, if any."""
        items = self.list.selectedItems() or (
            [self.list.currentItem()] if self.list.currentRow() >= 0 else []
        )
        for item in items:
            value: object = item.data(Qt.ItemDataRole.UserRole)
            if isinstance(value, Profile):
                return value
        return None

    def selected_name(self) -> str | None:
        """Name of the highlighted profile."""
        profile = self.selected_profile()
        return profile.name if profile else None

    def _update_buttons(self) -> None:
        profile = self.selected_profile()
        for button in (
            self.edit_button,
            self.duplicate_button,
            self.delete_button,
            self.export_button,
        ):
            button.setEnabled(profile is not None)
        self.bind_button.setEnabled(profile is not None and self.identity is not None)
        if profile is None:
            self.detail.setText(self.tr("Profiles are stored in {0}").format(self.store.directory))
            return
        devices = ", ".join(profile.device_keys) or self.tr("not bound to a device")
        self.detail.setText(
            f"{profile.serial.describe()} · {devices}"
            + (f"\n{profile.description}" if profile.description else "")
        )

    # -- actions -----------------------------------------------------------

    def edit_selected(self) -> None:
        """Open the settings dialog for the highlighted profile."""
        profile = self.selected_profile()
        if profile is None:
            return
        from portside.gui.settings_dialog import SettingsDialog

        dialog = SettingsDialog(profile, self)
        if dialog.exec() == QDialog.DialogCode.Accepted:
            self._save(dialog.to_profile())

    def bind_selected(self) -> None:
        """Bind the highlighted profile to the connected device (F2 + F20)."""
        profile = self.selected_profile()
        if profile is None or self.identity is None:
            return
        self._save(profile.with_device(self.identity))

    def duplicate_selected(self) -> None:
        """Copy a profile under a new name."""
        profile = self.selected_profile()
        if profile is None:
            return
        name, ok = QInputDialog.getText(
            self, self.tr("Duplicate profile"), self.tr("New name:"), text=f"{profile.name} copy"
        )
        if ok and name.strip():
            from dataclasses import replace

            self._save(replace(profile, name=name.strip(), device_keys=()))

    def delete_selected(self) -> None:
        """Delete the highlighted profile, after confirming."""
        profile = self.selected_profile()
        if profile is None:
            return
        confirm = QMessageBox.question(
            self,
            self.tr("Delete profile"),
            self.tr("Delete the profile {0}?").format(profile.name),
        )
        if confirm == QMessageBox.StandardButton.Yes:
            self.store.delete(profile.name)
            self.refresh()

    def export_selected(self, path: Path | None = None) -> Path | None:
        """Write the highlighted profile to a file (F20)."""
        profile = self.selected_profile()
        if profile is None:
            return None
        target = path
        if target is None:  # pragma: no cover - native dialog
            chosen, _ = QFileDialog.getSaveFileName(
                self, self.tr("Export profile"), f"{profile.name}.toml", "TOML (*.toml)"
            )
            if not chosen:
                return None
            target = Path(chosen)
        try:
            return self.store.export(profile, target)
        except ProfileError as error:
            self._report(error)
            return None

    def import_profile(self, path: Path | None = None) -> Profile | None:
        """Read a profile from a file and save it (F20)."""
        source = path
        if source is None:  # pragma: no cover - native dialog
            chosen, _ = QFileDialog.getOpenFileName(
                self, self.tr("Import profile"), "", "TOML (*.toml)"
            )
            if not chosen:
                return None
            source = Path(chosen)
        try:
            imported = self.store.import_file(source)
        except ProfileError as error:
            self._report(error)
            return None
        self._save(imported)
        return imported

    def _save(self, profile: Profile) -> None:
        try:
            self.store.save(profile)
        except ProfileError as error:
            self._report(error)
            return
        self.refresh()

    def _report(self, error: ProfileError) -> None:
        """Show a profile error, keeping the text for tests to assert on."""
        self.last_error = error.display()
        QMessageBox.warning(self, self.tr("Profile problem"), self.last_error)
