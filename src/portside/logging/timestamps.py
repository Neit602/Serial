"""Per-line timestamps for session logs (PRD F16).

Four modes, all pure functions of (line, arrival time), so the same formatter
serves the log writer, the TUI's timestamp column and any future exporter.

A line is stamped with the time its **first** byte arrived: serial data comes
in chunks, and a line split across two reads must carry one timestamp, not two.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import UTC, datetime
from enum import StrEnum

__all__ = ["TimestampMode", "Timestamper"]


class TimestampMode(StrEnum):
    """How each logged line is stamped (F16)."""

    OFF = "off"
    ABSOLUTE = "absolute"
    ELAPSED = "elapsed"
    DELTA = "delta"


def _format_absolute(moment: float) -> str:
    """Local time with its UTC offset, so logs from several machines line up."""
    stamp = datetime.fromtimestamp(moment, tz=UTC).astimezone()
    return stamp.isoformat(timespec="milliseconds")


def _format_interval(seconds: float) -> str:
    """``HH:MM:SS.mmm`` — readable at a glance and sortable."""
    if seconds < 0:
        seconds = 0.0
    whole = int(seconds)
    millis = round((seconds - whole) * 1000)
    if millis == 1000:  # rounding crossed a second boundary
        whole, millis = whole + 1, 0
    hours, remainder = divmod(whole, 3600)
    minutes, secs = divmod(remainder, 60)
    return f"{hours:02d}:{minutes:02d}:{secs:02d}.{millis:03d}"


@dataclass
class Timestamper:
    """Stateful formatter: knows when the session started and when the last
    line arrived, which is what ``elapsed`` and ``delta`` need.
    """

    mode: TimestampMode = TimestampMode.OFF
    started_at: float = 0.0
    _previous: float | None = None

    def reset(self, now: float) -> None:
        """Restart the elapsed clock (F16 says it must be resettable)."""
        self.started_at = now
        self._previous = None

    def prefix(self, now: float) -> str:
        """The prefix for a line whose first byte arrived at ``now``.

        Returns ``""`` when stamping is off, so callers need no special case.
        """
        if self.mode is TimestampMode.OFF:
            return ""
        if self.mode is TimestampMode.ABSOLUTE:
            return f"[{_format_absolute(now)}] "
        if self.mode is TimestampMode.ELAPSED:
            return f"[{_format_interval(now - self.started_at)}] "
        delta = 0.0 if self._previous is None else now - self._previous
        self._previous = now
        return f"[+{_format_interval(delta)}] "

    def format_line(self, line: str, now: float) -> str:
        """``line`` with its timestamp prefix applied."""
        return f"{self.prefix(now)}{line}"
