"""Hot-plug detection (F1: noticed within 2 s)."""

from __future__ import annotations

import asyncio
from collections.abc import Sequence

import pytest

from portside.core.ports import PortInfo
from portside.platform.hotplug import (
    DEFAULT_POLL_INTERVAL,
    PollingPortWatcher,
    PortAdded,
    PortRemoved,
    create_port_watcher,
)

USB0 = PortInfo(device="/dev/ttyUSB0", vid=0x0403, pid=0x6001, serial_number="A")
USB1 = PortInfo(device="/dev/ttyUSB1", vid=0x10C4, pid=0xEA60, serial_number="B")


class ScriptedLister:
    """Returns a different port list on each poll."""

    def __init__(self, *frames: Sequence[PortInfo]) -> None:
        self._frames = list(frames)
        self.calls = 0

    def __call__(self) -> Sequence[PortInfo]:
        frame = self._frames[min(self.calls, len(self._frames) - 1)]
        self.calls += 1
        return frame


async def _collect(watcher: PollingPortWatcher, count: int) -> list[object]:
    events: list[object] = []
    async for event in watcher.events():
        events.append(event)
        if len(events) == count:
            break
    return events


async def test_added_and_removed_ports_are_reported() -> None:
    lister = ScriptedLister([USB0], [USB0, USB1], [USB1])
    watcher = PollingPortWatcher(lister, interval=0, sleep=lambda _: asyncio.sleep(0))
    events = await _collect(watcher, 2)
    assert isinstance(events[0], PortAdded)
    assert events[0].port.device == "/dev/ttyUSB1"
    assert isinstance(events[1], PortRemoved)
    assert events[1].port.device == "/dev/ttyUSB0"


async def test_the_initial_port_list_is_not_reported_as_new() -> None:
    lister = ScriptedLister([USB0, USB1], [USB0, USB1], [USB0])
    watcher = PollingPortWatcher(lister, interval=0, sleep=lambda _: asyncio.sleep(0))
    events = await _collect(watcher, 1)
    assert isinstance(events[0], PortRemoved)


async def test_initial_ports_can_be_requested() -> None:
    watcher = PollingPortWatcher(
        ScriptedLister([USB0]), interval=0, sleep=lambda _: asyncio.sleep(0), emit_initial=True
    )
    events = await _collect(watcher, 1)
    assert isinstance(events[0], PortAdded)


async def test_changes_are_reported_within_the_prd_budget() -> None:
    """F1 allows 2 s; polling at 1 s guarantees it with room to spare."""
    slept: list[float] = []

    async def fake_sleep(delay: float) -> None:
        slept.append(delay)

    lister = ScriptedLister([USB0], [USB0, USB1])
    watcher = PollingPortWatcher(lister, sleep=fake_sleep)
    events = await _collect(watcher, 1)
    assert isinstance(events[0], PortAdded)
    assert sum(slept) <= 2.0
    assert DEFAULT_POLL_INTERVAL <= 1.0


def test_watcher_selection_falls_back_to_polling_off_linux() -> None:
    watcher = create_port_watcher(lambda: [], platform_name="windows")
    assert isinstance(watcher, PollingPortWatcher)


def test_watcher_selection_can_skip_udev_on_linux() -> None:
    watcher = create_port_watcher(lambda: [], platform_name="linux", prefer_udev=False)
    assert isinstance(watcher, PollingPortWatcher)


async def test_udev_watcher_diffs_the_port_list_on_a_kernel_event() -> None:
    """The pyudev path is driven with a stub monitor so it runs on every OS."""
    from portside.platform.hotplug import UdevPortWatcher

    class StubMonitor:
        def __init__(self) -> None:
            self.polls = 0

        def poll(self, timeout: float | None = None) -> object | None:
            self.polls += 1
            return object() if self.polls == 1 else None

    lister = ScriptedLister([USB0], [USB0, USB1], [USB0, USB1])
    watcher = UdevPortWatcher(lister, monitor_factory=StubMonitor)
    events: list[object] = []
    async for event in watcher.events():
        events.append(event)
        break
    assert isinstance(events[0], PortAdded)
    assert events[0].port.device == "/dev/ttyUSB1"


@pytest.mark.skipif(
    not __import__("sys").platform.startswith("linux"), reason="pyudev is Linux only"
)
def test_pyudev_is_importable_on_linux() -> None:
    from portside.platform.hotplug import UdevPortWatcher

    assert UdevPortWatcher.available() is True
