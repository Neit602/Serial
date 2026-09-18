"""Per-line timestamps (F16)."""

from __future__ import annotations

from datetime import datetime

import pytest

from portside.logging.timestamps import Timestamper, TimestampMode

# 2026-09-18 12:00:00 UTC, as a float clock reading.
BASE = datetime.fromisoformat("2026-09-18T12:00:00+00:00").timestamp()


def test_off_adds_nothing() -> None:
    assert Timestamper(TimestampMode.OFF).prefix(BASE) == ""
    assert Timestamper(TimestampMode.OFF).format_line("hello", BASE) == "hello"


def test_absolute_is_iso_8601_with_an_offset() -> None:
    """Logs from several machines must be comparable, so the offset is in."""
    prefix = Timestamper(TimestampMode.ABSOLUTE).prefix(BASE)
    assert prefix.startswith("[2026-09-18T")
    assert prefix.endswith("] ")
    parsed = datetime.fromisoformat(prefix.strip("[] "))
    assert parsed.utcoffset() is not None
    assert parsed.timestamp() == pytest.approx(BASE, abs=0.001)


def test_absolute_keeps_milliseconds() -> None:
    prefix = Timestamper(TimestampMode.ABSOLUTE).prefix(BASE + 0.25)
    assert ".250" in prefix


def test_elapsed_counts_from_the_session_start() -> None:
    stamper = Timestamper(TimestampMode.ELAPSED, started_at=BASE)
    assert stamper.prefix(BASE) == "[00:00:00.000] "
    assert stamper.prefix(BASE + 1.5) == "[00:00:01.500] "
    assert stamper.prefix(BASE + 3661.25) == "[01:01:01.250] "


def test_elapsed_can_be_reset() -> None:
    """F16 requires the elapsed clock to be resettable mid-session."""
    stamper = Timestamper(TimestampMode.ELAPSED, started_at=BASE)
    assert stamper.prefix(BASE + 10) == "[00:00:10.000] "
    stamper.reset(BASE + 10)
    assert stamper.prefix(BASE + 11) == "[00:00:01.000] "


def test_delta_measures_the_gap_between_lines() -> None:
    stamper = Timestamper(TimestampMode.DELTA, started_at=BASE)
    assert stamper.prefix(BASE) == "[+00:00:00.000] "
    assert stamper.prefix(BASE + 0.05) == "[+00:00:00.050] "
    assert stamper.prefix(BASE + 0.10) == "[+00:00:00.050] "
    assert stamper.prefix(BASE + 2.0) == "[+00:00:01.900] "


def test_delta_restarts_after_a_reset() -> None:
    stamper = Timestamper(TimestampMode.DELTA, started_at=BASE)
    stamper.prefix(BASE + 1)
    stamper.reset(BASE + 5)
    assert stamper.prefix(BASE + 6) == "[+00:00:00.000] "


def test_sub_millisecond_gaps_round_sanely() -> None:
    stamper = Timestamper(TimestampMode.DELTA, started_at=BASE)
    stamper.prefix(BASE)
    assert stamper.prefix(BASE + 0.0004) == "[+00:00:00.000] "
    assert stamper.prefix(BASE + 0.0004 + 0.9999) == "[+00:00:01.000] "


def test_a_clock_that_goes_backwards_does_not_print_negative_time() -> None:
    """NTP steps and suspend/resume happen mid-session."""
    stamper = Timestamper(TimestampMode.ELAPSED, started_at=BASE)
    assert stamper.prefix(BASE - 5) == "[00:00:00.000] "


def test_format_line_prefixes_the_text() -> None:
    stamper = Timestamper(TimestampMode.ELAPSED, started_at=BASE)
    assert stamper.format_line("Switch>", BASE + 2) == "[00:00:02.000] Switch>"
