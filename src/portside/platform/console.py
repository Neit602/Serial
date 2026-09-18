"""Raw terminal I/O for the CLI's pass-through mode.

The POSIX and Windows implementations have the same interface so
:mod:`portside.cli` contains no platform branches.  Both restore the terminal
in ``__exit__``, including on exceptions and Ctrl-C.
"""

from __future__ import annotations

import os
import sys
from abc import ABC, abstractmethod
from types import TracebackType
from typing import IO, Any, Self

__all__ = ["RawConsole", "create_console"]


class RawConsole(ABC):
    """Byte-level access to the controlling terminal."""

    def __init__(self) -> None:
        self._entered = False

    @property
    @abstractmethod
    def is_tty(self) -> bool:
        """True if stdin is an interactive terminal."""

    @abstractmethod
    def read(self, timeout: float = 0.1) -> bytes | None:
        """Read what is available within ``timeout``.

        Returns ``b""`` on timeout and ``None`` at end of input (Ctrl-D, or a
        closed pipe), which the CLI treats as "stop forwarding input".
        """

    def write(self, data: bytes) -> None:
        """Write bytes straight to stdout and flush."""
        stream: IO[bytes] = sys.stdout.buffer
        stream.write(data)
        stream.flush()

    def enter_raw(self) -> None:
        """Switch the terminal to raw mode (no-op when not a tty)."""

    def restore(self) -> None:
        """Restore the terminal to the mode it was in."""

    def __enter__(self) -> Self:
        self.enter_raw()
        self._entered = True
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        if self._entered:
            self.restore()
            self._entered = False


class PosixRawConsole(RawConsole):
    """termios/select based console for Linux, macOS and the BSDs."""

    def __init__(self, stdin: IO[bytes] | None = None) -> None:
        super().__init__()
        self._stdin = stdin if stdin is not None else sys.stdin.buffer
        self._saved: Any | None = None

    @property
    def _fd(self) -> int:
        return self._stdin.fileno()

    @property
    def is_tty(self) -> bool:
        try:
            return os.isatty(self._fd)
        except (OSError, ValueError):
            return False

    def enter_raw(self) -> None:
        if not self.is_tty:
            return
        import termios
        import tty

        self._saved = termios.tcgetattr(self._fd)
        tty.setraw(self._fd)

    def restore(self) -> None:
        if self._saved is None:
            return
        import termios

        try:
            termios.tcsetattr(self._fd, termios.TCSADRAIN, self._saved)
        finally:
            self._saved = None

    def read(self, timeout: float = 0.1) -> bytes | None:
        import select

        try:
            ready, _, _ = select.select([self._fd], [], [], timeout)
        except (OSError, ValueError):
            return None
        if not ready:
            return b""
        try:
            data = os.read(self._fd, 4096)
        except OSError:
            return None
        if not data:
            return None
        return data


class WindowsRawConsole(RawConsole):
    """msvcrt based console for Windows.

    Windows has no ``select`` on file handles, so keystrokes are polled with
    ``msvcrt.kbhit``; piped (non-tty) input falls back to blocking reads on
    stdin, which is what scripts and the CI end-to-end test use.
    """

    def __init__(self, stdin: IO[bytes] | None = None) -> None:
        super().__init__()
        self._stdin = stdin if stdin is not None else sys.stdin.buffer
        self._msvcrt: Any | None = None

    def _module(self) -> Any:
        if self._msvcrt is None:
            import msvcrt

            self._msvcrt = msvcrt
        return self._msvcrt

    @property
    def is_tty(self) -> bool:
        try:
            return os.isatty(self._stdin.fileno())
        except (OSError, ValueError, AttributeError):
            return False

    def read(self, timeout: float = 0.1) -> bytes | None:
        if not self.is_tty:
            data = self._stdin.read(4096)
            return data if data else None
        import time

        msvcrt = self._module()
        deadline = time.monotonic() + timeout
        chunks = bytearray()
        while True:
            while msvcrt.kbhit():
                chunks.extend(msvcrt.getch())
            if chunks:
                return bytes(chunks)
            if time.monotonic() >= deadline:
                return b""
            time.sleep(0.005)


def create_console(
    platform_name: str | None = None, *, stdin: IO[bytes] | None = None
) -> RawConsole:
    """The console implementation for this platform."""
    from portside.platform import platform_name as current

    name = platform_name or current()
    if name == "windows":
        return WindowsRawConsole(stdin)
    return PosixRawConsole(stdin)
