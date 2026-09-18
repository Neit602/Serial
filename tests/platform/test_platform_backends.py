"""All three platform backends, exercised on whichever OS is running the tests."""

from __future__ import annotations

from pathlib import Path

import pytest

from portside.core.ports import PortInfo
from portside.platform import get_platform, platform_for, platform_name
from portside.platform.linux import LinuxPlatform
from portside.platform.macos import MacOSPlatform
from portside.platform.windows import WindowsPlatform

ALL = ["linux", "macos", "windows", "posix"]


@pytest.mark.parametrize(
    ("sys_platform", "expected"),
    [
        ("linux", "linux"),
        ("linux2", "linux"),
        ("darwin", "macos"),
        ("win32", "windows"),
        ("cygwin", "windows"),
        ("freebsd13", "posix"),
    ],
)
def test_platform_detection(sys_platform: str, expected: str) -> None:
    assert platform_name(sys_platform) == expected


def test_get_platform_matches_this_machine() -> None:
    assert get_platform().name == platform_name()


def test_unknown_platform_is_rejected() -> None:
    with pytest.raises(ValueError, match="unknown platform"):
        platform_for("solaris")


@pytest.mark.parametrize("name", ALL)
def test_every_backend_supplies_all_remedies(name: str) -> None:
    support = platform_for(name)
    for remedy in (
        support.permission_remedy("/dev/ttyUSB0"),
        support.busy_remedy("/dev/ttyUSB0"),
        support.not_found_remedy("/dev/ttyUSB0"),
        support.vanished_remedy("/dev/ttyUSB0"),
    ):
        assert remedy.strip()
        assert "/dev/ttyUSB0" in remedy


@pytest.mark.parametrize("name", ALL)
def test_config_and_log_dirs_are_absolute_paths(name: str) -> None:
    support = platform_for(name)
    assert isinstance(support.config_dir(), Path)
    assert support.config_dir().is_absolute()
    assert support.log_dir().is_absolute()


# -- macOS ---------------------------------------------------------------


def test_macos_lists_callout_devices_and_hides_dial_in() -> None:
    """PRD: /dev/cu.* only — /dev/tty.* blocks on DCD and looks like a hang."""
    ports = [
        PortInfo(device="/dev/cu.usbserial-A50285BI"),
        PortInfo(device="/dev/tty.usbserial-A50285BI"),
        PortInfo(device="/dev/cu.Bluetooth-Incoming-Port"),
        PortInfo(device="/dev/tty.Bluetooth-Incoming-Port"),
    ]
    kept = [port.device for port in MacOSPlatform().filter_ports(ports)]
    assert kept == ["/dev/cu.usbserial-A50285BI", "/dev/cu.Bluetooth-Incoming-Port"]


# -- Windows -------------------------------------------------------------


def test_windows_keeps_every_com_port_including_above_com9() -> None:
    ports = [PortInfo(device=f"COM{n}") for n in (1, 9, 10, 256)]
    assert WindowsPlatform().filter_ports(ports) == ports


def test_windows_sorts_com_ports_naturally() -> None:
    support = WindowsPlatform()
    ports = [PortInfo(device="COM10"), PortInfo(device="COM2"), PortInfo(device="COM1")]
    ordered = [port.device for port in sorted(ports, key=support.sort_key)]
    assert ordered == ["COM1", "COM2", "COM10"]


def test_windows_not_found_remedy_explains_com_naming() -> None:
    assert "COM10" in WindowsPlatform().not_found_remedy("COM10")


# -- Linux ---------------------------------------------------------------


def test_linux_permission_remedy_gives_the_exact_command(tmp_path: Path) -> None:
    """F6: Linux users must get a command they can paste."""
    remedy = LinuxPlatform().permission_remedy("/dev/ttyUSB0")
    assert "sudo usermod -aG dialout $USER" in remedy
    assert "log out" in remedy.lower()


def test_linux_permission_remedy_names_the_group_that_owns_the_node(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Arch and friends use uucp, not dialout — say what is actually true."""
    support = LinuxPlatform()
    monkeypatch.setattr(support, "device_group", lambda port: "uucp")
    remedy = support.permission_remedy("/dev/ttyUSB0")
    assert "sudo usermod -aG uucp $USER" in remedy


def test_linux_device_group_falls_back_when_the_node_is_missing() -> None:
    assert LinuxPlatform().device_group("/dev/definitely-not-here") == "dialout"


def test_linux_finds_the_stable_by_id_path(tmp_path: Path) -> None:
    device = tmp_path / "ttyUSB0"
    device.write_text("")
    by_id = tmp_path / "by-id"
    by_id.mkdir()
    link = by_id / "usb-FTDI_FT232R_A50285BI-if00-port0"
    link.symlink_to(device)

    support = LinuxPlatform(by_id=by_id, sys_tty=tmp_path / "sys")
    enriched = support.enrich_port(PortInfo(device=str(device)))
    assert enriched.stable_path == str(link)


def test_linux_reads_the_bound_driver_from_sysfs(tmp_path: Path) -> None:
    sys_tty = tmp_path / "sys"
    (sys_tty / "ttyUSB0" / "device").mkdir(parents=True)
    driver = tmp_path / "drivers" / "ftdi_sio"
    driver.mkdir(parents=True)
    (sys_tty / "ttyUSB0" / "device" / "driver").symlink_to(driver)

    support = LinuxPlatform(by_id=tmp_path / "missing", sys_tty=sys_tty)
    enriched = support.enrich_port(PortInfo(device="/dev/ttyUSB0"))
    assert enriched.driver == "ftdi_sio"


def test_linux_enrichment_survives_a_missing_sysfs(tmp_path: Path) -> None:
    support = LinuxPlatform(by_id=tmp_path / "nope", sys_tty=tmp_path / "nope")
    enriched = support.enrich_port(PortInfo(device="/dev/ttyUSB0"))
    assert enriched.driver is None
    assert enriched.stable_path is None


def test_usb_ports_sort_before_plain_uarts() -> None:
    support = platform_for("linux")
    ports = [
        PortInfo(device="/dev/ttyS0"),
        PortInfo(device="/dev/ttyUSB0", vid=0x0403, pid=0x6001),
    ]
    ordered = [port.device for port in sorted(ports, key=support.sort_key)]
    assert ordered == ["/dev/ttyUSB0", "/dev/ttyS0"]
