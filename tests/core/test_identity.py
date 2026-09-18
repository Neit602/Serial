"""Device identity survives port renames (F2)."""

from __future__ import annotations

from portside.core.identity import DeviceIdentity
from portside.core.ports import PortInfo

FTDI = PortInfo(
    device="/dev/ttyUSB0",
    description="FT232R USB UART",
    vid=0x0403,
    pid=0x6001,
    serial_number="A50285BI",
)


def test_same_adapter_on_a_different_port_name_has_the_same_key() -> None:
    linux = DeviceIdentity.from_port(FTDI)
    from dataclasses import replace

    windows = DeviceIdentity.from_port(replace(FTDI, device="COM7"))
    assert linux == windows
    assert linux.key() == windows.key() == "usb_0403_6001_A50285BI"


def test_two_identical_chips_with_different_serials_differ() -> None:
    from dataclasses import replace

    first = DeviceIdentity.from_port(FTDI)
    second = DeviceIdentity.from_port(replace(FTDI, serial_number="A50285XX"))
    assert first != second
    assert first.key() != second.key()


def test_adapter_without_serial_number_degrades_to_a_weak_identity() -> None:
    port = PortInfo(device="/dev/ttyUSB2", description="USB Serial", vid=0x1A86, pid=0x7523)
    identity = DeviceIdentity.from_port(port)
    assert identity.weak is True
    assert identity.serial_number is None
    assert identity.key() == "usb_1a86_7523_desc_USB_Serial"


def test_non_usb_port_identity_falls_back_to_the_port_name() -> None:
    identity = DeviceIdentity.from_port(PortInfo(device="/dev/ttyS0"))
    assert identity.weak is True
    assert identity.key() == "port_dev_ttyS0"


def test_key_is_safe_as_a_toml_bare_key() -> None:
    port = PortInfo(
        device="COM3",
        description="Weird / device: name",
        vid=0x0403,
        pid=0x6001,
        serial_number="serial with spaces",
    )
    key = DeviceIdentity.from_port(port).key()
    assert all(char.isalnum() or char in "_.-" for char in key), key


def test_matches_recognises_the_same_device() -> None:
    from dataclasses import replace

    identity = DeviceIdentity.from_port(FTDI)
    assert identity.matches(replace(FTDI, device="/dev/ttyUSB9"))
    assert not identity.matches(replace(FTDI, serial_number="OTHER"))


def test_describe_is_human_readable() -> None:
    assert DeviceIdentity.from_port(FTDI).describe() == "USB 0403:6001 serial A50285BI"
