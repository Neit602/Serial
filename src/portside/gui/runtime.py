"""An asyncio loop that lives beside the Qt event loop.

The core is asyncio; Qt has its own loop and will not share it.  Portside runs
the asyncio loop on a dedicated thread and moves results across with Qt's
queued signals, which keeps the PRD's rule intact — the GUI thread never
touches a port, and never blocks on one either.

The alternative would be a loop-integration library such as qasync; a worker
thread keeps the dependency list exactly as the PRD specifies and makes the
thread boundary something you can see rather than something you assume.
"""

from __future__ import annotations

import asyncio
import threading
from collections.abc import Coroutine
from concurrent.futures import Future
from typing import Any, TypeVar

__all__ = ["AsyncRuntime"]

T = TypeVar("T")

#: How long :meth:`AsyncRuntime.stop` waits for the loop thread to finish.
SHUTDOWN_TIMEOUT = 5.0


class AsyncRuntime:
    """Runs an asyncio event loop on its own thread."""

    def __init__(self, name: str = "portside-async") -> None:
        self._name = name
        self._loop: asyncio.AbstractEventLoop | None = None
        self._thread: threading.Thread | None = None
        self._ready = threading.Event()

    # -- lifecycle ---------------------------------------------------------

    def start(self) -> None:
        """Start the loop thread.  Idempotent."""
        if self._thread is not None:
            return
        self._ready.clear()
        self._thread = threading.Thread(target=self._run, name=self._name, daemon=True)
        self._thread.start()
        # Nothing may be submitted before the loop exists.
        self._ready.wait(timeout=SHUTDOWN_TIMEOUT)

    def _run(self) -> None:
        loop = asyncio.new_event_loop()
        self._loop = loop
        asyncio.set_event_loop(loop)
        self._ready.set()
        try:
            loop.run_forever()
        finally:
            try:
                self._cancel_pending(loop)
                loop.run_until_complete(loop.shutdown_asyncgens())
            finally:
                loop.close()

    @staticmethod
    def _cancel_pending(loop: asyncio.AbstractEventLoop) -> None:
        """Cancel whatever is still running so shutdown cannot hang."""
        pending = [task for task in asyncio.all_tasks(loop) if not task.done()]
        for task in pending:
            task.cancel()
        if pending:
            loop.run_until_complete(asyncio.gather(*pending, return_exceptions=True))

    def stop(self) -> None:
        """Stop the loop and join its thread.  Idempotent."""
        loop, self._loop = self._loop, None
        thread, self._thread = self._thread, None
        if loop is not None:
            loop.call_soon_threadsafe(loop.stop)
        if thread is not None:
            thread.join(timeout=SHUTDOWN_TIMEOUT)
        self._ready.clear()

    @property
    def running(self) -> bool:
        """True while the loop thread is alive."""
        return self._thread is not None and self._thread.is_alive()

    @property
    def loop(self) -> asyncio.AbstractEventLoop:
        """The running loop.  Starts the runtime if it is not running yet."""
        if self._loop is None:
            self.start()
        loop = self._loop
        if loop is None:  # pragma: no cover - start() failed
            raise RuntimeError("the async runtime could not be started")
        return loop

    # -- submitting work ---------------------------------------------------

    def submit(self, coro: Coroutine[Any, Any, T]) -> Future[T]:
        """Schedule ``coro`` on the loop thread and return its future.

        The caller (the GUI thread) never waits on the future unless it asks
        to: attach a callback, or use :meth:`run` for the rare case where a
        result really is needed synchronously.
        """
        return asyncio.run_coroutine_threadsafe(coro, self.loop)

    def run(self, coro: Coroutine[Any, Any, T], timeout: float | None = 10.0) -> T:
        """Run ``coro`` and wait for its result.

        Only for start-up and tests: calling this from a Qt slot freezes the
        UI for the duration, which is exactly what the runtime exists to avoid.
        """
        return self.submit(coro).result(timeout)

    def __enter__(self) -> AsyncRuntime:
        self.start()
        return self

    def __exit__(self, *exc: object) -> None:
        self.stop()
