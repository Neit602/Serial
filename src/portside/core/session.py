"""The session core: one device, one asyncio-facing API (PRD architecture).

Threading contract, which the whole design rests on:

* pySerial reads and writes block, so they run on threads this class owns —
  one reader thread, and a single-worker executor for writes and control calls
  (a single worker keeps writes strictly ordered).
* Data crosses into asyncio through ``loop.call_soon_threadsafe``.
* Callers (CLI today, TUI and GUI later) only ever await this class or listen
  on its :class:`~portside.core.events.EventBus`.  **No front end ever touches
  the transport.**
"""

from __future__ import annotations

import asyncio
import contextlib
import threading
from collections.abc import AsyncIterator, Callable
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from enum import StrEnum
from typing import Any

from portside.core.breaks import BreakMode, BreakResult, send_break
from portside.core.errors import (
    DeviceVanishedError,
    PortsideError,
    SessionError,
    TransportError,
)
from portside.core.events import (
    BytesReceived,
    BytesSent,
    ErrorRaised,
    EventBus,
    ReconnectAttempt,
    StateChanged,
)
from portside.core.identity import DeviceIdentity
from portside.core.settings import SerialSettings
from portside.transport.base import DEFAULT_BREAK_DURATION, ModemLines, Transport

__all__ = ["ReconnectPolicy", "Session", "SessionState"]

#: How long :meth:`Session.close` waits for the reader thread to notice.
_THREAD_JOIN_TIMEOUT = 3.0


class SessionState(StrEnum):
    """Lifecycle of a session; UIs render this directly."""

    DISCONNECTED = "disconnected"
    CONNECTING = "connecting"
    CONNECTED = "connected"
    RECONNECTING = "reconnecting"
    CLOSED = "closed"


@dataclass(frozen=True, slots=True)
class ReconnectPolicy:
    """Auto-reconnect behaviour (PRD F5)."""

    enabled: bool = False
    initial_delay: float = 0.5
    max_delay: float = 5.0
    factor: float = 2.0
    #: ``None`` means keep trying until the session is closed.
    max_attempts: int | None = None

    def delay_for(self, attempt: int) -> float:
        """Capped exponential backoff for attempt number ``attempt`` (1-based)."""
        delay = self.initial_delay * (self.factor ** max(0, attempt - 1))
        return min(delay, self.max_delay)


#: Called (on a worker thread) to obtain a fresh, *unopened* transport when the
#: device comes back.  See :func:`portside.transport.factory.identity_reopener`.
Reopener = Callable[[], Transport]


class Session:
    """One connection to one device.

    Args:
        transport: the transport to drive; the session owns it from here on.
        settings: line settings to apply on open (defaults to the transport's).
        reconnect: auto-reconnect policy (F5).
        reopen: factory returning a fresh transport for reconnects — normally
            built from a :class:`DeviceIdentity` so a renamed port still matches.
        identity: the device this session belongs to, for status and profiles.
        rx_maxsize: bounded receive queue; a consumer that falls behind loses
            the *oldest* buffered chunks rather than growing memory forever.
        read_size / read_timeout: reader thread tuning.
    """

    def __init__(
        self,
        transport: Transport,
        *,
        settings: SerialSettings | None = None,
        reconnect: ReconnectPolicy | None = None,
        reopen: Reopener | None = None,
        identity: DeviceIdentity | None = None,
        bus: EventBus | None = None,
        rx_maxsize: int = 512,
        read_size: int = 4096,
        read_timeout: float = 0.1,
        break_mode: BreakMode = BreakMode.AUTO,
    ) -> None:
        self._transport = transport
        self._settings = settings or transport.settings
        self._reconnect = reconnect or ReconnectPolicy()
        self._reopen = reopen
        self._identity = identity
        self.events = bus or EventBus()
        self._read_size = read_size
        self._read_timeout = read_timeout
        self.break_mode = break_mode

        self._state = SessionState.DISCONNECTED
        self._loop: asyncio.AbstractEventLoop | None = None
        self._queue: asyncio.Queue[bytes | None] = asyncio.Queue(maxsize=rx_maxsize)
        self._reader: threading.Thread | None = None
        self._stop = threading.Event()
        self._executor: ThreadPoolExecutor | None = None
        self._reconnect_task: asyncio.Task[None] | None = None
        self._closing = False

        self.bytes_in = 0
        self.bytes_out = 0
        #: Bytes dropped because the consumer could not keep up (see rx_maxsize).
        self.rx_dropped = 0

    # -- introspection -----------------------------------------------------

    @property
    def state(self) -> SessionState:
        """Current lifecycle state."""
        return self._state

    @property
    def port(self) -> str:
        """Name of the port currently in use."""
        return self._transport.name

    @property
    def settings(self) -> SerialSettings:
        """Line settings in force."""
        return self._settings

    @property
    def identity(self) -> DeviceIdentity | None:
        """Device identity, when the session was opened from a discovered port."""
        return self._identity

    @property
    def transport(self) -> Transport:
        """The transport — for tests and diagnostics, not for front ends."""
        return self._transport

    @property
    def is_connected(self) -> bool:
        """True while bytes can flow."""
        return self._state is SessionState.CONNECTED

    def status_line(self) -> str:
        """``/dev/ttyUSB0 115200 8N1 none · in 12 B · out 3 B · connected`` (F22)."""
        return (
            f"{self.port} {self._settings.describe()} · in {self.bytes_in} B · "
            f"out {self.bytes_out} B · {self._state.value}"
        )

    # -- lifecycle ---------------------------------------------------------

    async def open(self) -> None:
        """Open the port and start the reader thread."""
        if self._state in {SessionState.CONNECTED, SessionState.CONNECTING}:
            raise SessionError(f"session for {self.port} is already open", port=self.port)
        if self._state is SessionState.CLOSED:
            raise SessionError("session is closed; create a new one", port=self.port)
        self._loop = asyncio.get_running_loop()
        self._executor = ThreadPoolExecutor(
            max_workers=1, thread_name_prefix=f"portside-io-{self.port}"
        )
        self._set_state(SessionState.CONNECTING)
        try:
            await asyncio.to_thread(self._open_transport, self._transport)
        except PortsideError as exc:
            self._set_state(SessionState.DISCONNECTED, detail=str(exc))
            self._shutdown_executor()
            raise
        self._start_reader()
        self._set_state(SessionState.CONNECTED)

    def _open_transport(self, transport: Transport) -> None:
        """Open and configure a transport (runs on a worker thread)."""
        transport.apply_settings(self._settings)
        transport.open()

    async def close(self) -> None:
        """Close the port and stop every thread.  Idempotent."""
        if self._state is SessionState.CLOSED:
            return
        self._closing = True
        task, self._reconnect_task = self._reconnect_task, None
        if task is not None:
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
            except Exception:
                pass
        await self._stop_reader()
        self._shutdown_executor()
        self._set_state(SessionState.CLOSED)
        self._end_stream()

    async def __aenter__(self) -> Session:
        await self.open()
        return self

    async def __aexit__(self, *exc_info: object) -> None:
        await self.close()

    # -- data --------------------------------------------------------------

    async def send(self, data: bytes) -> int:
        """Write ``data`` to the device.

        Writes are serialised on a single worker thread, so two concurrent
        callers cannot interleave their bytes.
        """
        if not data:
            return 0
        transport = self._require_connected()
        try:
            written = int(await self._run_io(transport.write, data))
        except TransportError as exc:
            self._handle_failure(exc)
            raise
        self.bytes_out += written
        self.events.publish(BytesSent(written))
        return written

    async def receive(self, timeout: float | None = None) -> bytes | None:
        """Next chunk of received data; ``None`` once the session is finished."""
        if timeout is None:
            return await self._queue.get()
        try:
            return await asyncio.wait_for(self._queue.get(), timeout)
        except TimeoutError:
            return b""

    async def stream(self) -> AsyncIterator[bytes]:
        """Async iterator over received data, ending when the session ends."""
        while True:
            chunk = await self._queue.get()
            if chunk is None:
                return
            yield chunk

    # -- control lines -----------------------------------------------------

    async def send_break(
        self, duration: float = DEFAULT_BREAK_DURATION, *, mode: BreakMode | None = None
    ) -> BreakResult:
        """Send a break condition, emulating it when the driver cannot (F12).

        Runs on the I/O worker because the emulated form changes the baud rate
        and sleeps, which must never happen on the event loop.
        """
        transport = self._require_connected()
        chosen = mode or self.break_mode
        result = await self._run_io(lambda: send_break(transport, duration, mode=chosen))
        assert isinstance(result, BreakResult)
        return result

    async def set_dtr(self, value: bool) -> bool:
        """Drive DTR; False means the driver has no such line (F4)."""
        transport = self._require_connected()
        return bool(await self._run_io(transport.set_dtr, value))

    async def set_rts(self, value: bool) -> bool:
        """Drive RTS; False means the driver has no such line (F4)."""
        transport = self._require_connected()
        return bool(await self._run_io(transport.set_rts, value))

    async def modem_lines(self) -> ModemLines:
        """Read the input modem lines (F4)."""
        transport = self._require_connected()
        lines = await self._run_io(transport.get_modem_lines)
        assert isinstance(lines, ModemLines)
        return lines

    async def apply_settings(self, settings: SerialSettings) -> None:
        """Change line settings on the fly (F3)."""
        settings.validate()
        self._settings = settings
        if self._state is SessionState.CONNECTED:
            await self._run_io(self._transport.apply_settings, settings)

    # -- reader thread -----------------------------------------------------

    def _start_reader(self) -> None:
        self._stop.clear()
        thread = threading.Thread(
            target=self._read_loop,
            args=(self._transport,),
            name=f"portside-reader-{self.port}",
            daemon=True,
        )
        self._reader = thread
        thread.start()

    def _read_loop(self, transport: Transport) -> None:
        """Blocking read loop.  Runs on its own thread; never touches asyncio state."""
        while not self._stop.is_set():
            try:
                data = transport.read(self._read_size, self._read_timeout)
            except TransportError as exc:
                if not self._stop.is_set():
                    self._post(self._handle_failure, exc)
                return
            except Exception as exc:
                if not self._stop.is_set():
                    self._post(
                        self._handle_failure,
                        TransportError(f"read failed: {exc}", port=transport.name),
                    )
                return
            if data:
                self._post(self._on_data, data)

    def _post(self, callback: Callable[..., None], *args: Any) -> None:
        """Hand a callback to the event loop from the reader thread."""
        loop = self._loop
        if loop is None or loop.is_closed():
            return
        # pragma: no cover - the loop can shut down between the check and the call
        with contextlib.suppress(RuntimeError):
            loop.call_soon_threadsafe(callback, *args)

    def _on_data(self, data: bytes) -> None:
        """Runs on the loop thread: account, publish, enqueue with backpressure."""
        self.bytes_in += len(data)
        self.events.publish(BytesReceived(data))
        while True:
            try:
                self._queue.put_nowait(data)
                return
            except asyncio.QueueFull:
                # Bounded memory beats unbounded lag: drop the oldest chunk.
                dropped = self._queue.get_nowait()
                if dropped is None:
                    return
                self.rx_dropped += len(dropped)

    async def _stop_reader(self) -> None:
        self._stop.set()
        transport = self._transport
        with contextlib.suppress(Exception):  # unblocking a read is best effort
            transport.cancel_read()
        with contextlib.suppress(Exception):  # closing must not raise
            transport.close()
        thread, self._reader = self._reader, None
        if thread is not None and thread.is_alive():
            await asyncio.to_thread(thread.join, _THREAD_JOIN_TIMEOUT)

    # -- failure and reconnect --------------------------------------------

    def _handle_failure(self, error: TransportError) -> None:
        """Runs on the loop thread when the reader or a write failed."""
        if self._is_closing() or self._state is SessionState.CLOSED:
            return
        should_reconnect = (
            self._reconnect.enabled
            and self._reopen is not None
            and isinstance(error, DeviceVanishedError)
        )
        self.events.publish(ErrorRaised(error, fatal=not should_reconnect))
        if should_reconnect:
            self._set_state(SessionState.RECONNECTING, detail=str(error))
            if self._reconnect_task is None or self._reconnect_task.done():
                self._reconnect_task = asyncio.create_task(self._reconnect_loop(str(error)))
            return
        self._set_state(SessionState.DISCONNECTED, detail=str(error))
        self._end_stream()

    async def _reconnect_loop(self, reason: str) -> None:
        """Re-open the device by identity, with capped backoff (F5)."""
        assert self._reopen is not None
        self._stop.set()
        attempt = 0
        while not self._is_closing():
            attempt += 1
            if self._reconnect.max_attempts is not None and attempt > self._reconnect.max_attempts:
                error = DeviceVanishedError(
                    f"gave up reconnecting to {self.port} after {attempt - 1} attempts",
                    port=self.port,
                )
                self.events.publish(ErrorRaised(error, fatal=True))
                self._set_state(SessionState.DISCONNECTED, detail=str(error))
                self._end_stream()
                return
            delay = self._reconnect.delay_for(attempt)
            self.events.publish(ReconnectAttempt(attempt=attempt, delay=delay, reason=reason))
            await asyncio.sleep(delay)
            if self._is_closing():
                return
            try:
                transport = await asyncio.to_thread(self._reopen)
                await asyncio.to_thread(self._open_transport, transport)
            except PortsideError as exc:
                # Still missing, or came back with the wrong permissions: retry.
                self.events.publish(ErrorRaised(exc, fatal=False))
                reason = str(exc)
                continue
            self._transport = transport
            self._start_reader()
            self._set_state(SessionState.CONNECTED, detail=f"reconnected to {transport.name}")
            return

    # -- helpers -----------------------------------------------------------

    async def _run_io(self, func: Callable[..., Any], *args: Any) -> Any:
        """Run a blocking transport call on the session's single I/O worker."""
        loop = self._loop or asyncio.get_running_loop()
        executor = self._executor
        if executor is None:
            raise SessionError("session is not open", port=self.port)
        return await loop.run_in_executor(executor, lambda: func(*args))

    def _is_closing(self) -> bool:
        """Whether close() has begun.

        A method rather than a bare attribute read: the flag is set by another
        task while the reconnect loop is awaiting, which static flow analysis
        cannot see.
        """
        return self._closing

    def _require_connected(self) -> Transport:
        if self._state is not SessionState.CONNECTED:
            raise SessionError(
                f"session for {self.port} is {self._state.value}, not connected",
                port=self.port,
            )
        return self._transport

    def _set_state(self, new: SessionState, *, detail: str | None = None) -> None:
        old, self._state = self._state, new
        if old is not new:
            self.events.publish(StateChanged(old=old, new=new, detail=detail))

    def _end_stream(self) -> None:
        """Wake up `stream()` / `receive()` consumers with the end sentinel."""
        try:
            self._queue.put_nowait(None)
        except asyncio.QueueFull:
            # Consumer is far behind; make room - the sentinel matters more.
            try:
                self._queue.get_nowait()
                self._queue.put_nowait(None)
            except (asyncio.QueueEmpty, asyncio.QueueFull):  # pragma: no cover
                pass

    def _shutdown_executor(self) -> None:
        executor, self._executor = self._executor, None
        if executor is not None:
            executor.shutdown(wait=False, cancel_futures=True)
