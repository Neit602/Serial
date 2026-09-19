"""Qt key events → serial bytes (PRD F7, F9).

The GUI twin of :mod:`portside.tui.keys`; both defer to the same
:class:`~portside.core.textio.TextPolicy` so Enter and Backspace behave
identically in the TUI and the GUI.
"""

from __future__ import annotations

from typing import Final

from PySide6.QtCore import Qt
from PySide6.QtGui import QKeyEvent

from portside.core.textio import TextPolicy

__all__ = ["qt_key_to_bytes"]

#: Cursor and editing keys in their xterm form.
_SEQUENCES: Final[dict[Qt.Key, bytes]] = {
    Qt.Key.Key_Up: b"\x1b[A",
    Qt.Key.Key_Down: b"\x1b[B",
    Qt.Key.Key_Right: b"\x1b[C",
    Qt.Key.Key_Left: b"\x1b[D",
    Qt.Key.Key_Home: b"\x1b[H",
    Qt.Key.Key_End: b"\x1b[F",
    Qt.Key.Key_Insert: b"\x1b[2~",
    Qt.Key.Key_Delete: b"\x1b[3~",
    Qt.Key.Key_Escape: b"\x1b",
    Qt.Key.Key_Tab: b"\t",
    Qt.Key.Key_Backtab: b"\x1b[Z",
    Qt.Key.Key_F1: b"\x1bOP",
    Qt.Key.Key_F2: b"\x1bOQ",
    Qt.Key.Key_F3: b"\x1bOR",
    Qt.Key.Key_F4: b"\x1bOS",
    Qt.Key.Key_F5: b"\x1b[15~",
    Qt.Key.Key_F6: b"\x1b[17~",
    Qt.Key.Key_F7: b"\x1b[18~",
    Qt.Key.Key_F8: b"\x1b[19~",
    Qt.Key.Key_F9: b"\x1b[20~",
    Qt.Key.Key_F10: b"\x1b[21~",
    Qt.Key.Key_F11: b"\x1b[23~",
    Qt.Key.Key_F12: b"\x1b[24~",
}

#: Keys the window keeps for itself; scrollback is local, not a device key.
_LOCAL_KEYS: Final = frozenset({Qt.Key.Key_PageUp, Qt.Key.Key_PageDown})


def qt_key_to_bytes(event: QKeyEvent, policy: TextPolicy) -> bytes:
    """What a key press should put on the wire, or ``b""`` for local keys."""
    key = Qt.Key(event.key())
    modifiers = event.modifiers()

    if key in _LOCAL_KEYS:
        return b""
    if key in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
        return policy.send_ending.bytes or b"\r"
    if key == Qt.Key.Key_Backspace:
        return policy.backspace.byte
    if key in _SEQUENCES:
        return _SEQUENCES[key]

    if modifiers & Qt.KeyboardModifier.ControlModifier:
        return _control_byte(key)

    text = event.text()
    if text and text.isprintable():
        return text.encode("utf-8", "replace")
    if text in {"\r", "\n", "\t"}:
        return text.encode()
    return b""


def _control_byte(key: Qt.Key) -> bytes:
    """Ctrl-A…Ctrl-Z and the classic punctuation control codes."""
    value = int(key)
    if Qt.Key.Key_A <= key <= Qt.Key.Key_Z:
        return bytes([value - int(Qt.Key.Key_A) + 1])
    # The VT100's "subtract 64" rule: Ctrl-@ is NUL, Ctrl-[ is ESC, and so on.
    if key in {
        Qt.Key.Key_At,
        Qt.Key.Key_BracketLeft,
        Qt.Key.Key_Backslash,
        Qt.Key.Key_BracketRight,
        Qt.Key.Key_AsciiCircum,
        Qt.Key.Key_Underscore,
    }:
        return bytes([value - 0x40])
    if key == Qt.Key.Key_Space:
        return b"\x00"
    return b""
