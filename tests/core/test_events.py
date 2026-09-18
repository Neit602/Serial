"""The event bus front ends attach to."""

from __future__ import annotations

import logging

import pytest

from portside.core.errors import PortBusyError
from portside.core.events import (
    BytesReceived,
    ErrorRaised,
    Event,
    EventBus,
    PortListChanged,
    StateChanged,
)
from portside.core.ports import PortInfo
from portside.core.session import SessionState


def test_subscribers_receive_events_in_order() -> None:
    bus = EventBus()
    seen: list[Event] = []
    bus.subscribe(seen.append)
    bus.publish(BytesReceived(b"a"))
    bus.publish(BytesReceived(b"b"))
    assert [event.data for event in seen] == [b"a", b"b"]  # type: ignore[union-attr]


def test_every_subscriber_is_called() -> None:
    bus = EventBus()
    first: list[Event] = []
    second: list[Event] = []
    bus.subscribe(first.append)
    bus.subscribe(second.append)
    bus.publish(StateChanged(SessionState.DISCONNECTED, SessionState.CONNECTED))
    assert len(first) == len(second) == 1
    assert bus.subscriber_count == 2


def test_unsubscribe_stops_delivery_and_is_idempotent() -> None:
    bus = EventBus()
    seen: list[Event] = []
    unsubscribe = bus.subscribe(seen.append)
    unsubscribe()
    unsubscribe()
    bus.publish(BytesReceived(b"x"))
    assert seen == []
    assert bus.subscriber_count == 0


def test_a_raising_subscriber_is_logged_and_the_rest_still_run(
    caplog: pytest.LogCaptureFixture,
) -> None:
    bus = EventBus()
    seen: list[Event] = []

    def explode(event: Event) -> None:
        raise RuntimeError("bad widget")

    bus.subscribe(explode)
    bus.subscribe(seen.append)
    with caplog.at_level(logging.ERROR):
        bus.publish(BytesReceived(b"x"))
    assert len(seen) == 1
    assert "subscriber" in caplog.text


def test_unsubscribing_during_publish_is_safe() -> None:
    bus = EventBus()
    seen: list[Event] = []

    def once(event: Event) -> None:
        seen.append(event)
        unsubscribe()

    unsubscribe = bus.subscribe(once)
    bus.publish(BytesReceived(b"a"))
    bus.publish(BytesReceived(b"b"))
    assert len(seen) == 1


def test_events_are_immutable_value_objects() -> None:
    event = ErrorRaised(PortBusyError("busy", port="COM3"))
    with pytest.raises(AttributeError):
        event.fatal = False  # type: ignore[misc]
    assert event.fatal is True


def test_port_list_changed_carries_both_sides() -> None:
    added = (PortInfo(device="/dev/ttyUSB0"),)
    event = PortListChanged(added=added)
    assert event.added == added
    assert event.removed == ()
