"""Line discipline: endings, local echo, backspace, XON/XOFF (PRD F9, F14).

Per the PRD these are **per-profile settings, never OS defaults**: a session
opened with the same profile must behave identically on Linux, macOS and
Windows, whatever the host's own line-ending convention is.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum
from typing import Final

__all__ = [
    "XOFF",
    "XON",
    "Backspace",
    "LineEnding",
    "TextPolicy",
    "XonXoffGate",
]

#: DC3 / DC1, the software flow-control pair (F14).
XOFF: Final = 0x13
XON: Final = 0x11


class LineEnding(StrEnum):
    """What a line break becomes on the wire, and what to do with one received."""

    CR = "cr"
    LF = "lf"
    CRLF = "crlf"
    NONE = "none"

    @property
    def bytes(self) -> bytes:
        """The byte sequence this ending stands for."""
        return _ENDING_BYTES[self]


_ENDING_BYTES: Final[dict[LineEnding, bytes]] = {
    LineEnding.CR: b"\r",
    LineEnding.LF: b"\n",
    LineEnding.CRLF: b"\r\n",
    LineEnding.NONE: b"",
}


class Backspace(StrEnum):
    """What the backspace key sends (F9).

    Cisco and most Unix hosts expect DEL; some embedded monitors expect BS.
    Getting this wrong makes editing a command line impossible, so it is
    explicit rather than guessed.
    """

    BS = "bs"
    DEL = "del"

    @property
    def byte(self) -> bytes:
        """The byte the backspace key sends."""
        return b"\x08" if self is Backspace.BS else b"\x7f"


@dataclass(frozen=True, slots=True)
class TextPolicy:
    """Line-discipline settings for one session (F9).

    Args:
        send_ending: what Enter sends.  CR is the serial-console norm.
        receive_ending: how received line breaks are normalised for the screen
            and the text log; ``none`` passes them through untouched.
        local_echo: echo typed characters locally, for devices that do not.
        backspace: what the backspace key sends.
    """

    send_ending: LineEnding = LineEnding.CR
    receive_ending: LineEnding = LineEnding.NONE
    local_echo: bool = False
    backspace: Backspace = Backspace.DEL

    # -- sending -----------------------------------------------------------

    def encode_line(self, line: str, encoding: str = "utf-8") -> bytes:
        """Encode one line plus the configured ending, ready for the wire."""
        return line.encode(encoding, errors="replace") + self.send_ending.bytes

    def translate_output(self, data: bytes) -> bytes:
        """Rewrite line breaks in outgoing data to the configured ending.

        Any of CRLF, CR or LF in the source is treated as one line break, so a
        file pasted from any platform reaches the device the same way.
        """
        if self.send_ending is LineEnding.NONE:
            return data
        return self._normalise(data, self.send_ending.bytes)

    # -- receiving ---------------------------------------------------------

    def translate_input(self, data: bytes) -> bytes:
        """Normalise received line breaks; ``none`` leaves the stream untouched."""
        if self.receive_ending is LineEnding.NONE:
            return data
        return self._normalise(data, self.receive_ending.bytes)

    @staticmethod
    def _normalise(data: bytes, ending: bytes) -> bytes:
        """Collapse CRLF/CR/LF to one ending, without merging separate breaks."""
        out = bytearray()
        index = 0
        length = len(data)
        while index < length:
            byte = data[index]
            if byte == 0x0D:
                out += ending
                # A CR followed by LF is one break, not two.
                index += 2 if index + 1 < length and data[index + 1] == 0x0A else 1
                continue
            if byte == 0x0A:
                out += ending
                index += 1
                continue
            out.append(byte)
            index += 1
        return bytes(out)

    def echo_for(self, data: bytes) -> bytes:
        """What local echo should display for ``data`` (empty when echo is off)."""
        return data if self.local_echo else b""


class XonXoffGate:
    """Software flow control for drivers that do not implement it (F14).

    A received XOFF pauses sending; XON resumes it.  A lost XON would otherwise
    wedge the session forever, so the pause expires after ``max_pause`` seconds
    and says so.
    """

    def __init__(self, *, enabled: bool = False, max_pause: float = 30.0) -> None:
        self._enabled = enabled
        self._max_pause = max_pause
        self._paused_at: float | None = None
        self.timeouts = 0

    @property
    def enabled(self) -> bool:
        """Whether the gate acts on XON/XOFF at all."""
        return self._enabled

    def observe(self, data: bytes, now: float) -> None:
        """Update the gate from data received from the device."""
        if not self._enabled:
            return
        for byte in data:
            if byte == XOFF:
                self._paused_at = now
            elif byte == XON:
                self._paused_at = None

    def is_paused(self, now: float) -> bool:
        """Whether sending should be held right now."""
        if not self._enabled or self._paused_at is None:
            return False
        if now - self._paused_at >= self._max_pause:
            # Treat a lost XON as "resume": a stuck terminal is worse than a
            # possible overrun, and the count makes the event visible.
            self._paused_at = None
            self.timeouts += 1
            return False
        return True

    def resume(self) -> None:
        """Force a resume (the user pressing Ctrl-Q, or a reconnect)."""
        self._paused_at = None
