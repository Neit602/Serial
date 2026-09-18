"""Hot-plug detection (PRD F1: refresh within 2 s, F5: auto-reconnect).

Two implementations behind one async iterator:

* :class:`PollingPortWatcher` — rescans the port list on an interval.  Works
  identically on all three platforms and is the fallback everywhere.
* :class:`UdevPortWatcher` — Linux only, event driven via pyudev (an optional
  import), so a replug is noticed immediately instead of up to one poll late.
"""

from __future__ import annotations

import asyncio
from collections.abc import AsyncIterator, Awaitable, Callable, Sequence
from dataclasses import dataclass

from portside.core.ports import PortInfo

__all__ = [
    "DEFAULT_POLL_INTERVAL",
    "PollingPortWatcher",
    "PortAdded",
    "PortEvent",
    "PortRemoved",
    "PortWatcher",
    "create_port_watcher",
]

#: Poll interval; half the PRD's 2 s refresh budget so a change is always
#: reported inside it.
DEFAULT_POLL_INTERVAL = 1.0


@dataclass(frozen=True, slots=True)
class PortAdded:
    """A port appeared."""

    port: PortInfo


@dataclass(frozen=True, slots=True)
class PortRemoved:
    """A port went away."""

    port: PortInfo


PortEvent = PortAdded | PortRemoved

Lister = Callable[[], Sequence[PortInfo]]
Sleeper = Callable[[float], Awaitable[None]]


class PortWatcher:
    """Base class: an async iterator of :data:`PortEvent`."""

    def __aiter__(self) -> AsyncIterator[PortEvent]:
        return self.events()

    def events(self) -> AsyncIterator[PortEvent]:  # pragma: no cover - interface
        raise NotImplementedError


class PollingPortWatcher(PortWatcher):
    """Rescan the port list every ``interval`` seconds and diff it."""

    def __init__(
        self,
        lister: Lister,
        *,
        interval: float = DEFAULT_POLL_INTERVAL,
        sleep: Sleeper | None = None,
        emit_initial: bool = False,
    ) -> None:
        self._lister = lister
        self._interval = interval
        self._sleep = sleep or asyncio.sleep
        self._emit_initial = emit_initial

    async def events(self) -> AsyncIterator[PortEvent]:
        """Yield add/remove events until the consumer stops iterating."""
        known: dict[str, PortInfo] = {}
        first = True
        while True:
            current = {port.device: port for port in self._lister()}
            if first and not self._emit_initial:
                known = current
                first = False
            else:
                for device, port in current.items():
                    if device not in known:
                        yield PortAdded(port)
                for device, port in known.items():
                    if device not in current:
                        yield PortRemoved(port)
                known = current
                first = False
            await self._sleep(self._interval)


class UdevPortWatcher(PortWatcher):
    """Linux hot-plug via pyudev, falling back to polling if pyudev is absent."""

    def __init__(
        self,
        lister: Lister,
        *,
        interval: float = DEFAULT_POLL_INTERVAL,
        monitor_factory: Callable[[], object] | None = None,
    ) -> None:
        self._lister = lister
        self._interval = interval
        self._monitor_factory = monitor_factory

    @staticmethod
    def available() -> bool:
        """True if pyudev can be imported (Linux, optional dependency)."""
        try:
            import pyudev  # noqa: F401
        except Exception:
            return False
        return True

    def _make_monitor(self) -> object:
        if self._monitor_factory is not None:
            return self._monitor_factory()
        import pyudev

        context = pyudev.Context()
        monitor = pyudev.Monitor.from_netlink(context)
        monitor.filter_by("tty")
        return monitor

    async def events(self) -> AsyncIterator[PortEvent]:
        """Yield events driven by udev, diffing the port list on each kernel event."""
        monitor = self._make_monitor()
        poll = getattr(monitor, "poll", None)
        if poll is None:  # pragma: no cover - defensive
            async for event in PollingPortWatcher(self._lister, interval=self._interval).events():
                yield event
            return
        known = {port.device: port for port in self._lister()}
        while True:
            device = await asyncio.to_thread(poll, self._interval)
            if device is None:
                continue
            current = {port.device: port for port in self._lister()}
            for name, port in current.items():
                if name not in known:
                    yield PortAdded(port)
            for name, port in known.items():
                if name not in current:
                    yield PortRemoved(port)
            known = current


def create_port_watcher(
    lister: Lister,
    *,
    interval: float = DEFAULT_POLL_INTERVAL,
    prefer_udev: bool = True,
    platform_name: str | None = None,
) -> PortWatcher:
    """The best watcher available on this platform."""
    from portside.platform import platform_name as current_platform_name

    name = platform_name or current_platform_name()
    if name == "linux" and prefer_udev and UdevPortWatcher.available():
        return UdevPortWatcher(lister, interval=interval)
    return PollingPortWatcher(lister, interval=interval)
