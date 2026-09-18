"""Incremental byte→text decoding (PRD F11, and the high-bit option of F10).

Serial data arrives in arbitrary chunks, so a multi-byte character is regularly
split across two reads.  A stateful decoder is therefore not a nicety: decoding
each chunk independently would corrupt every UTF-8 character that straddles a
read boundary.

Undecodable bytes become U+FFFD.  A terminal that raises on bad input is
useless on a serial line, where line noise and wrong baud rates are routine.
"""

from __future__ import annotations

import codecs
from enum import StrEnum
from typing import Final

from portside.core.errors import InvalidSettingsError

__all__ = ["CharacterSet", "IncrementalTextDecoder"]


class CharacterSet(StrEnum):
    """Character sets Portside can decode (PRD F11)."""

    UTF8 = "utf-8"
    ASCII = "ascii"
    LATIN1 = "latin-1"
    CP437 = "cp437"
    CP850 = "cp850"

    @property
    def codec(self) -> str:
        """Name of the Python codec behind this character set."""
        return _CODECS[self]


_CODECS: Final[dict[CharacterSet, str]] = {
    CharacterSet.UTF8: "utf-8",
    CharacterSet.ASCII: "ascii",
    CharacterSet.LATIN1: "latin-1",
    CharacterSet.CP437: "cp437",
    CharacterSet.CP850: "cp850",
}

#: Bytes below 0x20 that a terminal must see even though they are "control":
#: pyte interprets them, so they are never replaced by the decoder.
_KEEP_CONTROL: Final = frozenset(range(0x00, 0x20)) | {0x7F}


class IncrementalTextDecoder:
    """Decode a byte stream to text, one chunk at a time.

    Args:
        charset: the character set to decode with.
        strip_high_bit: mask bit 7 off every byte before decoding (F10).  Some
            7-bit serial devices set it as parity or as a marker; stripping it
            turns otherwise unreadable output back into text.
    """

    def __init__(
        self,
        charset: CharacterSet = CharacterSet.UTF8,
        *,
        strip_high_bit: bool = False,
    ) -> None:
        self._charset = charset
        self._strip_high_bit = strip_high_bit
        self._decoder = self._make_decoder(charset)

    @staticmethod
    def _make_decoder(charset: CharacterSet) -> codecs.IncrementalDecoder:
        try:
            factory = codecs.getincrementaldecoder(charset.codec)
        except LookupError as exc:  # pragma: no cover - guarded by the enum
            raise InvalidSettingsError(f"unsupported character set {charset!r}") from exc
        return factory("replace")

    @property
    def charset(self) -> CharacterSet:
        """The character set in use."""
        return self._charset

    @property
    def strip_high_bit(self) -> bool:
        """Whether bit 7 is masked off before decoding."""
        return self._strip_high_bit

    def set_charset(self, charset: CharacterSet) -> None:
        """Switch character set, discarding any half-decoded character."""
        self._charset = charset
        self._decoder = self._make_decoder(charset)

    def set_strip_high_bit(self, strip: bool) -> None:
        """Turn high-bit stripping on or off (F10)."""
        self._strip_high_bit = strip

    def decode(self, data: bytes, *, final: bool = False) -> str:
        """Decode ``data``, keeping any incomplete trailing character for later.

        With ``final=True`` an incomplete trailing sequence is flushed as U+FFFD
        rather than held, which is what a session close wants.
        """
        if self._strip_high_bit:
            data = bytes(byte & 0x7F for byte in data)
        return self._decoder.decode(data, final)

    def reset(self) -> None:
        """Forget any partially decoded character (used on reconnect)."""
        self._decoder.reset()
