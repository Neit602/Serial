"""Key → wire byte mapping (F7, F9)."""

from __future__ import annotations

import pytest

from portside.core.textio import Backspace, LineEnding, TextPolicy
from portside.tui.keys import COMMAND_KEYS, key_to_bytes

DEFAULT = TextPolicy()


def press(key: str, character: str | None = None, policy: TextPolicy = DEFAULT) -> bytes:
    return key_to_bytes(key, character, policy)


def test_printable_characters_go_out_as_typed() -> None:
    assert press("a", "a") == b"a"
    assert press("A", "A") == b"A"
    assert press("slash", "/") == b"/"
    assert press("space", " ") == b" "


def test_non_ascii_characters_are_encoded_as_utf8() -> None:
    assert press("é", "é") == "é".encode()


@pytest.mark.parametrize(
    ("ending", "expected"),
    [(LineEnding.CR, b"\r"), (LineEnding.LF, b"\n"), (LineEnding.CRLF, b"\r\n")],
)
def test_enter_sends_the_profile_line_ending(ending: LineEnding, expected: bytes) -> None:
    assert press("enter", policy=TextPolicy(send_ending=ending)) == expected


def test_enter_never_sends_nothing() -> None:
    """A profile set to 'none' must still let the user press Enter."""
    assert press("enter", policy=TextPolicy(send_ending=LineEnding.NONE)) == b"\r"


@pytest.mark.parametrize(
    ("setting", "expected"), [(Backspace.BS, b"\x08"), (Backspace.DEL, b"\x7f")]
)
def test_backspace_follows_the_profile(setting: Backspace, expected: bytes) -> None:
    assert press("backspace", policy=TextPolicy(backspace=setting)) == expected


@pytest.mark.parametrize(
    ("key", "expected"),
    [
        ("up", b"\x1b[A"),
        ("down", b"\x1b[B"),
        ("right", b"\x1b[C"),
        ("left", b"\x1b[D"),
        ("home", b"\x1b[H"),
        ("end", b"\x1b[F"),
        ("delete", b"\x1b[3~"),
        ("insert", b"\x1b[2~"),
        ("escape", b"\x1b"),
        ("tab", b"\t"),
        ("shift+tab", b"\x1b[Z"),
    ],
)
def test_editing_and_cursor_keys_use_xterm_sequences(key: str, expected: bytes) -> None:
    assert press(key) == expected


@pytest.mark.parametrize(
    ("key", "expected"),
    [
        ("ctrl+a", b"\x01"),  # Cisco: start of line
        ("ctrl+c", b"\x03"),  # interrupt
        ("ctrl+z", b"\x1a"),  # Cisco: back to enable mode
        ("ctrl+d", b"\x04"),
        ("ctrl+[", b"\x1b"),
        ("ctrl+@", b"\x00"),
        ("ctrl+space", b"\x00"),
    ],
)
def test_control_keys_map_to_control_bytes(key: str, expected: bytes) -> None:
    """Ctrl-C and Ctrl-Z must reach the device, not the local UI."""
    assert press(key) == expected


def test_function_keys_are_reserved_for_portside_commands() -> None:
    for key in ("f1", "f5", "f10"):
        assert press(key) == b"", "function keys must not reach the device"
        assert key in COMMAND_KEYS


def test_function_keys_can_still_be_sent_when_asked() -> None:
    assert key_to_bytes("f1", None, DEFAULT, allow_function_keys=True) == b"\x1bOP"
    assert key_to_bytes("f5", None, DEFAULT, allow_function_keys=True) == b"\x1b[15~"


def test_scrollback_keys_are_local() -> None:
    assert press("pageup") == b""
    assert press("pagedown") == b""


def test_unknown_keys_send_nothing() -> None:
    assert press("f24") == b""
    assert press("ctrl+shift+alt+super") == b""
    assert press("nonsense", None) == b""
