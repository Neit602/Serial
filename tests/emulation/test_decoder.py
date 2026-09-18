"""Incremental decoding and character sets (F10, F11)."""

from __future__ import annotations

import pytest
from hypothesis import given
from hypothesis import strategies as st

from portside.emulation.decoder import CharacterSet, IncrementalTextDecoder


def test_utf8_split_across_chunks_decodes_correctly() -> None:
    """The whole point: serial data does not arrive on character boundaries."""
    decoder = IncrementalTextDecoder()
    payload = "café ☕".encode()
    pieces = [decoder.decode(payload[i : i + 1]) for i in range(len(payload))]
    assert "".join(pieces) == "café ☕"


def test_a_held_partial_character_is_not_emitted_early() -> None:
    decoder = IncrementalTextDecoder()
    assert decoder.decode(b"caf\xc3") == "caf"
    assert decoder.decode(b"\xa9") == "é"


def test_final_flush_replaces_an_incomplete_character() -> None:
    decoder = IncrementalTextDecoder()
    assert decoder.decode(b"caf\xc3", final=True) == "caf�"


@pytest.mark.parametrize(
    ("charset", "data", "expected"),
    [
        (CharacterSet.ASCII, b"plain ascii", "plain ascii"),
        (CharacterSet.LATIN1, bytes([0xE9, 0xF1]), "éñ"),
        (CharacterSet.CP437, bytes([0xB0, 0xDB]), "░█"),
        (CharacterSet.CP850, bytes([0x9B, 0x9C]), "ø£"),
        (CharacterSet.UTF8, "日本".encode(), "日本"),
    ],
)
def test_every_supported_charset_decodes(charset: CharacterSet, data: bytes, expected: str) -> None:
    assert IncrementalTextDecoder(charset).decode(data) == expected


def test_undecodable_bytes_become_replacement_characters_not_exceptions() -> None:
    """Line noise and a wrong baud rate must not kill the terminal."""
    decoder = IncrementalTextDecoder(CharacterSet.ASCII)
    assert decoder.decode(bytes([0xFF, 0xFE, 0x41])) == "��A"


def test_strip_high_bit_recovers_7_bit_output() -> None:
    """A device marking parity in bit 7 turns to mojibake without this (F10)."""
    marked = bytes(ord(char) | 0x80 for char in "HELLO")
    assert IncrementalTextDecoder(CharacterSet.ASCII, strip_high_bit=True).decode(marked) == "HELLO"


def test_strip_high_bit_can_be_toggled_at_runtime() -> None:
    decoder = IncrementalTextDecoder(CharacterSet.ASCII)
    assert decoder.strip_high_bit is False
    decoder.set_strip_high_bit(True)
    assert decoder.decode(bytes([0xC1])) == "A"


def test_switching_charset_discards_a_partial_character() -> None:
    decoder = IncrementalTextDecoder()
    assert decoder.decode(b"\xc3") == ""
    decoder.set_charset(CharacterSet.LATIN1)
    assert decoder.charset is CharacterSet.LATIN1
    assert decoder.decode(b"\xe9") == "é"


def test_reset_forgets_a_partial_character() -> None:
    decoder = IncrementalTextDecoder()
    decoder.decode(b"\xc3")
    decoder.reset()
    assert decoder.decode(b"A") == "A"


@given(data=st.binary(max_size=512))
def test_decoding_never_raises_whatever_arrives(data: bytes) -> None:
    for charset in CharacterSet:
        IncrementalTextDecoder(charset).decode(data)


@given(data=st.binary(max_size=256))
def test_single_byte_charsets_preserve_length(data: bytes) -> None:
    """Latin-1 and CP437 map every byte to exactly one character."""
    for charset in (CharacterSet.LATIN1, CharacterSet.CP437):
        assert len(IncrementalTextDecoder(charset).decode(data)) == len(data)


@given(chunks=st.lists(st.binary(max_size=8), max_size=20))
def test_chunking_does_not_change_the_result(chunks: list[bytes]) -> None:
    payload = b"".join(chunks)
    piecewise = IncrementalTextDecoder()
    incremental = "".join(piecewise.decode(chunk) for chunk in chunks)
    incremental += piecewise.decode(b"", final=True)
    whole = IncrementalTextDecoder().decode(payload, final=True)
    assert incremental == whole
