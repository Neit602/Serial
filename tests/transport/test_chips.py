"""VID:PID to chip family (F1)."""

from __future__ import annotations

import pytest

from portside.transport.chips import chip_family


@pytest.mark.parametrize(
    ("vid", "pid", "expected"),
    [
        (0x0403, 0x6001, "FTDI FT232R"),
        (0x10C4, 0xEA60, "Silicon Labs CP2102/CP2109"),
        (0x1A86, 0x7523, "WCH CH340"),
        (0x067B, 0x2303, "Prolific PL2303"),
        (0x2E8A, 0x0005, "Raspberry Pi Pico (CDC)"),
    ],
)
def test_known_chips_are_named(vid: int, pid: int, expected: str) -> None:
    assert chip_family(vid, pid) == expected


def test_known_vendor_unknown_product_names_the_vendor() -> None:
    assert chip_family(0x0403, 0x1234) == "FTDI 1234"


def test_unknown_vendor_is_not_guessed() -> None:
    assert chip_family(0xDEAD, 0xBEEF) is None


def test_missing_ids_are_not_guessed() -> None:
    assert chip_family(None, None) is None
    assert chip_family(0x0403, None) is None
