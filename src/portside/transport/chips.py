"""USB VID:PID → chip family (PRD F1).

Only well-known IDs are listed; an unknown adapter reports ``None`` rather than
a guess, because a wrong chip name is worse than no chip name when the user is
trying to work out which driver they need.
"""

from __future__ import annotations

from typing import Final

__all__ = ["VENDOR_NAMES", "chip_family"]

#: (vid, pid) -> chip family.  Sources: vendor datasheets and the Linux drivers.
_CHIPS: Final[dict[tuple[int, int], str]] = {
    # FTDI
    (0x0403, 0x6001): "FTDI FT232R",
    (0x0403, 0x6010): "FTDI FT2232",
    (0x0403, 0x6011): "FTDI FT4232",
    (0x0403, 0x6014): "FTDI FT232H",
    (0x0403, 0x6015): "FTDI FT-X",
    # Silicon Labs
    (0x10C4, 0xEA60): "Silicon Labs CP2102/CP2109",
    (0x10C4, 0xEA70): "Silicon Labs CP2105",
    (0x10C4, 0xEA71): "Silicon Labs CP2108",
    # WCH
    (0x1A86, 0x7523): "WCH CH340",
    (0x1A86, 0x7522): "WCH CH340K",
    (0x1A86, 0x5523): "WCH CH341",
    (0x1A86, 0x55D4): "WCH CH9102",
    # Prolific
    (0x067B, 0x2303): "Prolific PL2303",
    (0x067B, 0x23A3): "Prolific PL2303GC",
    (0x067B, 0x23C3): "Prolific PL2303GS",
    # Common CDC-ACM boards
    (0x2E8A, 0x0005): "Raspberry Pi Pico (CDC)",
    (0x2341, 0x0043): "Arduino Uno (CDC)",
    (0x2341, 0x8036): "Arduino Leonardo (CDC)",
    (0x239A, 0x8014): "Adafruit (CDC)",
    (0x1366, 0x1015): "SEGGER J-Link (CDC)",
    (0x0483, 0x374B): "ST-LINK/V2-1 (CDC)",
    (0x303A, 0x1001): "Espressif USB-JTAG/serial",
}

#: Vendor names for adapters whose exact product ID is not in the table.
VENDOR_NAMES: Final[dict[int, str]] = {
    0x0403: "FTDI",
    0x10C4: "Silicon Labs",
    0x1A86: "WCH",
    0x067B: "Prolific",
    0x2E8A: "Raspberry Pi",
    0x2341: "Arduino",
    0x239A: "Adafruit",
    0x303A: "Espressif",
    0x0483: "STMicroelectronics",
    0x1366: "SEGGER",
    0x0525: "Linux gadget (CDC)",
    0x05AC: "Apple",
}


def chip_family(vid: int | None, pid: int | None) -> str | None:
    """Return the chip family for ``vid``/``pid``, or ``None`` if unknown.

    A known vendor with an unknown product still yields the vendor name plus
    the product ID, which is enough to search for a driver.
    """
    if vid is None or pid is None:
        return None
    known = _CHIPS.get((vid, pid))
    if known:
        return known
    vendor = VENDOR_NAMES.get(vid)
    if vendor:
        return f"{vendor} {pid:04x}"
    return None
