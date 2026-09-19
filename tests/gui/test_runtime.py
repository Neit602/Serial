"""The asyncio loop that lives beside Qt (architecture rule)."""

from __future__ import annotations

import asyncio
import threading

import pytest

from portside.gui.runtime import AsyncRuntime

pytestmark = pytest.mark.gui


async def _identity(value: int) -> int:
    await asyncio.sleep(0)
    return value


def test_coroutines_run_off_the_calling_thread() -> None:
    """The GUI thread must never be the one doing I/O."""
    caller = threading.get_ident()
    with AsyncRuntime() as runtime:
        where = runtime.run(_where())
    assert where != caller


async def _where() -> int:
    return threading.get_ident()


def test_submit_returns_a_future_without_blocking() -> None:
    with AsyncRuntime() as runtime:
        future = runtime.submit(_identity(42))
        assert future.result(timeout=5) == 42


def test_exceptions_surface_to_the_caller() -> None:
    async def boom() -> None:
        raise ValueError("bang")

    with AsyncRuntime() as runtime, pytest.raises(ValueError, match="bang"):
        runtime.run(boom())


def test_start_and_stop_are_idempotent() -> None:
    runtime = AsyncRuntime()
    runtime.start()
    runtime.start()
    assert runtime.running is True
    runtime.stop()
    runtime.stop()
    assert runtime.running is False


def test_no_thread_is_leaked() -> None:
    before = {thread.name for thread in threading.enumerate()}
    for _ in range(5):
        with AsyncRuntime() as runtime:
            runtime.run(_identity(1))
    after = {thread.name for thread in threading.enumerate()}
    assert not {name for name in after - before if "portside-async" in name}


def test_pending_work_is_cancelled_on_shutdown() -> None:
    """A stuck coroutine must not stop the app from closing."""
    runtime = AsyncRuntime()
    runtime.start()
    forever = runtime.submit(asyncio.sleep(3600))
    runtime.stop()
    assert runtime.running is False
    assert forever.cancelled() or forever.done()


def test_the_loop_starts_on_demand() -> None:
    runtime = AsyncRuntime()
    try:
        assert runtime.loop.is_running()
    finally:
        runtime.stop()
