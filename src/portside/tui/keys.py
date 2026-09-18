"""Keyboard → serial bytes (PRD F7, F9).

Kept free of Textual so the mapping can be tested on its own and reused by the
Qt front end in M3: it takes a key name and an optional character, and returns
exactly what should go on the wire.
"""

from __future__ import annotations

from typing import Final

from portside.core.textio import TextPolicy

__all__ = ["COMMAND_KEYS", "key_to_bytes"]

#: Keys Portside keeps for its own commands, so they are never sent to the
#: device.  Function keys are used because a serial console almost never needs
#: them, while every control character belongs to the device.
COMMAND_KEYS: Final = frozenset(
    {
        "f1",
        "f2",
        "f3",
        "f4",
        "f5",
        "f6",
        "f7",
        "f8",
        "f9",
        "f10",
        "f11",
        "f12",
        "pageup",
        "pagedown",
    }
)

#: Cursor and editing keys in their xterm form, which every network device and
#: Unix host understands.
_SEQUENCES: Final[dict[str, bytes]] = {
    "up": b"\x1b[A",
    "down": b"\x1b[B",
    "right": b"\x1b[C",
    "left": b"\x1b[D",
    "home": b"\x1b[H",
    "end": b"\x1b[F",
    "insert": b"\x1b[2~",
    "delete": b"\x1b[3~",
    "escape": b"\x1b",
    "tab": b"\t",
    "space": b" ",
    "shift+tab": b"\x1b[Z",
}

#: Function keys, for completeness — these are only sent when a caller asks for
#: them explicitly (the TUI keeps them for its own commands).
_FUNCTION_KEYS: Final[dict[str, bytes]] = {
    "f1": b"\x1bOP",
    "f2": b"\x1bOQ",
    "f3": b"\x1bOR",
    "f4": b"\x1bOS",
    "f5": b"\x1b[15~",
    "f6": b"\x1b[17~",
    "f7": b"\x1b[18~",
    "f8": b"\x1b[19~",
    "f9": b"\x1b[20~",
    "f10": b"\x1b[21~",
    "f11": b"\x1b[23~",
    "f12": b"\x1b[24~",
}


def key_to_bytes(
    key: str,
    character: str | None,
    policy: TextPolicy,
    *,
    allow_function_keys: bool = False,
) -> bytes:
    """Translate one key press into bytes for the wire.

    Returns ``b""`` for keys that have no serial meaning, so the caller can
    simply ignore them.
    """
    if key == "enter":
        return policy.send_ending.bytes or b"\r"
    if key == "backspace":
        # F9: some devices want BS, others DEL; the profile decides.
        return policy.backspace.byte
    if key in _SEQUENCES:
        return _SEQUENCES[key]
    if key in _FUNCTION_KEYS:
        return _FUNCTION_KEYS[key] if allow_function_keys else b""

    if key.startswith("ctrl+"):
        return _control_byte(key.removeprefix("ctrl+"))

    if character and len(character) == 1 and character.isprintable():
        return character.encode("utf-8", "replace")
    if character and character in {"\r", "\n", "\t"}:
        return character.encode()
    return b""


def _control_byte(name: str) -> bytes:
    """Control characters: Ctrl-A…Ctrl-Z plus the classic punctuation forms."""
    if len(name) == 1:
        if "a" <= name.lower() <= "z":
            return bytes([ord(name.lower()) - ord("a") + 1])
        # Ctrl-@ is NUL, Ctrl-[ is ESC, Ctrl-\ is FS, Ctrl-] is GS, Ctrl-^ RS,
        # Ctrl-_ US: the same "subtract 64" rule the VT100 used.
        if name in {"@", "[", "\\", "]", "^", "_"}:
            return bytes([ord(name) - 0x40])
    if name == "space":  # Ctrl-Space is another way to send NUL
        return b"\x00"
    return b""
