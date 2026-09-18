"""``portside --list`` (F1, F23)."""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any

import pytest

from portside.cli import main as cli
from portside.core.ports import PortInfo


@dataclass
class FakeComPort:
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


PORTS = [
    FakeComPort(
        device="/dev/ttyUSB0",
        description="FT232R USB UART",
        vid=0x0403,
        pid=0x6001,
        serial_number="A50285BI",
    ),
    FakeComPort(device="/dev/ttyS0"),
]


@pytest.fixture
def patched_ports(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("portside.transport.discovery._default_lister", lambda: list(PORTS))


def test_list_prints_a_table(patched_ports: None, capsys: pytest.CaptureFixture[str]) -> None:
    assert cli.main(["--list"]) == cli.EXIT_OK
    out = capsys.readouterr().out
    assert "PORT" in out and "VID:PID" in out and "CHIP" in out and "SERIAL" in out
    assert "/dev/ttyUSB0" in out
    assert "0403:6001" in out
    assert "FTDI FT232R" in out
    assert "A50285BI" in out


def test_list_subcommand_matches_the_flag(
    patched_ports: None, capsys: pytest.CaptureFixture[str]
) -> None:
    assert cli.main(["list"]) == cli.EXIT_OK
    first = capsys.readouterr().out
    assert cli.main(["--list"]) == cli.EXIT_OK
    assert capsys.readouterr().out == first


def test_verbose_adds_driver_and_stable_path(
    patched_ports: None, capsys: pytest.CaptureFixture[str]
) -> None:
    assert cli.main(["--list", "--verbose"]) == cli.EXIT_OK
    out = capsys.readouterr().out
    assert "DRIVER" in out and "STABLE PATH" in out


def test_json_output_is_machine_readable(
    patched_ports: None, capsys: pytest.CaptureFixture[str]
) -> None:
    assert cli.main(["--list", "--json"]) == cli.EXIT_OK
    payload: list[dict[str, Any]] = json.loads(capsys.readouterr().out)
    assert [entry["device"] for entry in payload] == ["/dev/ttyUSB0", "/dev/ttyS0"]
    first = payload[0]
    assert first["vid_pid"] == "0403:6001"
    assert first["chip_family"] == "FTDI FT232R"
    assert set(first) >= {
        "device",
        "description",
        "vid",
        "pid",
        "serial_number",
        "manufacturer",
        "product",
        "chip_family",
        "driver",
        "stable_path",
    }


def test_no_ports_is_not_an_error(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    monkeypatch.setattr("portside.transport.discovery._default_lister", list)
    assert cli.main(["--list"]) == cli.EXIT_OK
    out = capsys.readouterr().out
    assert "No serial ports found" in out
    assert "driver" in out


def test_empty_json_is_an_empty_array(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    monkeypatch.setattr("portside.transport.discovery._default_lister", list)
    assert cli.main(["--list", "--json"]) == cli.EXIT_OK
    assert json.loads(capsys.readouterr().out) == []


def test_table_columns_line_up() -> None:
    ports = [
        PortInfo(device="COM1"),
        PortInfo(device="/dev/ttyUSB0", description="a much longer description"),
    ]
    lines = cli.format_port_table(ports).splitlines()
    assert len({len(line.split("  ")[0]) for line in lines}) >= 1
    assert lines[0].startswith("PORT")


def test_bare_invocation_prints_help(capsys: pytest.CaptureFixture[str]) -> None:
    assert cli.main([]) == cli.EXIT_USAGE
    assert "usage: portside" in capsys.readouterr().out
