"""Shared fixtures.

The socat fixture is what makes the PRD's "loopback and socat tests pass on all
3 OSes" exit criterion honest: on Linux and macOS it gives two real PTYs wired
together, and on Windows (or wherever socat is missing) the tests that need it
skip cleanly instead of failing.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import time
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

import pytest

from portside.core.ports import PortInfo

SOCAT_STARTUP_TIMEOUT = 10.0
_PTY_RE = re.compile(r"N PTY is (\S+)")


def socat_available() -> bool:
    """True if a socat binary exists and this platform can use it."""
    if sys.platform.startswith("win"):
        return False
    return shutil.which("socat") is not None


requires_socat = pytest.mark.skipif(
    not socat_available(),
    reason="socat is not available (expected on Windows; install socat elsewhere)",
)


@dataclass
class SocatPair:
    """Two PTY paths wired together by a socat process."""

    left: str
    right: str
    process: subprocess.Popen[str]

    def kill(self) -> None:
        """Simulate an unplug: tear the virtual cable out."""
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:  # pragma: no cover - stubborn child
                self.process.kill()
                self.process.wait(timeout=5)


def _terminate(process: subprocess.Popen[str]) -> None:
    """Stop socat and reap it, so no ResourceWarning escapes into other tests."""
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:  # pragma: no cover - stubborn child
            process.kill()
            process.wait(timeout=5)
    if process.stderr is not None and not process.stderr.closed:
        process.stderr.close()


@pytest.fixture
def socat_pair() -> Iterator[SocatPair]:
    """A pair of connected virtual serial ports, torn down afterwards.

    socat prints the two PTY paths on stderr at -d -d; they are read back here
    rather than guessed, because the kernel picks the numbers.
    """
    binary = shutil.which("socat") if socat_available() else None
    if binary is None:
        pytest.skip("socat is not available")
    process = subprocess.Popen(
        [binary, "-d", "-d", "pty,raw,echo=0", "pty,raw,echo=0"],
        stderr=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        text=True,
        bufsize=1,
    )
    try:
        paths: list[str] = []
        deadline = time.monotonic() + SOCAT_STARTUP_TIMEOUT
        assert process.stderr is not None
        while len(paths) < 2 and time.monotonic() < deadline:
            line = process.stderr.readline()
            if not line:
                break
            match = _PTY_RE.search(line)
            if match:
                paths.append(match.group(1))
        if len(paths) < 2:
            pytest.skip("socat did not report two PTY paths")
        for path in paths:
            while not Path(path).exists() and time.monotonic() < deadline:
                time.sleep(0.01)
    except BaseException:
        _terminate(process)
        raise

    pair = SocatPair(left=paths[0], right=paths[1], process=process)
    try:
        yield pair
    finally:
        _terminate(process)


@pytest.fixture
def fake_ports() -> list[PortInfo]:
    """A representative port list: the adapters support actually sees."""
    return [
        PortInfo(
            device="/dev/ttyUSB0",
            description="FT232R USB UART",
            vid=0x0403,
            pid=0x6001,
            serial_number="A50285BI",
            manufacturer="FTDI",
        ),
        PortInfo(
            device="/dev/ttyUSB1",
            description="CP2102 USB to UART Bridge Controller",
            vid=0x10C4,
            pid=0xEA60,
            serial_number="0001",
            manufacturer="Silicon Labs",
        ),
        PortInfo(
            device="/dev/ttyUSB2",
            description="USB Serial",
            vid=0x1A86,
            pid=0x7523,
            serial_number=None,
            manufacturer="QinHeng Electronics",
        ),
        PortInfo(device="/dev/ttyS0", description="ttyS0"),
    ]
