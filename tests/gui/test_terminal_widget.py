"""The painted terminal: rendering, selection, clipboard, scrollback (F7, F8, F10)."""

from __future__ import annotations

from typing import Any

import pytest
from PySide6.QtCore import QPoint, QPointF, Qt
from PySide6.QtGui import QMouseEvent
from PySide6.QtWidgets import QApplication
from pytestqt.qtbot import QtBot

from portside.emulation.hexview import ViewMode
from portside.emulation.terminal import Terminal
from portside.gui.palette import ThemeName
from portside.gui.terminal_widget import TerminalWidget

pytestmark = pytest.mark.gui


@pytest.fixture
def widget(qtbot: QtBot) -> TerminalWidget:
    made = TerminalWidget(Terminal(80, 24))
    qtbot.addWidget(made)
    made.resize(800, 480)
    made.show()
    qtbot.waitExposed(made)
    return made


def _point(widget: TerminalWidget, row: int, column: int) -> QPointF:
    """The centre of a character cell, in widget coordinates."""
    width, height = widget.cell_size
    return QPointF(column * width + width / 2, row * height + height / 2)


def _mouse(widget: TerminalWidget, kind: QMouseEvent.Type, row: int, column: int) -> QMouseEvent:
    local = _point(widget, row, column)
    return QMouseEvent(
        kind,
        local,
        widget.mapToGlobal(local.toPoint()).toPointF(),
        Qt.MouseButton.LeftButton,
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.NoModifier,
    )


def _drag(
    widget: TerminalWidget, qtbot: QtBot, start: tuple[int, int], end: tuple[int, int]
) -> None:
    widget.mousePressEvent(_mouse(widget, QMouseEvent.Type.MouseButtonPress, *start))
    widget.mouseMoveEvent(_mouse(widget, QMouseEvent.Type.MouseMove, *end))
    widget.mouseReleaseEvent(_mouse(widget, QMouseEvent.Type.MouseButtonRelease, *end))


# -- rendering -----------------------------------------------------------


def test_the_emulator_is_sized_to_the_widget(widget: TerminalWidget) -> None:
    """The device must wrap where the window wraps."""
    columns, rows = widget.terminal.columns, widget.terminal.rows
    assert columns > 1 and rows > 1
    widget.resize(400, 240)
    assert widget.terminal.columns < columns


def test_received_text_reaches_the_emulator(widget: TerminalWidget) -> None:
    widget.feed(b"Switch> show version\r\n")
    assert widget.terminal.snapshot().text()[0] == "Switch> show version"


def test_painting_does_not_raise_for_colours_and_attributes(
    widget: TerminalWidget, qtbot: QtBot
) -> None:
    """A paint crash would take the whole app down, so exercise the paths."""
    widget.feed(b"\x1b[31;1mRED\x1b[0m \x1b[44m\x1b[7mREV\x1b[0m \x1b[4mUL\x1b[0m")
    widget.repaint()
    assert "RED" in widget.terminal.snapshot().text()[0]


def test_rendering_is_batched_not_per_byte(widget: TerminalWidget, qtbot: QtBot) -> None:
    """PRD risk: a repaint per byte would spend the session drawing."""
    painted = 0
    original = widget.update

    def counting_update(*args: Any, **kwargs: Any) -> None:
        nonlocal painted
        painted += 1
        original(*args, **kwargs)

    widget.update = counting_update  # type: ignore[method-assign]
    for _ in range(300):
        widget.feed(b"x")
    assert painted == 0, "feeding data must not paint; the frame timer does"
    widget._tick()
    assert painted == 1


def test_hex_view_shows_the_same_bytes(widget: TerminalWidget) -> None:
    widget.feed(b"AB")
    widget.set_view_mode(ViewMode.HEX)
    assert widget.view_mode is ViewMode.HEX
    widget.repaint()  # must not raise


# -- fonts and themes ----------------------------------------------------


def test_changing_the_font_changes_the_cell_grid(widget: TerminalWidget) -> None:
    small = widget.cell_size
    widget.set_font("", 20)
    assert widget.cell_size[0] > small[0]
    assert widget.font_size == 20


def test_an_unknown_font_falls_back_to_a_monospace(widget: TerminalWidget) -> None:
    """No font name exists on all three platforms, so this must not break."""
    widget.set_font("Definitely Not Installed", 12)
    assert widget.cell_size[0] > 0
    assert widget.terminal.columns > 1


def theme_of(widget: TerminalWidget) -> ThemeName:
    """Read through a call so one assert does not narrow the next."""
    return widget.palette_name


def test_themes_can_be_switched(widget: TerminalWidget) -> None:
    widget.set_theme(ThemeName.LIGHT)
    assert theme_of(widget) is ThemeName.LIGHT
    widget.repaint()
    widget.set_theme(ThemeName.HIGH_CONTRAST)
    assert theme_of(widget) is ThemeName.HIGH_CONTRAST


# -- selection and clipboard (F8) ---------------------------------------


def test_dragging_selects_text(widget: TerminalWidget, qtbot: QtBot) -> None:
    widget.feed(b"configure terminal\r\n")
    _drag(widget, qtbot, (0, 0), (0, 8))
    assert widget.selected_text() == "configure"


def test_selection_spans_several_rows(widget: TerminalWidget, qtbot: QtBot) -> None:
    widget.feed(b"first line\r\nsecond line\r\n")
    _drag(widget, qtbot, (0, 0), (1, 5))
    assert widget.selected_text().splitlines() == ["first line", "second"]


def test_copy_puts_the_selection_on_the_system_clipboard(
    widget: TerminalWidget, qtbot: QtBot
) -> None:
    """The half of F8 that M2 left to the GUI."""
    widget.feed(b"hostname lab-sw1\r\n")
    _drag(widget, qtbot, (0, 0), (0, 15))
    copied = widget.copy_selection()
    assert copied == "hostname lab-sw1"
    clipboard = QApplication.clipboard()
    assert clipboard is not None
    assert clipboard.text() == "hostname lab-sw1"


def test_select_to_copy_option(widget: TerminalWidget, qtbot: QtBot) -> None:
    widget.select_to_copy = True
    widget.feed(b"auto copied\r\n")
    _drag(widget, qtbot, (0, 0), (0, 10))
    clipboard = QApplication.clipboard()
    assert clipboard is not None
    assert clipboard.text() == "auto copied"


def test_copying_nothing_leaves_the_clipboard_alone(widget: TerminalWidget) -> None:
    clipboard = QApplication.clipboard()
    assert clipboard is not None
    clipboard.setText("untouched")
    assert widget.copy_selection() == ""
    assert clipboard.text() == "untouched"


def test_selection_can_be_cleared(widget: TerminalWidget, qtbot: QtBot) -> None:
    widget.feed(b"some text\r\n")
    _drag(widget, qtbot, (0, 0), (0, 3))
    widget.clear_selection()
    assert widget.selected_text() == ""


# -- scrollback (F8) -----------------------------------------------------


def test_scrolling_moves_through_the_history(widget: TerminalWidget) -> None:
    for n in range(200):
        widget.feed(f"line{n}\r\n".encode())
    widget.scroll_pages(1)
    assert widget.terminal.following is False
    widget.follow_tail()
    assert widget.terminal.following is True


def test_the_wheel_scrolls(widget: TerminalWidget, qtbot: QtBot) -> None:
    from PySide6.QtCore import QPointF
    from PySide6.QtGui import QWheelEvent

    for n in range(100):
        widget.feed(f"line{n}\r\n".encode())
    event = QWheelEvent(
        QPointF(10, 10),
        QPointF(10, 10),
        QPoint(0, 0),
        QPoint(0, 120),
        Qt.MouseButton.NoButton,
        Qt.KeyboardModifier.NoModifier,
        Qt.ScrollPhase.NoScrollPhase,
        False,
    )
    widget.wheelEvent(event)
    assert widget.terminal.scroll_offset > 0


# -- accessibility -------------------------------------------------------


def test_the_terminal_has_an_accessible_name(widget: TerminalWidget) -> None:
    assert widget.accessibleName()
    assert widget.accessibleDescription()
