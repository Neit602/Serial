"""End-to-end CLI runs: the console script, a real subprocess, real pipes."""

from __future__ import annotations

import subprocess
import sys

import pytest

from ..conftest import SocatPair, requires_socat


def run_cli(
    *args: str, stdin: bytes = b"", timeout: float = 30.0
) -> subprocess.CompletedProcess[bytes]:
    """Run ``python -m portside`` the way a user or a script would."""
    return subprocess.run(
        [sys.executable, "-m", "portside", *args],
        input=stdin,
        capture_output=True,
        timeout=timeout,
        check=False,
    )


def test_list_runs_as_a_subprocess() -> None:
    result = run_cli("--list")
    assert result.returncode == 0
    assert b"PORT" in result.stdout or b"No serial ports found" in result.stdout


def test_list_json_is_parseable_from_a_subprocess() -> None:
    import json

    result = run_cli("--list", "--json")
    assert result.returncode == 0
    assert isinstance(json.loads(result.stdout), list)


def test_version_flag() -> None:
    result = run_cli("--version")
    assert result.returncode == 0
    assert b"portside" in result.stdout


def test_connect_over_the_loop_url_pipes_bytes_through() -> None:
    """The scripting path from the README: pipe in, read the device's reply out."""
    result = run_cli(
        "connect",
        "loop://",
        "--baud",
        "115200",
        "--quiet",
        "--drain",
        "0.5",
        stdin=b"configure terminal\r",
    )
    assert result.returncode == 0
    assert result.stdout == b"configure terminal\r"


def test_connecting_to_a_missing_port_exits_with_the_not_found_code() -> None:
    result = run_cli("connect", "/dev/portside-does-not-exist", "--baud", "9600")
    assert result.returncode == 5
    assert b"portside --list" in result.stderr


@pytest.mark.socat
@requires_socat
def test_connect_talks_to_a_real_virtual_port(socat_pair: SocatPair) -> None:
    from portside.transport.serial_transport import PySerialTransport

    with PySerialTransport(socat_pair.right) as device:
        result = run_cli(
            "connect",
            socat_pair.left,
            "--baud",
            "115200",
            "--quiet",
            "--drain",
            "0.5",
            stdin=b"hello device\r",
        )
        assert result.returncode == 0
        received = bytearray()
        import time

        deadline = time.monotonic() + 5
        while len(received) < 13 and time.monotonic() < deadline:
            received.extend(device.read(4096, 0.1))
        assert bytes(received) == b"hello device\r"
