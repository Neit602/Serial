"""Scrollback search (F8) and hex rendering (F10)."""

from __future__ import annotations

import pytest
from hypothesis import given
from hypothesis import strategies as st

from portside.core.errors import InvalidSettingsError
from portside.emulation.find import Match, SearchOptions, find_all, find_next, find_prev
from portside.emulation.hexview import ViewMode, format_hex, hex_rows
from portside.emulation.terminal import Terminal

LINES = [
    "Switch> show ip interface brief",
    "GigabitEthernet1/0/1  10.0.0.1  up  up",
    "GigabitEthernet1/0/2  unassigned  down down",
    "switch uptime is 4 days",
]


# -- find ----------------------------------------------------------------


def test_literal_search_is_the_default() -> None:
    """An IP address or Gi1/0/1 must not need escaping (F8)."""
    matches = find_all(LINES, "10.0.0.1")
    assert len(matches) == 1
    assert matches[0].line == 1


def test_regex_metacharacters_are_literal_unless_asked() -> None:
    assert find_all(["a.c", "abc"], "a.c") == [Match(line=0, start=0, end=3)]


def test_search_is_case_insensitive_by_default() -> None:
    assert len(find_all(LINES, "switch")) == 2


def test_case_sensitive_search() -> None:
    matches = find_all(LINES, "switch", SearchOptions(case_sensitive=True))
    assert [match.line for match in matches] == [3]


def test_regex_search_when_requested() -> None:
    matches = find_all(LINES, r"Gi\w+Ethernet1/0/\d", SearchOptions(regex=True))
    assert [match.line for match in matches] == [1, 2]


def test_whole_word_search() -> None:
    assert find_all(["up updated", "up"], "up", SearchOptions(whole_word=True))[0].line == 0
    assert len(find_all(["up updated"], "up", SearchOptions(whole_word=True))) == 1


def test_multiple_matches_on_one_line_are_ordered() -> None:
    matches = find_all(["down down down"], "down")
    assert [match.start for match in matches] == [0, 5, 10]


def test_no_matches_and_empty_query() -> None:
    assert find_all(LINES, "nonexistent") == []
    assert find_all(LINES, "") == []


def test_invalid_regex_is_a_typed_error() -> None:
    with pytest.raises(InvalidSettingsError, match="invalid search pattern"):
        find_all(LINES, "unclosed(", SearchOptions(regex=True))


def test_next_and_previous_wrap_around() -> None:
    matches = find_all(LINES, "up")
    assert find_next(matches, None) == matches[0]
    assert find_next(matches, matches[-1]) == matches[0]
    assert find_prev(matches, None) == matches[-1]
    assert find_prev(matches, matches[0]) == matches[-1]


def test_navigation_with_no_matches_returns_nothing() -> None:
    assert find_next([], None) is None
    assert find_prev([], None) is None


def test_search_spans_the_scrollback_boundary() -> None:
    """A hit that scrolled off screen must still be findable (F8)."""
    terminal = Terminal(40, 3, scrollback=50)
    terminal.feed(b"needle in the history\r\n")
    for n in range(10):
        terminal.feed(f"filler {n}\r\n".encode())
    matches = find_all(terminal.all_text_lines(), "needle")
    assert len(matches) == 1
    assert matches[0].line == 0


# -- hex view ------------------------------------------------------------


def test_hex_dump_layout() -> None:
    rendered = format_hex(b"Hello, Portside!")
    assert rendered == (
        "00000000  48 65 6c 6c 6f 2c 20 50  6f 72 74 73 69 64 65 21  |Hello, Portside!|"
    )


def test_non_printable_bytes_show_as_dots() -> None:
    assert "|..A.|" in format_hex(bytes([0x00, 0x1B, 0x41, 0xFF]))


def test_a_partial_final_row_stays_aligned() -> None:
    """Short rows pad their hex columns, so the ASCII gutter starts in one place."""
    lines = format_hex(b"A" * 20).splitlines()
    assert len(lines) == 2
    assert [line.index("|") for line in lines] == [lines[0].index("|")] * 2
    assert lines[1].endswith("|AAAA|")


def test_offsets_continue_across_calls() -> None:
    rendered = format_hex(b"B" * 16, offset=0x40)
    assert rendered.startswith("00000040")


def test_strip_high_bit_matches_the_text_view() -> None:
    """The hex and text views must agree about what the device sent (F10)."""
    marked = bytes(ord(char) | 0x80 for char in "HI")
    assert "|HI|" in format_hex(marked, strip_high_bit=True)


def test_row_width_is_configurable() -> None:
    rows = list(hex_rows(b"12345678", width=4))
    assert [row.offset for row in rows] == [0, 4]
    assert rows[0].ascii_columns == "1234"


def test_zero_width_is_rejected() -> None:
    with pytest.raises(ValueError, match="width must be positive"):
        list(hex_rows(b"x", width=0))


def test_empty_input_renders_nothing() -> None:
    assert format_hex(b"") == ""


def test_view_modes_are_named_for_the_prd() -> None:
    assert {mode.value for mode in ViewMode} == {"text", "hex", "mixed"}


@given(data=st.binary(max_size=200))
def test_every_byte_survives_the_dump(data: bytes) -> None:
    """Round-trip: the hex columns must reproduce the input exactly."""
    recovered = bytes.fromhex("".join(row.hex_columns.replace(" ", "") for row in hex_rows(data)))
    assert recovered == data
