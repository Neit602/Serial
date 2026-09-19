"""Connection, terminal, logging and pacing settings (PRD F3, F9-F16).

Everything here round-trips through the core's own dataclasses, so the dialog
cannot express a configuration the core would reject — validation happens as
the user types rather than at connect time.
"""

from __future__ import annotations

import re
from pathlib import Path

from PySide6.QtCore import Qt
from PySide6.QtGui import QIntValidator
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QSpinBox,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from portside.core.breaks import BreakMode
from portside.core.errors import InvalidSettingsError
from portside.core.pacing import PacePolicy
from portside.core.settings import (
    BAUD_MAX,
    BAUD_MIN,
    DATA_BITS_CHOICES,
    FlowControl,
    Parity,
    SerialSettings,
    StopBits,
)
from portside.core.textio import Backspace, LineEnding, TextPolicy
from portside.emulation.decoder import CharacterSet
from portside.emulation.terminal import DEFAULT_SCROLLBACK
from portside.gui.terminal_widget import DEFAULT_FONT_SIZE
from portside.logging.session_log import (
    DEFAULT_TEMPLATE,
    LogFormat,
    LogSettings,
    resolve_template,
)
from portside.logging.timestamps import TimestampMode
from portside.profiles.store import Profile, TerminalOptions

__all__ = ["SettingsDialog"]

#: Offered in the combo; any value in range can still be typed.
COMMON_BAUD_RATES = [
    300,
    1200,
    2400,
    4800,
    9600,
    19200,
    38400,
    57600,
    115200,
    230400,
    460800,
    921600,
    1500000,
    3000000,
]


class SettingsDialog(QDialog):
    """Edits a :class:`~portside.profiles.store.Profile`."""

    def __init__(self, profile: Profile, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setWindowTitle(self.tr("Session settings"))
        self._profile = profile

        layout = QVBoxLayout(self)
        self.tabs = QTabWidget(self)
        self.tabs.addTab(self._build_connection(), self.tr("Connection"))
        self.tabs.addTab(self._build_terminal(), self.tr("Terminal"))
        self.tabs.addTab(self._build_logging(), self.tr("Logging"))
        self.tabs.addTab(self._build_pacing(), self.tr("Paste && break"))
        layout.addWidget(self.tabs)

        self.buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel,
            parent=self,
        )
        self.buttons.accepted.connect(self._accept_if_valid)
        self.buttons.rejected.connect(self.reject)
        layout.addWidget(self.buttons)

        self.error_label = QLabel("", self)
        self.error_label.setStyleSheet("color: #c92a2a;")
        self.error_label.setWordWrap(True)
        layout.addWidget(self.error_label)

        self.load(profile)

    # -- pages -------------------------------------------------------------

    def _build_connection(self) -> QWidget:
        page = QWidget(self)
        form = QFormLayout(page)

        self.baud = QComboBox(page)
        self.baud.setEditable(True)
        self.baud.addItems([str(rate) for rate in COMMON_BAUD_RATES])
        edit = self.baud.lineEdit()
        if edit is not None:
            edit.setValidator(QIntValidator(BAUD_MIN, BAUD_MAX, self))
        self.baud.setAccessibleName(self.tr("Baud rate"))
        form.addRow(self.tr("&Baud rate"), self.baud)

        self.data_bits = QComboBox(page)
        self.data_bits.addItems([str(bits) for bits in DATA_BITS_CHOICES])
        self.data_bits.setAccessibleName(self.tr("Data bits"))
        form.addRow(self.tr("&Data bits"), self.data_bits)

        self.parity = QComboBox(page)
        for value, label in (
            (Parity.NONE, self.tr("None")),
            (Parity.EVEN, self.tr("Even")),
            (Parity.ODD, self.tr("Odd")),
            (Parity.MARK, self.tr("Mark")),
            (Parity.SPACE, self.tr("Space")),
        ):
            self.parity.addItem(label, value.value)
        self.parity.setAccessibleName(self.tr("Parity"))
        form.addRow(self.tr("&Parity"), self.parity)

        self.stop_bits = QComboBox(page)
        for stop_bits, stop_label in (
            (StopBits.ONE, "1"),
            (StopBits.ONE_POINT_FIVE, "1.5"),
            (StopBits.TWO, "2"),
        ):
            self.stop_bits.addItem(stop_label, stop_bits.value)
        self.stop_bits.setAccessibleName(self.tr("Stop bits"))
        form.addRow(self.tr("&Stop bits"), self.stop_bits)

        self.flow = QComboBox(page)
        for flow, flow_label in (
            (FlowControl.NONE, self.tr("None")),
            (FlowControl.RTSCTS, "RTS/CTS"),
            (FlowControl.DSRDTR, "DSR/DTR"),
            (FlowControl.XONXOFF, "XON/XOFF"),
        ):
            self.flow.addItem(flow_label, flow.value)
        self.flow.setAccessibleName(self.tr("Flow control"))
        form.addRow(self.tr("&Flow control"), self.flow)
        return page

    def _build_terminal(self) -> QWidget:
        page = QWidget(self)
        form = QFormLayout(page)

        self.send_ending = QComboBox(page)
        for value, label in (
            (LineEnding.CR, self.tr("CR (typical serial console)")),
            (LineEnding.LF, "LF"),
            (LineEnding.CRLF, "CR LF"),
            (LineEnding.NONE, self.tr("Send nothing extra")),
        ):
            self.send_ending.addItem(label, value.value)
        form.addRow(self.tr("Enter &sends"), self.send_ending)

        self.receive_ending = QComboBox(page)
        for value, label in (
            (LineEnding.NONE, self.tr("Leave as received")),
            (LineEnding.LF, self.tr("Normalise to LF")),
            (LineEnding.CR, self.tr("Normalise to CR")),
            (LineEnding.CRLF, self.tr("Normalise to CR LF")),
        ):
            self.receive_ending.addItem(label, value.value)
        form.addRow(self.tr("&Received endings"), self.receive_ending)

        self.local_echo = QCheckBox(self.tr("Echo typed characters locally"), page)
        form.addRow("", self.local_echo)

        self.backspace = QComboBox(page)
        self.backspace.addItem(self.tr("DEL (0x7F)"), Backspace.DEL.value)
        self.backspace.addItem(self.tr("BS (0x08)"), Backspace.BS.value)
        form.addRow(self.tr("Bac&kspace sends"), self.backspace)

        self.charset = QComboBox(page)
        for charset in CharacterSet:
            self.charset.addItem(charset.value, charset.value)
        form.addRow(self.tr("Character se&t"), self.charset)

        self.strip_high_bit = QCheckBox(self.tr("Strip the high bit from received bytes"), page)
        form.addRow("", self.strip_high_bit)

        self.scrollback = QSpinBox(page)
        self.scrollback.setRange(0, 1_000_000)
        self.scrollback.setSingleStep(1000)
        self.scrollback.setAccessibleName(self.tr("Scrollback lines"))
        form.addRow(self.tr("Scr&ollback lines"), self.scrollback)

        self.font_family = QLineEdit(page)
        self.font_family.setPlaceholderText(self.tr("System monospace"))
        form.addRow(self.tr("Font f&amily"), self.font_family)

        self.font_size = QSpinBox(page)
        self.font_size.setRange(6, 48)
        form.addRow(self.tr("Font si&ze"), self.font_size)
        return page

    def _build_logging(self) -> QWidget:
        page = QWidget(self)
        form = QFormLayout(page)

        self.log_enabled = QCheckBox(self.tr("Start logging when the session connects"), page)
        form.addRow("", self.log_enabled)

        self.log_format = QComboBox(page)
        self.log_format.addItem(self.tr("Text (decoded, timestamped)"), LogFormat.TEXT.value)
        self.log_format.addItem(self.tr("Raw (byte-exact)"), LogFormat.RAW.value)
        form.addRow(self.tr("For&mat"), self.log_format)

        directory_row = QWidget(page)
        directory_layout = QVBoxLayout(directory_row)
        directory_layout.setContentsMargins(0, 0, 0, 0)
        self.log_directory = QLineEdit(directory_row)
        self.log_directory.setPlaceholderText(self.tr("Default log folder for this platform"))
        directory_layout.addWidget(self.log_directory)
        browse = QPushButton(self.tr("Browse…"), directory_row)
        browse.clicked.connect(self._browse_for_directory)
        directory_layout.addWidget(browse)
        form.addRow(self.tr("Folde&r"), directory_row)

        self.log_template = QLineEdit(page)
        self.log_template.textChanged.connect(self._update_template_preview)
        form.addRow(self.tr("File&name"), self.log_template)

        self.template_preview = QLabel("", page)
        self.template_preview.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        form.addRow(self.tr("Preview"), self.template_preview)

        self.timestamps = QComboBox(page)
        for mode, label in (
            (TimestampMode.OFF, self.tr("Off")),
            (TimestampMode.ABSOLUTE, self.tr("Absolute")),
            (TimestampMode.ELAPSED, self.tr("Elapsed since connect")),
            (TimestampMode.DELTA, self.tr("Delta between lines")),
        ):
            self.timestamps.addItem(label, mode.value)
        form.addRow(self.tr("Timestam&ps"), self.timestamps)

        self.rotate_bytes = QSpinBox(page)
        self.rotate_bytes.setRange(0, 1_000_000_000)
        self.rotate_bytes.setSingleStep(1_048_576)
        self.rotate_bytes.setSpecialValueText(self.tr("No rotation"))
        form.addRow(self.tr("Rotate after (bytes)"), self.rotate_bytes)

        self.keep_files = QSpinBox(page)
        self.keep_files.setRange(1, 100)
        form.addRow(self.tr("Keep files"), self.keep_files)
        return page

    def _build_pacing(self) -> QWidget:
        page = QWidget(self)
        form = QFormLayout(page)

        self.char_delay = QDoubleSpinBox(page)
        self.char_delay.setRange(0.0, 1.0)
        self.char_delay.setDecimals(3)
        self.char_delay.setSingleStep(0.005)
        self.char_delay.setSuffix(self.tr(" s"))
        form.addRow(self.tr("Delay per &character"), self.char_delay)

        self.line_delay = QDoubleSpinBox(page)
        self.line_delay.setRange(0.0, 10.0)
        self.line_delay.setDecimals(3)
        self.line_delay.setSingleStep(0.05)
        self.line_delay.setSuffix(self.tr(" s"))
        form.addRow(self.tr("Delay per &line"), self.line_delay)

        self.wait_for_prompt = QLineEdit(page)
        self.wait_for_prompt.setPlaceholderText(r"[>#]\s*$")
        self.wait_for_prompt.textChanged.connect(self._validate_prompt)
        form.addRow(self.tr("&Wait for prompt"), self.wait_for_prompt)

        self.prompt_timeout = QDoubleSpinBox(page)
        self.prompt_timeout.setRange(0.1, 120.0)
        self.prompt_timeout.setSuffix(self.tr(" s"))
        form.addRow(self.tr("Prompt &timeout"), self.prompt_timeout)

        self.abort_on_timeout = QCheckBox(
            self.tr("Stop the paste if the prompt never arrives"), page
        )
        form.addRow("", self.abort_on_timeout)

        self.break_mode = QComboBox(page)
        for mode, label in (
            (BreakMode.AUTO, self.tr("Automatic (native, else emulated)")),
            (BreakMode.NATIVE, self.tr("Native only")),
            (BreakMode.EMULATED, self.tr("Emulated only")),
        ):
            self.break_mode.addItem(label, mode.value)
        form.addRow(self.tr("Break &mode"), self.break_mode)

        self.break_duration = QDoubleSpinBox(page)
        self.break_duration.setRange(0.01, 5.0)
        self.break_duration.setSingleStep(0.05)
        self.break_duration.setSuffix(self.tr(" s"))
        form.addRow(self.tr("Break &duration"), self.break_duration)
        return page

    # -- load and save -----------------------------------------------------

    def load(self, profile: Profile) -> None:
        """Fill every control from ``profile``."""
        serial = profile.serial
        self.baud.setCurrentText(str(serial.baudrate))
        self.data_bits.setCurrentText(str(serial.data_bits))
        self._select(self.parity, serial.parity.value)
        self._select(self.stop_bits, serial.stop_bits.value)
        self._select(self.flow, serial.flow_control.value)

        text = profile.text
        self._select(self.send_ending, text.send_ending.value)
        self._select(self.receive_ending, text.receive_ending.value)
        self.local_echo.setChecked(text.local_echo)
        self._select(self.backspace, text.backspace.value)

        terminal = profile.terminal
        self._select(self.charset, terminal.charset.value)
        self.strip_high_bit.setChecked(terminal.strip_high_bit)
        self.scrollback.setValue(terminal.scrollback)
        self.font_family.setText(terminal.font_family)
        self.font_size.setValue(terminal.font_size or DEFAULT_FONT_SIZE)

        logs = profile.logging
        self.log_enabled.setChecked(logs.auto_start)
        self._select(self.log_format, logs.log_format.value)
        self.log_directory.setText(str(logs.directory) if logs.directory else "")
        self.log_template.setText(logs.template or DEFAULT_TEMPLATE)
        self._select(self.timestamps, logs.timestamps.value)
        self.rotate_bytes.setValue(logs.max_bytes)
        self.keep_files.setValue(logs.keep)

        pace = profile.pacing
        self.char_delay.setValue(pace.char_delay)
        self.line_delay.setValue(pace.line_delay)
        self.wait_for_prompt.setText(pace.wait_for_prompt or "")
        self.prompt_timeout.setValue(pace.prompt_timeout)
        self.abort_on_timeout.setChecked(pace.abort_on_timeout)

        self._select(self.break_mode, profile.break_mode.value)
        self.break_duration.setValue(profile.break_duration)
        self._update_template_preview()

    def to_profile(self) -> Profile:
        """Build a profile from the controls.  Raises on invalid input."""
        from dataclasses import replace

        serial = SerialSettings(
            baudrate=int(self.baud.currentText()),
            data_bits=int(self.data_bits.currentText()),
            parity=Parity(self.parity.currentData()),
            stop_bits=StopBits(float(self.stop_bits.currentData())),
            flow_control=FlowControl(self.flow.currentData()),
        )
        text = TextPolicy(
            send_ending=LineEnding(self.send_ending.currentData()),
            receive_ending=LineEnding(self.receive_ending.currentData()),
            local_echo=self.local_echo.isChecked(),
            backspace=Backspace(self.backspace.currentData()),
        )
        terminal = TerminalOptions(
            scrollback=self.scrollback.value(),
            charset=CharacterSet(self.charset.currentData()),
            strip_high_bit=self.strip_high_bit.isChecked(),
            columns=self._profile.terminal.columns,
            rows=self._profile.terminal.rows,
            font_family=self.font_family.text().strip(),
            font_size=self.font_size.value(),
            theme=self._profile.terminal.theme,
        )
        directory = self.log_directory.text().strip()
        logs = LogSettings(
            directory=Path(directory) if directory else None,
            template=self.log_template.text().strip() or DEFAULT_TEMPLATE,
            log_format=LogFormat(self.log_format.currentData()),
            timestamps=TimestampMode(self.timestamps.currentData()),
            # The log's own charset is deliberately not edited here: the dialog
            # has no control for it, so overwriting it would silently discard
            # whatever a hand-edited profile set.
            charset=self._profile.logging.charset,
            auto_start=self.log_enabled.isChecked(),
            max_bytes=self.rotate_bytes.value(),
            keep=self.keep_files.value(),
        )
        prompt = self.wait_for_prompt.text().strip() or None
        pace = PacePolicy(
            char_delay=self.char_delay.value(),
            line_delay=self.line_delay.value(),
            wait_for_prompt=prompt,
            prompt_timeout=self.prompt_timeout.value(),
            abort_on_timeout=self.abort_on_timeout.isChecked(),
        )
        return replace(
            self._profile,
            serial=serial,
            text=text,
            terminal=terminal,
            logging=logs,
            pacing=pace,
            break_mode=BreakMode(self.break_mode.currentData()),
            break_duration=self.break_duration.value(),
        )

    # -- validation --------------------------------------------------------

    def _accept_if_valid(self) -> None:
        """Refuse to close on invalid input, and say what is wrong."""
        try:
            self.to_profile()
        except (InvalidSettingsError, ValueError) as error:
            self.error_label.setText(str(error))
            return
        self.error_label.setText("")
        self.accept()

    def _validate_prompt(self, pattern: str) -> None:
        """Mark an invalid regex while it is being typed (F13)."""
        if not pattern:
            self.wait_for_prompt.setStyleSheet("")
            self.error_label.setText("")
            return
        try:
            re.compile(pattern)
        except re.error as error:
            self.wait_for_prompt.setStyleSheet("border: 1px solid #c92a2a;")
            self.error_label.setText(self.tr("Invalid prompt pattern: {0}").format(error))
            return
        self.wait_for_prompt.setStyleSheet("")
        self.error_label.setText("")

    def _update_template_preview(self) -> None:
        """Show what the log file will actually be called (F15)."""
        template = self.log_template.text().strip() or DEFAULT_TEMPLATE
        self.template_preview.setText(
            resolve_template(
                template, port=self._profile.name or "port", profile=self._profile.name
            )
        )

    def _browse_for_directory(self) -> None:  # pragma: no cover - native dialog
        chosen = QFileDialog.getExistingDirectory(self, self.tr("Choose a log folder"))
        if chosen:
            self.log_directory.setText(chosen)

    @staticmethod
    def _select(combo: QComboBox, value: object) -> None:
        index = combo.findData(value)
        if index >= 0:
            combo.setCurrentIndex(index)

    @property
    def default_scrollback(self) -> int:
        """The scrollback used when a profile does not say (F8)."""
        return DEFAULT_SCROLLBACK
