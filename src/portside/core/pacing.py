"""Paced paste (PRD F13).

Pasting a config into a console at full speed is the classic way to lose half
of it: network gear echoes, parses and sometimes drops characters, and has no
flow control worth trusting.  Portside sends it the way a careful human would —
a delay between characters, a longer one between lines, and optionally waiting
for the device's prompt before each next line.

Timing is injected (``sleep``/``now``) so the tests are deterministic rather
than slow.
"""

from __future__ import annotations

import asyncio
import re
import time
from collections.abc import Awaitable, Callable
from dataclasses import dataclass, field
from typing import TYPE_CHECKING

from portside.core.errors import PortsideError
from portside.core.textio import LineEnding, TextPolicy

if TYPE_CHECKING:
    from portside.core.session import Session

__all__ = [
    "PacePolicy",
    "PasteAbortedError",
    "PasteFinished",
    "PasteProgress",
    "PasteResult",
    "paste_text",
]


@dataclass(frozen=True, slots=True)
class PacePolicy:
    """How fast to feed text to the device (F13).

    Args:
        char_delay: seconds between characters (0 sends each line in one write).
        line_delay: seconds after each line ending.
        wait_for_prompt: regex the device must emit before the next line goes
            out.  This is far more reliable than a fixed delay on gear whose
            parse time varies with the command.
        prompt_timeout: how long to wait for that prompt.
        abort_on_timeout: stop the paste when the prompt never arrives, rather
            than pressing on into a device that is not listening.
    """

    char_delay: float = 0.0
    line_delay: float = 0.05
    wait_for_prompt: str | None = None
    prompt_timeout: float = 5.0
    abort_on_timeout: bool = True

    def __post_init__(self) -> None:
        if self.char_delay < 0 or self.line_delay < 0:
            raise PortsideError("paste delays cannot be negative")
        if self.prompt_timeout <= 0:
            raise PortsideError("prompt timeout must be positive")
        if self.wait_for_prompt is not None:
            try:
                re.compile(self.wait_for_prompt)
            except re.error as exc:
                raise PortsideError(f"invalid prompt pattern: {exc}") from exc

    @property
    def prompt_pattern(self) -> re.Pattern[str] | None:
        """Compiled prompt regex, or ``None`` when not waiting for one."""
        return re.compile(self.wait_for_prompt) if self.wait_for_prompt else None


@dataclass(frozen=True, slots=True)
class PasteProgress:
    """Emitted after each line so a UI can show a progress bar (F13, F22)."""

    line: int
    total_lines: int
    bytes_sent: int
    waited_for_prompt: bool = False

    @property
    def fraction(self) -> float:
        """Completion between 0.0 and 1.0."""
        return self.line / self.total_lines if self.total_lines else 1.0


@dataclass(frozen=True, slots=True)
class PasteFinished:
    """Emitted once a paste ends, however it ended."""

    result: PasteResult


@dataclass(frozen=True, slots=True)
class PasteResult:
    """Outcome of a paste."""

    lines_sent: int
    total_lines: int
    bytes_sent: int
    elapsed: float
    aborted: bool = False
    timed_out_lines: tuple[int, ...] = field(default_factory=tuple)

    @property
    def complete(self) -> bool:
        """True when every line reached the device."""
        return not self.aborted and self.lines_sent == self.total_lines

    def describe(self) -> str:
        """One line for the status bar or the log."""
        state = "aborted" if self.aborted else "sent"
        summary = f"{state} {self.lines_sent}/{self.total_lines} lines, {self.bytes_sent} B"
        if self.timed_out_lines:
            summary += f", {len(self.timed_out_lines)} prompt timeout(s)"
        return summary


class PasteAbortedError(PortsideError):
    """The paste stopped early: cancelled, or a prompt never arrived."""


async def paste_text(
    session: Session,
    text: str,
    policy: PacePolicy | None = None,
    *,
    text_policy: TextPolicy | None = None,
    on_progress: Callable[[PasteProgress], None] | None = None,
    sleep: Callable[[float], Awaitable[None]] = asyncio.sleep,
    now: Callable[[], float] = time.monotonic,
) -> PasteResult:
    """Send ``text`` to the device at the pace ``policy`` describes.

    The coroutine is cancellable: cancelling it stops the paste between lines
    and leaves the session healthy, which is what the TUI's cancel key needs.
    """
    pace = policy or PacePolicy()
    discipline = text_policy or TextPolicy(send_ending=LineEnding.CR)
    pattern = pace.prompt_pattern
    lines = text.splitlines()
    started = now()
    sent_lines = 0
    sent_bytes = 0
    timed_out: list[int] = []

    watcher = _PromptWatcher(session, pattern) if pattern else None
    try:
        for index, line in enumerate(lines, start=1):
            if watcher is not None:
                watcher.arm()
            payload = discipline.encode_line(line)
            sent_bytes += await _send_paced(session, payload, pace, sleep)
            sent_lines = index

            waited = False
            # Nothing follows the last line, so waiting for one more prompt only
            # delays the paste - and, when the device stays quiet, turns a
            # successful paste into a spurious timeout.
            is_last = index == len(lines)
            if watcher is not None and not is_last:
                waited = await watcher.wait(pace.prompt_timeout, sleep=sleep, now=now)
                if not waited:
                    timed_out.append(index)
                    if pace.abort_on_timeout:
                        result = PasteResult(
                            lines_sent=sent_lines,
                            total_lines=len(lines),
                            bytes_sent=sent_bytes,
                            elapsed=now() - started,
                            aborted=True,
                            timed_out_lines=tuple(timed_out),
                        )
                        _report(on_progress, index, len(lines), sent_bytes, waited)
                        return result
            elif pace.line_delay and not is_last:
                await sleep(pace.line_delay)

            _report(on_progress, index, len(lines), sent_bytes, waited)
    except asyncio.CancelledError:
        # A cancelled paste is a normal outcome (the user pressed the key), but
        # the caller still needs to know how far it got.
        raise PasteAbortedError(
            f"paste cancelled after {sent_lines} of {len(lines)} lines",
            port=session.port,
        ) from None
    finally:
        if watcher is not None:
            watcher.close()

    return PasteResult(
        lines_sent=sent_lines,
        total_lines=len(lines),
        bytes_sent=sent_bytes,
        elapsed=now() - started,
        timed_out_lines=tuple(timed_out),
    )


def _report(
    on_progress: Callable[[PasteProgress], None] | None,
    line: int,
    total: int,
    sent: int,
    waited: bool,
) -> None:
    if on_progress is not None:
        on_progress(PasteProgress(line, total, sent, waited_for_prompt=waited))


async def _send_paced(
    session: Session,
    payload: bytes,
    pace: PacePolicy,
    sleep: Callable[[float], Awaitable[None]],
) -> int:
    """Send one line, character by character when a char delay is configured."""
    if not pace.char_delay:
        return await session.send(payload)
    total = 0
    for index in range(len(payload)):
        total += await session.send(payload[index : index + 1])
        await sleep(pace.char_delay)
    return total


class _PromptWatcher:
    """Watches received data for the prompt the next line should wait on.

    Subscribes to the session's event bus rather than consuming its receive
    queue, so the TUI keeps seeing every byte while a paste is in progress.
    """

    def __init__(self, session: Session, pattern: re.Pattern[str]) -> None:
        from portside.core.events import BytesReceived, Event

        self._pattern = pattern
        self._buffer = ""
        self._seen = False

        def listen(event: Event) -> None:
            if isinstance(event, BytesReceived):
                self._buffer += event.data.decode("utf-8", "replace")
                # Keep only enough context for a prompt to match across chunks.
                self._buffer = self._buffer[-4096:]
                if self._pattern.search(self._buffer):
                    self._seen = True

        self._unsubscribe = session.events.subscribe(listen)

    def arm(self) -> None:
        """Forget anything seen before this line was sent."""
        self._seen = False
        self._buffer = ""

    async def wait(
        self,
        timeout: float,
        *,
        sleep: Callable[[float], Awaitable[None]],
        now: Callable[[], float],
        poll: float = 0.01,
    ) -> bool:
        """Wait for the prompt; False if it never came within ``timeout``."""
        deadline = now() + timeout
        while now() < deadline:
            if self._seen:
                return True
            await sleep(poll)
        return self._seen

    def close(self) -> None:
        """Stop listening."""
        self._unsubscribe()
