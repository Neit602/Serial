"""Break signalling, native and emulated (F12)."""

from __future__ import annotations

import pytest

from portside.core.breaks import (
    BREAK_BITS,
    BreakMode,
    baud_for_break,
    send_break,
    zero_bytes_for_break,
)
from portside.core.errors import PortsideError
from portside.core.settings import BAUD_MIN, SerialSettings
from portside.transport.loopback import LoopbackTransport


class NoBreakTransport(LoopbackTransport):
    """An adapter whose driver has no break, like many CH340s and every PTY."""

    def send_break(self, duration: float = 0.25) -> None:
        raise NotImplementedError("this driver cannot send a break")


class RecordingTransport(LoopbackTransport):
    """Records the order of settings changes and writes, to prove the sequence."""

    def __init__(self, **kwargs: object) -> None:
        super().__init__(**kwargs)  # type: ignore[arg-type]
        self.journal: list[tuple[str, object]] = []

    def apply_settings(self, settings: SerialSettings) -> None:
        super().apply_settings(settings)
        self.journal.append(("settings", settings.baudrate))

    def write(self, data: bytes) -> int:
        self.journal.append(("write", data))
        return super().write(data)

    def send_break(self, duration: float = 0.25) -> None:
        raise NotImplementedError


def test_native_break_is_used_when_the_driver_has_one() -> None:
    transport = LoopbackTransport()
    transport.open()
    result = send_break(transport, 0.25)
    assert result.emulated is False
    assert list(transport.breaks) == [0.25]
    assert result.describe() == "native break, 250 ms"


def test_auto_falls_back_to_emulation() -> None:
    transport = NoBreakTransport(echo=False)
    transport.open()
    result = send_break(transport, 0.25, sleep=lambda _: None)
    assert result.emulated is True
    assert bytes(transport.written) == b"\x00" * result.bytes_sent
    assert transport.settings.baudrate == 115200, "line settings must be restored"


def test_emulated_break_sequence_is_settings_write_settings() -> None:
    """Drop the baud rate, send zeros, put the baud rate back — in that order."""
    transport = RecordingTransport(echo=False)
    transport.open()
    result = send_break(transport, 0.25, mode=BreakMode.EMULATED, sleep=lambda _: None)
    kinds = [kind for kind, _ in transport.journal]
    assert kinds == ["settings", "write", "settings"]
    assert transport.journal[0][1] == result.baudrate
    assert transport.journal[1][1] == b"\x00" * result.bytes_sent
    assert transport.journal[2][1] == 115200


def test_settings_are_restored_even_when_the_write_fails() -> None:
    """Otherwise a failed break leaves the session stuck at 50 baud."""

    class FailingTransport(RecordingTransport):
        def write(self, data: bytes) -> int:
            self.journal.append(("write", data))
            raise OSError("write failed")

    transport = FailingTransport(echo=False)
    transport.open()
    with pytest.raises(OSError, match="write failed"):
        send_break(transport, 0.25, mode=BreakMode.EMULATED, sleep=lambda _: None)
    assert transport.settings.baudrate == 115200
    assert [kind for kind, _ in transport.journal][-1] == "settings"


def test_native_mode_refuses_to_emulate_and_explains_why() -> None:
    transport = NoBreakTransport()
    transport.open()
    with pytest.raises(PortsideError) as caught:
        send_break(transport, 0.25, mode=BreakMode.NATIVE)
    assert caught.value.remedy is not None
    assert "emulated" in caught.value.remedy


def test_emulated_mode_skips_a_working_native_break() -> None:
    """For drivers that claim break support but do not assert it."""
    transport = LoopbackTransport(echo=False)
    transport.open()
    result = send_break(transport, 0.1, mode=BreakMode.EMULATED, sleep=lambda _: None)
    assert result.emulated is True
    assert not transport.breaks


@pytest.mark.parametrize("duration", [0.05, 0.1, 0.25, 0.5, 1.0])
def test_the_emulated_break_always_lasts_at_least_as_long_as_asked(
    duration: float,
) -> None:
    """A break that is too short simply fails to interrupt the device."""
    baudrate = baud_for_break(duration)
    count = zero_bytes_for_break(duration, baudrate)
    assert baudrate >= BAUD_MIN
    assert count * BREAK_BITS / baudrate >= duration


def test_the_slowest_baud_rate_is_respected() -> None:
    """No driver is obliged to accept an arbitrarily slow rate."""
    assert baud_for_break(10.0) == BAUD_MIN
    assert zero_bytes_for_break(10.0, BAUD_MIN) > 1


def test_a_non_positive_duration_is_rejected() -> None:
    transport = LoopbackTransport()
    transport.open()
    with pytest.raises(PortsideError, match="must be positive"):
        send_break(transport, 0.0)


def test_the_sleep_covers_the_time_on_the_wire() -> None:
    slept: list[float] = []
    transport = NoBreakTransport(echo=False)
    transport.open()
    result = send_break(transport, 0.25, sleep=slept.append)
    assert result.baudrate is not None
    assert sum(slept) >= 0.25
    assert sum(slept) == pytest.approx(result.bytes_sent * BREAK_BITS / result.baudrate)


def test_result_describes_itself_for_the_status_bar() -> None:
    transport = NoBreakTransport(echo=False)
    transport.open()
    described = send_break(transport, 0.25, sleep=lambda _: None).describe()
    assert "emulated break" in described and "50 baud" in described
