"""Session events become Qt signals on the GUI thread (F3, F4, F5, F12, F13)."""

from __future__ import annotations

import threading
import time
from pathlib import Path

import pytest
from pytestqt.qtbot import QtBot

from portside.core.errors import DeviceVanishedError, PermissionDeniedError
from portside.core.events import ErrorRaised, ReconnectAttempt
from portside.core.pacing import PacePolicy
from portside.core.session import ReconnectPolicy, Session, SessionState
from portside.core.settings import SerialSettings
from portside.gui.controller import SessionController
from portside.gui.runtime import AsyncRuntime
from portside.logging.session_log import LogSettings, SessionLogger
from portside.transport.base import ModemLines
from portside.transport.loopback import LoopbackTransport

pytestmark = pytest.mark.gui


# -- signals -------------------------------------------------------------


def test_received_data_arrives_as_a_signal(
    qtbot: QtBot, controller: SessionController, transport: LoopbackTransport
) -> None:
    with qtbot.waitSignal(controller.dataReceived, timeout=5000) as caught:
        transport.feed(b"Switch>")
    assert caught.args[0] == b"Switch>"


def test_signals_are_delivered_on_the_gui_thread(
    qtbot: QtBot, controller: SessionController, transport: LoopbackTransport
) -> None:
    """Qt's queued connections are what make the bridge safe."""
    seen: list[int] = []
    controller.dataReceived.connect(lambda _data: seen.append(threading.get_ident()))
    with qtbot.waitSignal(controller.dataReceived, timeout=5000):
        transport.feed(b"x")
    assert seen == [threading.get_ident()]


def test_sent_bytes_are_reported(qtbot: QtBot, controller: SessionController) -> None:
    with qtbot.waitSignal(controller.bytesSent, timeout=5000) as caught:
        controller.send(b"show version\r")
    assert caught.args[0] == 13


def test_state_changes_are_reported(qtbot: QtBot, runtime: AsyncRuntime) -> None:
    """Collect the states rather than racing one signal against the open."""
    session = Session(LoopbackTransport(echo=False))
    controller = SessionController(session, runtime)
    seen: list[str] = []
    controller.stateChanged.connect(lambda state, _detail: seen.append(state))
    try:
        runtime.submit(session.open()).result(timeout=5)
        qtbot.waitUntil(lambda: SessionState.CONNECTED.value in seen, timeout=5000)
        assert seen[0] == SessionState.CONNECTING.value
    finally:
        controller.close()


def test_errors_are_reported_with_their_remedy(qtbot: QtBot, controller: SessionController) -> None:
    """F6: the GUI must be able to show the remedy verbatim."""
    error = PermissionDeniedError("denied", port="/dev/ttyUSB0", remedy="join dialout")
    with qtbot.waitSignal(controller.errorRaised, timeout=5000) as caught:
        controller.session.events.publish(ErrorRaised(error, fatal=True))
    assert caught.args[0].remedy == "join dialout"
    assert caught.args[1] is True


def test_reconnect_attempts_are_reported(qtbot: QtBot, controller: SessionController) -> None:
    with qtbot.waitSignal(controller.reconnecting, timeout=5000) as caught:
        controller.session.events.publish(ReconnectAttempt(attempt=2, delay=0.5))
    assert caught.args == [2, 0.5]


# -- actions -------------------------------------------------------------


def test_send_reaches_the_device(
    controller: SessionController, transport: LoopbackTransport
) -> None:
    assert controller.send(b"hello").result(timeout=5) == 5
    assert bytes(transport.written) == b"hello"


def test_break_is_sent(controller: SessionController, transport: LoopbackTransport) -> None:
    result = controller.send_break(0.25).result(timeout=5)
    assert result.emulated is False
    assert list(transport.breaks) == [0.25]


def test_control_lines_report_driver_support(
    controller: SessionController, transport: LoopbackTransport
) -> None:
    """F4: a driver without DTR/RTS must disable the buttons, not crash."""
    assert controller.set_dtr(True).result(timeout=5) is True
    assert transport.dtr is True


def test_modem_lines_are_readable(controller: SessionController) -> None:
    lines = controller.modem_lines().result(timeout=5)
    assert isinstance(lines, ModemLines)


def test_settings_can_change_on_a_live_session(
    controller: SessionController, transport: LoopbackTransport
) -> None:
    """F3: changing the baud rate must not drop the session."""
    controller.apply_settings(SerialSettings(baudrate=9600)).result(timeout=5)
    assert transport.settings.baudrate == 9600
    assert controller.is_connected is True


def test_the_gui_thread_is_never_blocked_by_a_slow_port(
    controller: SessionController, transport: LoopbackTransport
) -> None:
    """The whole reason the runtime exists."""

    class SlowTransport(LoopbackTransport):
        def write(self, data: bytes) -> int:
            time.sleep(0.5)
            return super().write(data)

    slow = SlowTransport(echo=False)
    slow.open()
    controller._session._transport = slow
    started = time.monotonic()
    future = controller.send(b"slow")
    elapsed = time.monotonic() - started
    assert elapsed < 0.1, "send() blocked the calling thread"
    future.result(timeout=5)


# -- paste (F13) ---------------------------------------------------------


def test_paste_reports_progress_and_completion(
    qtbot: QtBot, controller: SessionController, transport: LoopbackTransport
) -> None:
    controller.pace = PacePolicy(line_delay=0.0)
    with qtbot.waitSignal(controller.pasteFinished, timeout=10_000) as caught:
        controller.paste_text("configure terminal\nhostname lab\nend")
    result = caught.args[0]
    assert result.complete is True
    assert bytes(transport.written) == b"configure terminal\rhostname lab\rend\r"


def test_paste_file(
    qtbot: QtBot, controller: SessionController, transport: LoopbackTransport, tmp_path: Path
) -> None:
    config = tmp_path / "switch.cfg"
    config.write_text("hostname lab-sw1\n")
    controller.pace = PacePolicy(line_delay=0.0)
    with qtbot.waitSignal(controller.pasteFinished, timeout=10_000):
        controller.paste_file(config)
    assert bytes(transport.written) == b"hostname lab-sw1\r"


def test_pasting_a_missing_file_reports_an_error(
    qtbot: QtBot, controller: SessionController, tmp_path: Path
) -> None:
    with qtbot.waitSignal(controller.errorRaised, timeout=5000) as caught:
        controller.paste_file(tmp_path / "nope.cfg")
    assert "cannot read" in str(caught.args[0])


def test_a_paste_can_be_cancelled(
    controller: SessionController, transport: LoopbackTransport
) -> None:
    controller.pace = PacePolicy(line_delay=5.0)
    controller.paste_text("one\ntwo\nthree")
    time.sleep(0.2)
    controller.cancel_paste()
    time.sleep(0.2)
    assert bytes(transport.written).count(b"\r") < 3
    assert controller.is_connected is True


# -- logging (F15) -------------------------------------------------------


def test_logging_can_be_started_and_stopped(
    qtbot: QtBot, runtime: AsyncRuntime, tmp_path: Path
) -> None:
    transport = LoopbackTransport(echo=False)
    session = Session(transport)
    runtime.run(session.open())
    logger = SessionLogger(settings=LogSettings(directory=tmp_path, flush_interval=0.0))
    controller = SessionController(session, runtime, logger=logger)
    try:
        path = controller.start_logging()
        assert path is not None
        assert controller.is_logging is True
        with qtbot.waitSignal(controller.dataReceived, timeout=5000):
            transport.feed(b"logged\n")
        controller.stop_logging()
        assert "logged" in path.read_text()
    finally:
        controller.close()


def test_without_a_logger_logging_is_simply_unavailable(
    controller: SessionController,
) -> None:
    assert controller.is_logging is False
    assert controller.start_logging() is None
    controller.stop_logging()  # must not raise


# -- shutdown ------------------------------------------------------------


def test_close_is_idempotent_and_stops_the_session(
    runtime: AsyncRuntime, transport: LoopbackTransport
) -> None:
    session = Session(transport)
    runtime.run(session.open())
    controller = SessionController(session, runtime)
    controller.close()
    controller.close()
    assert session.state is SessionState.CLOSED


def test_auto_reconnect_is_visible_to_the_gui(qtbot: QtBot, runtime: AsyncRuntime) -> None:
    """F5: the UI must be able to show 'reconnecting', not just freeze."""
    first = LoopbackTransport("/dev/ttyUSB0", echo=False, vanish_after=1)
    second = LoopbackTransport("/dev/ttyUSB1", echo=False)
    session = Session(
        first,
        reconnect=ReconnectPolicy(enabled=True, initial_delay=0.01, max_delay=0.05),
        reopen=lambda: second,
    )
    runtime.run(session.open())
    controller = SessionController(session, runtime)
    try:
        with qtbot.waitSignal(controller.reconnecting, timeout=10_000):
            first.feed(b"!")
        with qtbot.waitSignal(controller.stateChanged, timeout=10_000):
            pass
        deadline = time.monotonic() + 10
        while controller.state is not SessionState.CONNECTED and time.monotonic() < deadline:
            time.sleep(0.05)
        assert controller.port == "/dev/ttyUSB1"
    finally:
        controller.close()


def test_a_vanished_device_reaches_the_gui_as_an_error(qtbot: QtBot, runtime: AsyncRuntime) -> None:
    transport = LoopbackTransport(echo=False, vanish_after=1)
    session = Session(transport)
    runtime.run(session.open())
    controller = SessionController(session, runtime)
    try:
        with qtbot.waitSignal(controller.errorRaised, timeout=10_000) as caught:
            transport.feed(b"!")
        assert isinstance(caught.args[0], DeviceVanishedError)
    finally:
        controller.close()
