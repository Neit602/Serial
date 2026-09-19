"""Modem line indicators and DTR/RTS toggles (PRD F4).

Three states per indicator, not two: a driver that does not report a line gives
``None``, and the panel shows "unknown" rather than claiming the line is low.
"""

from __future__ import annotations

from PySide6.QtCore import QTimer, Signal
from PySide6.QtWidgets import (
    QCheckBox,
    QHBoxLayout,
    QLabel,
    QWidget,
)

from portside.transport.base import ModemLines

__all__ = ["SignalsPanel"]

_POLL_MS = 1000

_STATE_STYLE = {
    True: "color: #2f9e44; font-weight: bold;",
    False: "color: #868e96;",
    None: "color: #adb5bd; font-style: italic;",
}
_STATE_NAME = {True: "asserted", False: "low", None: "not reported by this driver"}


class SignalsPanel(QWidget):
    """Live CTS/DSR/DCD/RI plus DTR and RTS controls."""

    dtrToggled = Signal(bool)
    rtsToggled = Signal(bool)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(4, 0, 4, 0)

        self._indicators: dict[str, QLabel] = {}
        for name in ("CTS", "DSR", "DCD", "RI"):
            label = QLabel(name, self)
            label.setAccessibleName(self.tr("{0} signal").format(name))
            label.setToolTip(self.tr("{0}: unknown").format(name))
            self._indicators[name] = label
            layout.addWidget(label)

        layout.addSpacing(12)

        self.dtr = QCheckBox("DTR", self)
        self.dtr.setAccessibleName(self.tr("Data Terminal Ready"))
        self.dtr.toggled.connect(self.dtrToggled)
        layout.addWidget(self.dtr)

        self.rts = QCheckBox("RTS", self)
        self.rts.setAccessibleName(self.tr("Request To Send"))
        self.rts.toggled.connect(self.rtsToggled)
        layout.addWidget(self.rts)
        layout.addStretch(1)

        self.set_lines(ModemLines())

        self._timer = QTimer(self)
        self._timer.setInterval(_POLL_MS)

    # -- state -------------------------------------------------------------

    def set_lines(self, lines: ModemLines) -> None:
        """Update the indicators from a reading (F4)."""
        for name, value in (
            ("CTS", lines.cts),
            ("DSR", lines.dsr),
            ("DCD", lines.dcd),
            ("RI", lines.ri),
        ):
            label = self._indicators[name]
            label.setStyleSheet(_STATE_STYLE[value])
            label.setToolTip(f"{name}: {_STATE_NAME[value]}")
            label.setAccessibleDescription(f"{name} {_STATE_NAME[value]}")

    def state_of(self, name: str) -> str:
        """The rendered state of one indicator (used by tests)."""
        return self._indicators[name].toolTip().split(": ", 1)[1]

    def set_control_lines_supported(self, supported: bool) -> None:
        """Disable DTR/RTS when the driver has no such lines, and say why."""
        for box in (self.dtr, self.rts):
            box.setEnabled(supported)
            box.setToolTip(
                "" if supported else self.tr("This adapter's driver does not implement this line.")
            )

    def set_enabled_for_session(self, connected: bool) -> None:
        """Grey the whole panel out when nothing is connected."""
        self.setEnabled(connected)
        if not connected:
            self.set_lines(ModemLines())

    def start_polling(self, callback: object) -> None:
        """Poll the modem lines once a second while connected."""
        self._timer.timeout.connect(callback)
        self._timer.start()

    def stop_polling(self) -> None:
        """Stop polling (on disconnect or window close)."""
        self._timer.stop()
