"""Typed session events and a small fan-out bus.

This is the single seam the CLI uses today and the TUI (M2) and GUI (M3) will
attach to: front ends subscribe, they never reach into the session's internals
and never touch the port.

Subscribers are called synchronously on the event loop thread.  A subscriber
that raises is reported and skipped — one broken widget must not kill a
session.
"""

from __future__ import annotations

import contextlib
import logging as _stdlib_logging
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from portside.core.errors import PortsideError
    from portside.core.ports import PortInfo
    from portside.core.session import SessionState

__all__ = [
    "BytesReceived",
    "BytesSent",
    "ErrorRaised",
    "Event",
    "EventBus",
    "PortListChanged",
    "ReconnectAttempt",
    "StateChanged",
    "Subscriber",
]

_log = _stdlib_logging.getLogger(__name__)


@dataclass(frozen=True, slots=True)
class StateChanged:
    """The session moved between states."""

    old: SessionState
    new: SessionState
    detail: str | None = None


@dataclass(frozen=True, slots=True)
class BytesReceived:
    """Bytes arrived from the device."""

    data: bytes


@dataclass(frozen=True, slots=True)
class BytesSent:
    """Bytes were handed to the driver."""

    count: int


@dataclass(frozen=True, slots=True)
class ErrorRaised:
    """Something failed.  ``fatal`` marks the end of the session."""

    error: PortsideError
    fatal: bool = True


@dataclass(frozen=True, slots=True)
class ReconnectAttempt:
    """An auto-reconnect attempt is about to happen (F5)."""

    attempt: int
    delay: float
    reason: str | None = None


@dataclass(frozen=True, slots=True)
class PortListChanged:
    """The set of present ports changed (F1 hot-plug)."""

    added: tuple[PortInfo, ...] = ()
    removed: tuple[PortInfo, ...] = ()


Event = StateChanged | BytesReceived | BytesSent | ErrorRaised | ReconnectAttempt | PortListChanged
Subscriber = Callable[[Event], None]


@dataclass
class EventBus:
    """Synchronous fan-out to any number of subscribers."""

    _subscribers: list[Subscriber] = field(default_factory=list)

    def subscribe(self, subscriber: Subscriber) -> Callable[[], None]:
        """Register ``subscriber``; returns a callable that unsubscribes it."""
        self._subscribers.append(subscriber)

        def unsubscribe() -> None:
            with contextlib.suppress(ValueError):  # unsubscribing twice is fine
                self._subscribers.remove(subscriber)

        return unsubscribe

    def publish(self, event: Event) -> None:
        """Deliver ``event`` to every subscriber, isolating failures."""
        for subscriber in list(self._subscribers):
            try:
                subscriber(event)
            except Exception:
                _log.exception("event subscriber %r failed on %r", subscriber, event)

    @property
    def subscriber_count(self) -> int:
        """How many subscribers are attached (used by tests and diagnostics)."""
        return len(self._subscribers)
