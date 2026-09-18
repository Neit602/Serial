"""VT100/xterm emulation, scrollback and the alternate screen (F7, F8)."""

from __future__ import annotations

from dataclasses import FrozenInstanceError

import pytest

from portside.emulation.decoder import CharacterSet
from portside.emulation.terminal import DEFAULT_SCROLLBACK, Cursor, Terminal


@pytest.fixture
def term() -> Terminal:
    return Terminal(20, 4, scrollback=50)


def text(terminal: Terminal) -> list[str]:
    return terminal.snapshot().text()


def alternate(terminal: Terminal) -> bool:
    """Read through a call so one assert does not narrow the next."""
    return terminal.alternate_screen


# -- basics --------------------------------------------------------------


def test_plain_text_lands_on_screen(term: Terminal) -> None:
    term.feed(b"Switch> show version\r\n")
    assert text(term)[0] == "Switch> show version"


def test_default_geometry_and_scrollback_match_the_prd() -> None:
    terminal = Terminal()
    assert (terminal.columns, terminal.rows) == (80, 24)
    assert terminal.scrollback == DEFAULT_SCROLLBACK == 10_000


def test_carriage_return_overwrites_the_line(term: Terminal) -> None:
    """Progress output ('50%\\r75%\\r100%') must not stack up."""
    term.feed(b"50%\r75%\r100%")
    assert text(term)[0] == "100%"


def test_backspace_erases(term: Terminal) -> None:
    term.feed(b"enabel\x08\x08le")
    assert text(term)[0] == "enable"


def test_output_wraps_at_the_screen_edge() -> None:
    terminal = Terminal(10, 3, scrollback=10)
    terminal.feed(b"0123456789abcde")
    assert text(terminal)[:2] == ["0123456789", "abcde"]


def test_tabs_advance_to_the_next_stop(term: Terminal) -> None:
    term.feed(b"a\tb")
    assert text(term)[0] == "a" + " " * 7 + "b"


# -- ANSI ----------------------------------------------------------------


def test_colours_and_bold_are_captured(term: Terminal) -> None:
    term.feed(b"\x1b[31;1mERR\x1b[0m ok")
    cells = term.snapshot().lines[0]
    assert [cell.char for cell in cells[:3]] == ["E", "R", "R"]
    assert all(cell.fg == "red" and cell.bold for cell in cells[:3])
    assert cells[4].fg == "default" and not cells[4].bold


def test_background_and_reverse_video(term: Terminal) -> None:
    term.feed(b"\x1b[44m\x1b[7mX")
    cell = term.snapshot().lines[0][0]
    assert cell.bg == "blue"
    assert cell.reverse is True


def test_underline_italics_and_strikethrough(term: Terminal) -> None:
    term.feed(b"\x1b[4mU\x1b[0m\x1b[3mI\x1b[0m\x1b[9mS")
    line = term.snapshot().lines[0]
    assert line[0].underscore and line[1].italics and line[2].strikethrough


def test_cursor_positioning(term: Terminal) -> None:
    term.feed(b"\x1b[2;5HX")
    snapshot = term.snapshot()
    assert snapshot.lines[1][4].char == "X"
    assert (snapshot.cursor.x, snapshot.cursor.y) == (5, 1)


def test_cursor_can_be_hidden_and_shown(term: Terminal) -> None:
    term.feed(b"\x1b[?25l")
    assert term.snapshot().cursor.hidden is True
    term.feed(b"\x1b[?25h")
    assert not term.snapshot().cursor.hidden


def test_erase_in_display_clears_the_screen(term: Terminal) -> None:
    term.feed(b"noise everywhere\r\nmore noise\r\n\x1b[2J\x1b[H")
    assert text(term) == ["", "", "", ""]


def test_erase_to_end_of_line(term: Terminal) -> None:
    term.feed(b"keep this\r\x1b[5C\x1b[K")
    assert text(term)[0] == "keep"


def test_dec_line_drawing_charset(term: Terminal) -> None:
    """Box drawing is how switch menus and 'show' tables render (F7)."""
    term.feed(b"\x1b(0lqqk\x1b(B")
    assert text(term)[0] == "┌──┐"


def test_line_drawing_designation_split_across_reads(term: Terminal) -> None:
    """The escape can straddle a read boundary like any other byte sequence."""
    term.feed(b"\x1b")
    term.feed(b"(0")
    term.feed(b"lqk")
    assert text(term)[0] == "┌─┐"


def test_charset_designation_is_reset_back_to_ascii(term: Terminal) -> None:
    term.feed(b"\x1b(0l\x1b(Bl")
    assert text(term)[0] == "┌l"


def test_unknown_escape_sequences_do_not_crash(term: Terminal) -> None:
    term.feed(b"\x1b[?9999h\x1b]0;title\x07\x1b[99;99;99mstill here")
    assert "still here" in text(term)[0]


# -- scrollback (F8) -----------------------------------------------------


def test_lines_scroll_into_history(term: Terminal) -> None:
    for n in range(8):
        term.feed(f"line{n}\r\n".encode())
    assert text(term)[0] == "line5"  # a 4-row screen holds the last three plus the new line
    assert term.total_lines == 8 + 1  # 8 fed lines plus the empty current line
    assert "line0" in term.all_text_lines()


def test_scrollback_is_bounded_and_keeps_the_newest() -> None:
    terminal = Terminal(20, 4, scrollback=10)
    for n in range(100):
        terminal.feed(f"line{n}\r\n".encode())
    lines = terminal.all_text_lines()
    assert len(lines) <= 10 + 4
    assert "line99" in lines
    assert "line0" not in lines


def test_scrolling_up_shows_older_lines(term: Terminal) -> None:
    for n in range(10):
        term.feed(f"line{n}\r\n".encode())
    term.scroll_by(4)
    assert term.following is False
    assert text(term)[0] == "line3"


def test_new_output_does_not_yank_a_scrolled_view_back(term: Terminal) -> None:
    """The user is reading history; new output must not steal the view (F8)."""
    for n in range(10):
        term.feed(f"line{n}\r\n".encode())
    term.scroll_by(4)
    visible = text(term)
    term.feed(b"brand new line\r\n")
    assert text(term) == visible


def test_follow_returns_to_the_live_tail(term: Terminal) -> None:
    for n in range(10):
        term.feed(f"line{n}\r\n".encode())
    term.scroll_to_top()
    term.follow()
    assert term.following is True
    assert "line9" in text(term)


def test_scrolling_is_clamped_at_both_ends(term: Terminal) -> None:
    for n in range(6):
        term.feed(f"line{n}\r\n".encode())
    term.scroll_by(10_000)
    assert term.scroll_offset == term.total_lines - term.rows
    term.scroll_by(-10_000)
    assert term.scroll_offset == 0


def test_page_scrolling_moves_by_a_screen(term: Terminal) -> None:
    for n in range(20):
        term.feed(f"line{n}\r\n".encode())
    term.scroll_page(1)
    assert term.scroll_offset == term.rows


# -- alternate screen (F7) ----------------------------------------------


def test_alternate_screen_hides_and_restores_the_primary(term: Terminal) -> None:
    term.feed(b"important output\r\n")
    term.feed(b"\x1b[?1049h")
    term.feed(b"full screen program")
    assert alternate(term)
    assert text(term)[0] == "full screen program"
    term.feed(b"\x1b[?1049l")
    assert not alternate(term)
    assert text(term)[0] == "important output"


def test_alternate_screen_output_never_enters_the_scrollback(term: Terminal) -> None:
    """A redrawing full-screen program would otherwise flood the history."""
    term.feed(b"keep me\r\n")
    term.feed(b"\x1b[?1049h")
    for n in range(50):
        term.feed(f"frame {n}\r\n".encode())
    term.feed(b"\x1b[?1049l")
    history = term.all_text_lines()
    assert "keep me" in history
    assert not any("frame" in line for line in history)


def test_alternate_screen_starts_clean_each_time(term: Terminal) -> None:
    term.feed(b"\x1b[?1049hfirst visit\x1b[?1049l")
    term.feed(b"\x1b[?1049h")
    assert text(term)[0] == ""


@pytest.mark.parametrize("mode", [b"47", b"1047", b"1049"])
def test_every_alternate_screen_mode_is_recognised(term: Terminal, mode: bytes) -> None:
    term.feed(b"\x1b[?" + mode + b"h")
    assert alternate(term)
    term.feed(b"\x1b[?" + mode + b"l")
    assert not alternate(term)


def test_scrolling_is_inert_on_the_alternate_screen(term: Terminal) -> None:
    term.feed(b"\x1b[?1049h")
    term.scroll_by(5)
    assert term.scroll_offset == 0


# -- geometry, charsets, lifecycle --------------------------------------


def test_resize_keeps_the_scrollback(term: Terminal) -> None:
    for n in range(10):
        term.feed(f"line{n}\r\n".encode())
    term.resize(40, 6)
    assert (term.columns, term.rows) == (40, 6)
    assert "line9" in term.all_text_lines()


def test_feeding_split_utf8_across_reads_renders_one_character(term: Terminal) -> None:
    term.feed(b"caf\xc3")
    term.feed(b"\xa9")
    assert text(term)[0] == "café"


def test_charset_can_be_changed_at_runtime(term: Terminal) -> None:
    term.set_charset(CharacterSet.CP437)
    term.feed(bytes([0xDB]))
    assert text(term)[0] == "█"


def test_strip_high_bit_applies_to_the_screen(term: Terminal) -> None:
    term.set_strip_high_bit(True)
    term.feed(bytes(ord(c) | 0x80 for c in "HI"))
    assert text(term)[0] == "HI"


def test_reset_clears_screen_and_history(term: Terminal) -> None:
    for n in range(10):
        term.feed(f"line{n}\r\n".encode())
    term.reset()
    assert text(term) == ["", "", "", ""]
    assert all(not line for line in term.all_text_lines())


def test_revision_changes_only_when_something_happens(term: Terminal) -> None:
    """The renderer uses this to skip idle repaints (NFR: 60 fps cap)."""
    before = term.revision
    term.feed(b"")
    assert term.revision == before
    term.feed(b"x")
    assert term.revision > before


def test_snapshot_is_immutable(term: Terminal) -> None:
    """Snapshots cross thread boundaries, so nothing about them may be edited."""
    snapshot = term.snapshot()
    with pytest.raises(FrozenInstanceError):
        snapshot.cursor = Cursor()  # type: ignore[misc]
    with pytest.raises(FrozenInstanceError):
        snapshot.lines[0][0].char = "x"  # type: ignore[misc]
    with pytest.raises(TypeError):
        snapshot.lines[0] = ()  # type: ignore[index]
