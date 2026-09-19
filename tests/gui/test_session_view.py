"""One session's page: keys, signals, find, status (F4, F7, F8, F9, F22)."""

from __future__ import annotations

from collections.abc import Iterator

import pytest
from PySide6.QtCore import QEvent, Qt
from PySide6.QtGui import QKeyEvent
from pytestqt.qtbot import QtBot

from portside.core.session import Session
from portside.core.textio import Backspace, LineEnding, TextPolicy
from portside.gui.controller import SessionController
from portside.gui.keymap import qt_key_to_bytes
from portside.gui.runtime import AsyncRuntime
from portside.gui.session_view import SessionView
from portside.transport.base import ModemLines
from portside.transport.loopback import LoopbackTransport

pytestmark = pytest.mark.gui


@pytest.fixture
def view(
    qtbot: QtBot, runtime: AsyncRuntime, transport: LoopbackTransport
) -> Iterator[SessionView]:
    session = Session(transport)
    runtime.run(session.open())
    controller = SessionController(session, runtime)
    made = SessionView(controller)
    qtbot.addWidget(made)
    made.resize(800, 480)
    made.show()
    try:
        yield made
    finally:
        made.close_session()


def _key(key: Qt.Key, text: str = "", modifiers: Qt.KeyboardModifier | None = None) -> QKeyEvent:
    return QKeyEvent(
        QEvent.Type.KeyPress,
        key,
        modifiers or Qt.KeyboardModifier.NoModifier,
        text,
    )


# -- keyboard (F7, F9) ---------------------------------------------------


def test_typing_reaches_the_device(
    qtbot: QtBot, view: SessionView, transport: LoopbackTransport
) -> None:
    view.view.keyPressEvent(_key(Qt.Key.Key_A, "a"))
    qtbot.waitUntil(lambda: bytes(transport.written) == b"a", timeout=5000)


def test_enter_sends_the_configured_ending(
    qtbot: QtBot, view: SessionView, transport: LoopbackTransport
) -> None:
    view.controller.text_policy = TextPolicy(send_ending=LineEnding.CRLF)
    view.view.keyPressEvent(_key(Qt.Key.Key_Return))
    qtbot.waitUntil(lambda: bytes(transport.written) == b"\r\n", timeout=5000)


def test_backspace_follows_the_profile(qtbot: QtBot, view: SessionView) -> None:
    view.controller.text_policy = TextPolicy(backspace=Backspace.BS)
    assert qt_key_to_bytes(_key(Qt.Key.Key_Backspace), view.controller.text_policy) == b"\x08"


def test_control_keys_reach_the_device(
    qtbot: QtBot, view: SessionView, transport: LoopbackTransport
) -> None:
    """Ctrl-C must interrupt the switch, not the app."""
    view.view.keyPressEvent(_key(Qt.Key.Key_C, "", Qt.KeyboardModifier.ControlModifier))
    qtbot.waitUntil(lambda: bytes(transport.written) == b"\x03", timeout=5000)


def test_local_echo_shows_what_was_typed(qtbot: QtBot, view: SessionView) -> None:
    view.controller.text_policy = TextPolicy(local_echo=True)
    view.view.keyPressEvent(_key(Qt.Key.Key_H, "h"))
    view.view.keyPressEvent(_key(Qt.Key.Key_I, "i"))
    assert view.terminal.snapshot().text()[0] == "hi"


def test_typing_returns_to_the_live_tail(qtbot: QtBot, view: SessionView) -> None:
    for n in range(200):
        view.view.feed(f"line{n}\r\n".encode())
    view.view.scroll_pages(1)
    assert view.terminal.following is False
    view.view.keyPressEvent(_key(Qt.Key.Key_X, "x"))
    assert view.terminal.following is True


def test_page_keys_scroll_locally_and_send_nothing(
    view: SessionView, transport: LoopbackTransport
) -> None:
    assert qt_key_to_bytes(_key(Qt.Key.Key_PageUp), view.controller.text_policy) == b""


# -- signals (F4) --------------------------------------------------------


def test_unknown_modem_lines_are_shown_as_unknown(view: SessionView) -> None:
    """A driver that does not report a line must not be rendered as 'low'."""
    view.signals_panel.set_lines(ModemLines(cts=True, dsr=False, dcd=None, ri=None))
    assert view.signals_panel.state_of("CTS") == "asserted"
    assert view.signals_panel.state_of("DSR") == "low"
    assert view.signals_panel.state_of("DCD") == "not reported by this driver"


def test_toggling_dtr_reaches_the_transport(
    qtbot: QtBot, view: SessionView, transport: LoopbackTransport
) -> None:
    view.signals_panel.dtr.setChecked(True)
    qtbot.waitUntil(lambda: transport.dtr is True, timeout=5000)


def test_a_driver_without_control_lines_disables_the_toggles(
    qtbot: QtBot, runtime: AsyncRuntime
) -> None:
    """F4: report the limitation instead of pretending it worked."""
    transport = LoopbackTransport(echo=False, supports_control_lines=False)
    session = Session(transport)
    runtime.run(session.open())
    controller = SessionController(session, runtime)
    made = SessionView(controller)
    qtbot.addWidget(made)
    try:
        made.signals_panel.rts.setChecked(True)
        qtbot.waitUntil(lambda: made.signals_panel.rts.isEnabled() is False, timeout=5000)
        assert "does not implement" in made.signals_panel.rts.toolTip()
    finally:
        made.close_session()


def test_signals_are_greyed_out_when_disconnected(view: SessionView) -> None:
    view._on_state("disconnected", "device vanished")
    assert view.signals_panel.isEnabled() is False


# -- find (F8) -----------------------------------------------------------


def test_find_highlights_and_counts_matches(view: SessionView) -> None:
    view.view.feed(b"interface Gi1/0/1 is up\r\ninterface Gi1/0/2 is down\r\n")
    view.open_find()
    view.find_input.setText("interface")
    assert "2 matches" in view.find_status.text()
    assert len(view._matches) == 2


def test_find_reports_no_match(view: SessionView) -> None:
    view.open_find()
    view.find_input.setText("not-in-the-buffer")
    assert view.find_status.text() == "no match"


def test_find_is_literal_by_default(view: SessionView) -> None:
    """An IP address must not need escaping (F8)."""
    view.view.feed(b"address 10.0.0.1\r\n")
    view.open_find()
    view.find_input.setText("10.0.0.1")
    assert len(view._matches) == 1


def test_closing_find_clears_the_highlights(view: SessionView) -> None:
    view.view.feed(b"something\r\n")
    view.open_find()
    view.find_input.setText("something")
    view.close_find()
    assert view.find_bar.isVisible() is False
    assert view._matches == ()


# -- status (F22) --------------------------------------------------------


def test_the_status_line_reports_the_session(
    qtbot: QtBot, view: SessionView, transport: LoopbackTransport
) -> None:
    with qtbot.waitSignal(view.controller.dataReceived, timeout=5000):
        transport.feed(b"hello")
    text = view.status_text()
    assert transport.name in text
    assert "115200 8N1" in text
    assert "in 5 B" in text


def test_the_status_line_shows_scrollback_position(view: SessionView) -> None:
    for n in range(200):
        view.view.feed(f"line{n}\r\n".encode())
    view.view.scroll_pages(1)
    view._update_status()
    assert "scrollback" in view.status_text()


def test_reconnecting_is_visible(view: SessionView) -> None:
    """F5: the user must see why the terminal went quiet."""
    view._on_reconnecting(2, 0.5)
    assert "reconnecting (attempt 2" in view.status_text()
