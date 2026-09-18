"""Port discovery (F1) and identity-based lookup (F2)."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

import pytest

from portside.core.errors import PortNotFoundError
from portside.core.identity import DeviceIdentity
from portside.platform import platform_for
from portside.transport.discovery import find_port, list_ports, port_from_comport
from portside.transport.factory import identity_reopener, resolve_port


@dataclass
class FakeComPort:
    """Stand-in for serial.tools.list_ports_common.ListPortInfo."""

    device: str
    description: str | None = None
    vid: int | None = None
    pid: int | None = None
    serial_number: str | None = None
    manufacturer: str | None = None
    product: str | None = None
    location: str | None = None
    interface: str | None = None
    hwid: str | None = None


FTDI = FakeComPort(
    device="/dev/ttyUSB0",
    description="FT232R USB UART",
    vid=0x0403,
    pid=0x6001,
    serial_number="A50285BI",
    manufacturer="FTDI",
    hwid="USB VID:PID=0403:6001 SER=A50285BI",
)
PLAIN = FakeComPort(device="/dev/ttyS0", description="n/a", hwid="n/a")


def lister(*ports: Any) -> Any:
    return lambda: list(ports)


def test_comport_fields_are_carried_over() -> None:
    port = port_from_comport(FTDI)
    assert port.device == "/dev/ttyUSB0"
    assert port.vid_pid == "0403:6001"
    assert port.serial_number == "A50285BI"
    assert port.chip_family == "FTDI FT232R"
    assert port.extra["hwid"].startswith("USB VID:PID")


def test_pyserial_placeholders_become_none() -> None:
    """pySerial reports unknown fields as the literal string 'n/a'."""
    port = port_from_comport(PLAIN)
    assert port.description is None
    assert port.vid_pid is None
    assert port.chip_family is None
    assert "hwid" not in port.extra


def test_list_ports_uses_the_platform_filter() -> None:
    ports = lister(
        FakeComPort(device="/dev/cu.usbserial-A50285BI", vid=0x0403, pid=0x6001),
        FakeComPort(device="/dev/tty.usbserial-A50285BI", vid=0x0403, pid=0x6001),
    )
    found = list_ports(lister=ports, platform=platform_for("macos"))
    assert [port.device for port in found] == ["/dev/cu.usbserial-A50285BI"]


def test_list_ports_orders_usb_adapters_first() -> None:
    found = list_ports(lister=lister(PLAIN, FTDI), platform=platform_for("windows"))
    assert [port.device for port in found] == ["/dev/ttyUSB0", "/dev/ttyS0"]


def test_empty_port_list_is_not_an_error() -> None:
    assert list_ports(lister=lister(), platform=platform_for("windows")) == []


def test_find_port_by_name() -> None:
    port = find_port("/dev/ttyS0", lister=lister(FTDI, PLAIN), platform=platform_for("windows"))
    assert port is not None and port.device == "/dev/ttyS0"


def test_find_port_by_identity_survives_a_rename() -> None:
    """F2: the same adapter on a different port name is still found."""
    identity = DeviceIdentity(vid=0x0403, pid=0x6001, serial_number="A50285BI")
    renamed = FakeComPort(
        device="/dev/ttyUSB7",
        description="FT232R USB UART",
        vid=0x0403,
        pid=0x6001,
        serial_number="A50285BI",
    )
    port = find_port(identity, lister=lister(PLAIN, renamed), platform=platform_for("windows"))
    assert port is not None and port.device == "/dev/ttyUSB7"


def test_find_port_returns_none_when_absent() -> None:
    assert find_port("COM99", lister=lister(FTDI), platform=platform_for("windows")) is None


def test_resolve_port_passes_urls_through_untouched() -> None:
    device, identity = resolve_port("loop://")
    assert device == "loop://"
    assert identity is None


def test_resolve_port_returns_identity_for_real_ports() -> None:
    device, identity = resolve_port(
        "/dev/ttyUSB0", lister=lister(FTDI), platform=platform_for("windows")
    )
    assert device == "/dev/ttyUSB0"
    assert identity is not None and identity.serial_number == "A50285BI"


def test_resolve_port_passes_unlisted_names_to_the_driver() -> None:
    """Enumeration misses RFCOMM nodes, PTYs and some on-board UARTs.

    Those names must still open; the driver, not the port list, decides whether
    the port exists.  Unlisted ports simply carry no identity.
    """
    device, identity = resolve_port(
        "/dev/rfcomm0", lister=lister(FTDI), platform=platform_for("linux")
    )
    assert device == "/dev/rfcomm0"
    assert identity is None


def test_identity_reopener_finds_the_device_under_a_new_name() -> None:
    from portside.core.settings import SerialSettings
    from portside.transport.loopback import LoopbackTransport

    identity = DeviceIdentity(vid=0x0403, pid=0x6001, serial_number="A50285BI")
    renamed = FakeComPort(
        device="COM9",
        description="FT232R USB UART",
        vid=0x0403,
        pid=0x6001,
        serial_number="A50285BI",
    )
    made: list[str] = []

    def factory(device: str, settings: SerialSettings) -> LoopbackTransport:
        made.append(device)
        return LoopbackTransport(device, settings)

    reopen = identity_reopener(
        identity,
        SerialSettings(),
        lister=lister(renamed),
        platform=platform_for("windows"),
        factory=factory,
    )
    transport = reopen()
    assert made == ["COM9"]
    assert transport.name == "COM9"


def test_identity_reopener_raises_while_the_device_is_absent() -> None:
    from portside.core.settings import SerialSettings

    identity = DeviceIdentity(vid=0x0403, pid=0x6001, serial_number="A50285BI")
    reopen = identity_reopener(
        identity, SerialSettings(), lister=lister(PLAIN), platform=platform_for("windows")
    )
    with pytest.raises(PortNotFoundError):
        reopen()


def test_real_discovery_runs_on_this_machine() -> None:
    """Smoke test against the actual OS: it must not raise, whatever is plugged in."""
    for port in list_ports():
        assert port.device
