"""Line discipline: endings, echo, backspace, XON/XOFF (F9, F14)."""

from __future__ import annotations

import pytest
from hypothesis import given
from hypothesis import strategies as st

from portside.core.textio import XOFF, XON, Backspace, LineEnding, TextPolicy, XonXoffGate

ALL_ENDINGS = list(LineEnding)


# -- sending -------------------------------------------------------------


@pytest.mark.parametrize(
    ("ending", "expected"),
    [
        (LineEnding.CR, b"conf t\r"),
        (LineEnding.LF, b"conf t\n"),
        (LineEnding.CRLF, b"conf t\r\n"),
        (LineEnding.NONE, b"conf t"),
    ],
)
def test_encode_line_appends_the_configured_ending(ending: LineEnding, expected: bytes) -> None:
    assert TextPolicy(send_ending=ending).encode_line("conf t") == expected


@pytest.mark.parametrize("ending", [e for e in ALL_ENDINGS if e is not LineEnding.NONE])
def test_any_source_convention_is_normalised_on_send(ending: LineEnding) -> None:
    """A config file from any OS must reach the device the same way (F9)."""
    policy = TextPolicy(send_ending=ending)
    from_unix = policy.translate_output(b"a\nb\nc")
    from_windows = policy.translate_output(b"a\r\nb\r\nc")
    from_mac_classic = policy.translate_output(b"a\rb\rc")
    assert from_unix == from_windows == from_mac_classic


def test_crlf_is_one_break_not_two() -> None:
    assert TextPolicy(send_ending=LineEnding.LF).translate_output(b"a\r\nb") == b"a\nb"


def test_consecutive_breaks_are_preserved() -> None:
    """A blank line in a pasted config is meaningful; it must survive."""
    assert TextPolicy(send_ending=LineEnding.CR).translate_output(b"a\n\nb") == b"a\r\rb"


def test_none_passes_data_through_untouched() -> None:
    data = b"raw \r\n bytes \r stay \n as-is"
    assert TextPolicy(send_ending=LineEnding.NONE).translate_output(data) == data


def test_non_line_break_bytes_are_untouched() -> None:
    payload = bytes(range(0x20, 0x7F))
    assert TextPolicy(send_ending=LineEnding.CRLF).translate_output(payload) == payload


# -- receiving -----------------------------------------------------------


def test_received_endings_can_be_normalised() -> None:
    policy = TextPolicy(receive_ending=LineEnding.LF)
    assert policy.translate_input(b"line1\r\nline2\r") == b"line1\nline2\n"


def test_receive_defaults_to_passing_data_through() -> None:
    """Default is 'none': the emulator handles CR and LF itself."""
    data = b"Switch>\r\n"
    assert TextPolicy().translate_input(data) == data


# -- echo and backspace --------------------------------------------------


def test_local_echo_is_off_by_default() -> None:
    assert TextPolicy().echo_for(b"abc") == b""


def test_local_echo_returns_what_was_typed() -> None:
    assert TextPolicy(local_echo=True).echo_for(b"abc") == b"abc"


@pytest.mark.parametrize(
    ("setting", "expected"), [(Backspace.BS, b"\x08"), (Backspace.DEL, b"\x7f")]
)
def test_backspace_sends_the_configured_byte(setting: Backspace, expected: bytes) -> None:
    assert TextPolicy(backspace=setting).backspace.byte == expected


def test_defaults_are_identical_on_every_platform() -> None:
    """PRD rule: line endings are a profile setting, never an OS default."""
    policy = TextPolicy()
    assert policy.send_ending is LineEnding.CR
    assert policy.receive_ending is LineEnding.NONE
    assert policy.backspace is Backspace.DEL
    assert policy.local_echo is False


def test_policy_is_immutable() -> None:
    from dataclasses import FrozenInstanceError

    with pytest.raises(FrozenInstanceError):
        TextPolicy().local_echo = True  # type: ignore[misc]


# -- properties ----------------------------------------------------------


# The range stops below the surrogates: they are not valid scalars and cannot
# be encoded, so they say nothing about line-ending handling.
@given(
    text=st.text(
        alphabet=st.characters(min_codepoint=1, max_codepoint=0xD7FF, blacklist_characters="\r\n"),
        max_size=50,
    )
)
def test_encode_line_round_trips_the_text(text: str) -> None:
    encoded = TextPolicy(send_ending=LineEnding.CRLF).encode_line(text)
    assert encoded.endswith(b"\r\n")
    assert encoded[:-2].decode("utf-8", "replace") == text


@given(data=st.binary(max_size=200))
def test_translation_is_idempotent(data: bytes) -> None:
    """Translating already-translated data must not double the endings."""
    policy = TextPolicy(send_ending=LineEnding.CRLF)
    once = policy.translate_output(data)
    assert policy.translate_output(once) == once


@given(data=st.binary(max_size=200))
def test_line_count_is_preserved_across_translation(data: bytes) -> None:
    policy = TextPolicy(send_ending=LineEnding.LF)
    breaks = data.replace(b"\r\n", b"\n").count(b"\n") + data.replace(b"\r\n", b"\n").count(b"\r")
    assert policy.translate_output(data).count(b"\n") == breaks


# -- XON/XOFF (F14) ------------------------------------------------------


def test_gate_is_inert_when_disabled() -> None:
    gate = XonXoffGate(enabled=False)
    gate.observe(bytes([XOFF]), now=0.0)
    assert gate.is_paused(now=0.0) is False


def test_xoff_pauses_and_xon_resumes() -> None:
    gate = XonXoffGate(enabled=True)
    gate.observe(bytes([XOFF]), now=1.0)
    assert gate.is_paused(now=1.0) is True
    gate.observe(bytes([XON]), now=2.0)
    assert gate.is_paused(now=2.0) is False


def test_flow_control_bytes_inside_a_stream_are_seen() -> None:
    gate = XonXoffGate(enabled=True)
    gate.observe(b"output" + bytes([XOFF]) + b"more", now=1.0)
    assert gate.is_paused(now=1.0) is True


def test_a_lost_xon_expires_instead_of_wedging_the_session() -> None:
    """A stuck terminal is worse than a possible overrun; the count shows it."""
    gate = XonXoffGate(enabled=True, max_pause=5.0)
    gate.observe(bytes([XOFF]), now=0.0)
    assert gate.is_paused(now=4.9) is True
    assert gate.is_paused(now=5.0) is False
    assert gate.timeouts == 1


def test_manual_resume() -> None:
    gate = XonXoffGate(enabled=True)
    gate.observe(bytes([XOFF]), now=0.0)
    gate.resume()
    assert gate.is_paused(now=0.1) is False


def test_the_last_flow_control_byte_wins() -> None:
    gate = XonXoffGate(enabled=True)
    gate.observe(bytes([XOFF, XON, XOFF]), now=0.0)
    assert gate.is_paused(now=0.0) is True
