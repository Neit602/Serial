"""A scripted console that behaves like a network switch.

The PRD's M2 exit criterion is "configure a real Cisco switch from the TUI on
each OS", which CI cannot do.  This is the closest honest substitute: a device
on the far end of a real (socat) serial pair that echoes like a console, walks
the user > enable > configure prompts, records the configuration it is given,
and notices a break condition.

It is deliberately *not* a Cisco emulator — it implements only the behaviours
Portside's features have to cope with: echo, prompts that appear after a delay,
a password step, and a break that returns it to the boot monitor.
"""

from __future__ import annotations

import threading
from dataclasses import dataclass, field

import serial


@dataclass
class FakeSwitch:
    """Runs on a serial port and answers like a console would.

    Args:
        port: the device path to listen on (one end of a socat pair).
        prompt_delay: how long the device "thinks" before printing its prompt,
            which is what makes wait-for-prompt pacing meaningful.
    """

    port: str
    prompt_delay: float = 0.0
    hostname: str = "Switch"
    echo: bool = True

    config_lines: list[str] = field(default_factory=list)
    commands: list[str] = field(default_factory=list)
    breaks_seen: int = 0

    _serial: serial.Serial | None = field(default=None, init=False)
    _thread: threading.Thread | None = field(default=None, init=False)
    _stop: threading.Event = field(default_factory=threading.Event, init=False)
    _mode: str = field(default="user", init=False)
    _buffer: bytearray = field(default_factory=bytearray, init=False)

    # -- lifecycle ---------------------------------------------------------

    def start(self) -> None:
        """Open the port and start answering."""
        self._serial = serial.Serial(self.port, baudrate=115200, timeout=0.05)
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="fake-switch", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        """Stop answering and close the port."""
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5)
            self._thread = None
        if self._serial is not None:
            self._serial.close()
            self._serial = None

    def __enter__(self) -> FakeSwitch:
        self.start()
        return self

    def __exit__(self, *exc: object) -> None:
        self.stop()

    # -- behaviour ---------------------------------------------------------

    @property
    def prompt(self) -> str:
        """The prompt for the mode the 'switch' is currently in."""
        if self._mode == "user":
            return f"{self.hostname}>"
        if self._mode == "enable":
            return f"{self.hostname}#"
        return f"{self.hostname}(config)#"

    def banner(self) -> None:
        """Print the first prompt, as a device does when you press Enter."""
        self._write(f"\r\n{self.prompt}")

    def _run(self) -> None:
        assert self._serial is not None
        port = self._serial
        while not self._stop.is_set():
            try:
                chunk = port.read(256)
            except (serial.SerialException, OSError):
                return
            if not chunk:
                continue
            # A break arrives as a NUL (or a framing error the driver reports
            # as one); either way the device drops to its boot monitor.
            if b"\x00" in chunk:
                self.breaks_seen += 1
                self._mode = "rommon"
                self._write("\r\n*** break detected ***\r\nrommon 1 > ")
                chunk = chunk.replace(b"\x00", b"")
                if not chunk:
                    continue
            for byte in chunk:
                self._feed_byte(byte)

    def _feed_byte(self, byte: int) -> None:
        if byte in (0x0D, 0x0A):
            if self.echo:
                self._write("\r\n")
            line = self._buffer.decode("utf-8", "replace")
            self._buffer.clear()
            self._handle_line(line.strip())
            return
        if byte in (0x08, 0x7F):
            if self._buffer:
                self._buffer.pop()
                if self.echo:
                    self._write("\b \b")
            return
        self._buffer.append(byte)
        if self.echo:
            self._write(chr(byte))

    def _handle_line(self, line: str) -> None:
        if self.prompt_delay:
            # Real gear parses before it prompts; pacing has to cope with that.
            self._stop.wait(self.prompt_delay)
        if line:
            self.commands.append(line)

        lowered = line.lower()
        if self._mode == "rommon":
            if lowered in {"boot", "reset"}:
                self._mode = "user"
                self._write(f"\r\nbooting...\r\n{self.prompt}")
            else:
                self._write("\r\nrommon 1 > ")
            return

        if lowered == "enable":
            self._write("\r\nPassword: ")
            self._mode = "password"
            return
        if self._mode == "password":
            self._mode = "enable"
            self._write(f"\r\n{self.prompt}")
            return
        if lowered in {"configure terminal", "conf t"}:
            self._mode = "config"
            self._write(
                "\r\nEnter configuration commands, one per line.  End with CNTL/Z.\r\n"
                f"{self.prompt}"
            )
            return
        if lowered in {"end", "exit"} and self._mode == "config":
            self._mode = "enable"
            self._write(f"\r\n{self.prompt}")
            return
        if self._mode == "config" and line:
            self.config_lines.append(line)
            if line.startswith("hostname "):
                self.hostname = line.split(maxsplit=1)[1]
        self._write(f"\r\n{self.prompt}")

    def _write(self, text: str) -> None:
        if self._serial is None:
            return
        try:
            self._serial.write(text.encode())
            self._serial.flush()
        except (serial.SerialException, OSError):  # pragma: no cover - port went away
            pass
