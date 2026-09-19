"""The device list panel (PRD F1, F2).

Shows what is plugged in, refreshes on hot-plug within the PRD's two seconds,
and is the shortest path to a connected session: pick a port, press Enter.
"""

from __future__ import annotations

from PySide6.QtCore import Qt, QTimer, Signal
from PySide6.QtWidgets import (
    QAbstractItemView,
    QHeaderView,
    QLabel,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from portside.core.identity import DeviceIdentity
from portside.core.ports import PortInfo
from portside.platform.hotplug import DEFAULT_POLL_INTERVAL
from portside.transport.discovery import ComPortLister, list_ports

__all__ = ["DeviceList"]

_EMPTY_HELP = (
    "No serial ports found.\n\n"
    "Plug in an adapter and this list will refresh automatically.\n"
    "If it is already plugged in, check that its driver is installed and that "
    "you have permission to open the port."
)


class DeviceList(QWidget):
    """A refreshing list of serial ports."""

    #: Emitted when the user picks a port to connect to.
    portActivated = Signal(object)  # PortInfo
    #: Emitted whenever the selection changes (None when nothing is selected).
    portSelected = Signal(object)  # PortInfo | None

    def __init__(
        self,
        *,
        lister: ComPortLister | None = None,
        poll_interval: float = DEFAULT_POLL_INTERVAL,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._lister = lister
        self._ports: dict[str, PortInfo] = {}

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)

        self.tree = QTreeWidget(self)
        self.tree.setColumnCount(4)
        self.tree.setHeaderLabels(
            [self.tr("Port"), self.tr("Device"), self.tr("VID:PID"), self.tr("Serial")]
        )
        self.tree.setRootIsDecorated(False)
        self.tree.setAlternatingRowColors(True)
        self.tree.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.tree.setAccessibleName(self.tr("Serial ports"))
        header = self.tree.header()
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        self.tree.itemActivated.connect(self._on_activated)
        self.tree.itemDoubleClicked.connect(self._on_activated)
        self.tree.itemSelectionChanged.connect(self._on_selection_changed)
        layout.addWidget(self.tree)

        self.empty_label = QLabel(_EMPTY_HELP, self)
        self.empty_label.setWordWrap(True)
        self.empty_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.empty_label.setAccessibleName(self.tr("No ports found"))
        layout.addWidget(self.empty_label)

        # Polling covers every platform; Linux gets udev events through the
        # same code path in the core, and 1 s keeps us inside F1's 2 s budget.
        self._timer = QTimer(self)
        self._timer.setInterval(int(poll_interval * 1000))
        self._timer.timeout.connect(self.refresh)
        self._timer.start()
        self.refresh()

    # -- data --------------------------------------------------------------

    def refresh(self) -> None:
        """Rescan and update the list in place, keeping the selection (F1)."""
        try:
            ports = list_ports(lister=self._lister)
        except OSError:  # pragma: no cover - discovery should not fail
            return
        current = {port.device: port for port in ports}
        if current == self._ports:
            return

        selected = self.selected_port()
        self._ports = current
        self.tree.clear()
        for port in ports:
            item = QTreeWidgetItem(
                [
                    port.device,
                    port.chip_family or port.description or self.tr("serial port"),
                    port.vid_pid or "-",
                    port.serial_number or "-",
                ]
            )
            item.setData(0, Qt.ItemDataRole.UserRole, port)
            item.setToolTip(0, self._tooltip(port))
            self.tree.addTopLevelItem(item)
            if selected is not None and port.device == selected.device:
                item.setSelected(True)
                self.tree.setCurrentItem(item)

        has_ports = bool(ports)
        self.tree.setVisible(has_ports)
        self.empty_label.setVisible(not has_ports)

    @staticmethod
    def _tooltip(port: PortInfo) -> str:
        identity = DeviceIdentity.from_port(port)
        lines = [port.label(), identity.describe()]
        if port.driver:
            lines.append(f"driver: {port.driver}")
        if port.stable_path:
            lines.append(f"stable path: {port.stable_path}")
        return "\n".join(lines)

    @property
    def ports(self) -> list[PortInfo]:
        """The ports currently listed."""
        return list(self._ports.values())

    def selected_port(self) -> PortInfo | None:
        """The highlighted port, if any."""
        items = self.tree.selectedItems()
        if not items:
            return None
        value = items[0].data(0, Qt.ItemDataRole.UserRole)
        return value if isinstance(value, PortInfo) else None

    def activate_selected(self) -> None:
        """Connect to whatever is selected (the Connect button's action)."""
        port = self.selected_port()
        if port is not None:
            self.portActivated.emit(port)

    # -- signals -----------------------------------------------------------

    def _on_activated(self, item: QTreeWidgetItem, _column: int = 0) -> None:
        port = item.data(0, Qt.ItemDataRole.UserRole)
        if isinstance(port, PortInfo):
            self.portActivated.emit(port)

    def _on_selection_changed(self) -> None:
        self.portSelected.emit(self.selected_port())

    def stop(self) -> None:
        """Stop polling (called when the window closes)."""
        self._timer.stop()
