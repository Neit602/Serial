"""``portside`` command line front end (PRD F23, M1 subset).

Two commands:

* ``portside --list`` — every port with the details F1 asks for, optionally as
  JSON for scripts.
* ``portside connect <port> --baud N`` — raw pass-through: bytes from the
  terminal go to the port and back, with no emulation and no line-ending
  translation.  Emulation, logging and pacing arrive with the TUI in M2.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
from collections.abc import Sequence
from typing import Any

from portside import __version__
from portside.core.errors import (
    DeviceVanishedError,
    InvalidSettingsError,
    PermissionDeniedError,
    PortBusyError,
    PortNotFoundError,
    PortsideError,
)
from portside.core.events import ErrorRaised, Event, ReconnectAttempt, StateChanged
from portside.core.ports import PortInfo
from portside.core.session import ReconnectPolicy, Session, SessionState
from portside.core.settings import (
    BAUD_MAX,
    BAUD_MIN,
    DATA_BITS_CHOICES,
    FlowControl,
    Parity,
    SerialSettings,
    StopBits,
)
from portside.platform.console import RawConsole, create_console
from portside.transport.discovery import list_ports
from portside.transport.factory import identity_reopener, open_port, resolve_port

__all__ = ["main"]

EXIT_OK = 0
EXIT_FAILURE = 1
EXIT_USAGE = 2
EXIT_PERMISSION = 3
EXIT_BUSY = 4
EXIT_NOT_FOUND = 5

#: Ctrl-] leaves pass-through mode, following the telnet convention.
ESCAPE_BYTE = 0x1D

_EXIT_CODES: dict[type[PortsideError], int] = {
    PermissionDeniedError: EXIT_PERMISSION,
    PortBusyError: EXIT_BUSY,
    PortNotFoundError: EXIT_NOT_FOUND,
    DeviceVanishedError: EXIT_NOT_FOUND,
    InvalidSettingsError: EXIT_USAGE,
}


def exit_code_for(error: PortsideError) -> int:
    """Map a typed error to a process exit code scripts can branch on."""
    for kind, code in _EXIT_CODES.items():
        if isinstance(error, kind):
            return code
    return EXIT_FAILURE


# -- argument parsing -----------------------------------------------------


def _baud(value: str) -> int:
    try:
        baud = int(value, 0)
    except ValueError:
        raise argparse.ArgumentTypeError(f"{value!r} is not an integer baud rate") from None
    if not BAUD_MIN <= baud <= BAUD_MAX:
        raise argparse.ArgumentTypeError(
            f"baud rate must be between {BAUD_MIN} and {BAUD_MAX}, got {baud}"
        )
    return baud


def build_parser() -> argparse.ArgumentParser:
    """The full ``portside`` command line."""
    parser = argparse.ArgumentParser(
        prog="portside",
        description="Portside — cross-platform serial terminal.",
    )
    parser.add_argument("--version", action="version", version=f"portside {__version__}")
    parser.add_argument(
        "--list",
        action="store_true",
        dest="list_ports",
        help="list the serial ports on this machine and exit",
    )
    parser.add_argument("--json", action="store_true", help="machine-readable output for --list")
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="show stable paths and driver details"
    )

    sub = parser.add_subparsers(dest="command")
    sub.add_parser("list", help="list the serial ports on this machine")

    connect = sub.add_parser("connect", help="open a port in raw pass-through mode")
    connect.add_argument("port", help="port name (COM3, /dev/ttyUSB0) or pySerial URL (loop://)")
    connect.add_argument(
        "--baud", "-b", type=_baud, default=115200, help="baud rate (default 115200)"
    )
    connect.add_argument(
        "--data-bits", type=int, choices=DATA_BITS_CHOICES, default=8, help="data bits (default 8)"
    )
    connect.add_argument(
        "--parity",
        choices=[p.value for p in Parity],
        default=Parity.NONE.value,
        help="parity: N, E, O, M or S (default N)",
    )
    connect.add_argument(
        "--stop-bits",
        choices=["1", "1.5", "2"],
        default="1",
        help="stop bits (default 1)",
    )
    connect.add_argument(
        "--flow",
        choices=[f.value for f in FlowControl],
        default=FlowControl.NONE.value,
        help="flow control (default none)",
    )
    connect.add_argument(
        "--reconnect",
        action="store_true",
        help="reconnect automatically when the device disappears and returns (F5)",
    )
    connect.add_argument(
        "--drain",
        type=float,
        default=0.25,
        metavar="SECONDS",
        dest="drain",
        help=(
            "keep reading the port this long before exiting, so the device's last "
            "bytes are not lost (default 0.25)"
        ),
    )
    connect.add_argument("--quiet", "-q", action="store_true", help="suppress status messages")
    return parser


def settings_from_args(args: argparse.Namespace) -> SerialSettings:
    """Build validated :class:`SerialSettings` from parsed arguments (F3)."""
    return SerialSettings(
        baudrate=args.baud,
        data_bits=args.data_bits,
        parity=Parity(args.parity),
        stop_bits=StopBits(float(args.stop_bits)),
        flow_control=FlowControl(args.flow),
    )


# -- portside --list ------------------------------------------------------


def format_port_table(ports: Sequence[PortInfo], *, verbose: bool = False) -> str:
    """Render the port list as an aligned table (F1)."""
    if not ports:
        return (
            "No serial ports found.\n"
            "Plug in an adapter and try again; if it is plugged in, check that its driver "
            "is installed and that you have permission to open the port."
        )
    headers = ["PORT", "DESCRIPTION", "VID:PID", "CHIP", "SERIAL"]
    if verbose:
        headers += ["DRIVER", "STABLE PATH"]
    rows: list[list[str]] = []
    for port in ports:
        row = [
            port.device,
            port.description or "-",
            port.vid_pid or "-",
            port.chip_family or "-",
            port.serial_number or "-",
        ]
        if verbose:
            row += [port.driver or "-", port.stable_path or "-"]
        rows.append(row)
    widths = [max(len(headers[i]), *(len(row[i]) for row in rows)) for i in range(len(headers))]
    lines = ["  ".join(h.ljust(widths[i]) for i, h in enumerate(headers)).rstrip()]
    lines += [
        "  ".join(cell.ljust(widths[i]) for i, cell in enumerate(row)).rstrip() for row in rows
    ]
    return "\n".join(lines)


def cmd_list(args: argparse.Namespace, *, lister: Any = None) -> int:
    """Implement ``portside --list``."""
    ports = list_ports(lister=lister)
    if args.json:
        print(json.dumps([port.to_dict() for port in ports], indent=2))
    else:
        print(format_port_table(ports, verbose=args.verbose))
    return EXIT_OK


# -- portside connect -----------------------------------------------------


def _status(message: str, *, quiet: bool = False) -> None:
    """Status goes to stderr so piped session data on stdout stays clean."""
    if not quiet:
        print(message, file=sys.stderr, flush=True)


def _make_status_reporter(quiet: bool) -> Any:
    def report(event: Event) -> None:
        if isinstance(event, StateChanged):
            if event.new is SessionState.RECONNECTING:
                _status(f"[portside] device lost: {event.detail or 'disconnected'}", quiet=quiet)
            elif event.new is SessionState.CONNECTED and event.detail:
                _status(f"[portside] {event.detail}", quiet=quiet)
        elif isinstance(event, ReconnectAttempt):
            _status(
                f"[portside] reconnect attempt {event.attempt} in {event.delay:.1f}s", quiet=quiet
            )
        elif isinstance(event, ErrorRaised) and event.fatal:
            _status(f"[portside] error: {event.error.display()}", quiet=quiet)

    return report


async def _pump_input(session: Session, console: RawConsole, *, drain: float) -> str:
    """Terminal → port.  Returns why it stopped: ``escape``, ``eof`` or ``closed``.

    Both exit paths pause for ``drain`` seconds first, so whatever the device
    said last still reaches the screen before the output pump is stopped.
    """
    while True:
        data = await asyncio.to_thread(console.read, 0.1)
        if data is None:
            await _drain_for(drain)
            return "eof"
        if not data:
            if session.state is SessionState.CLOSED:
                return "closed"
            continue
        if console.is_tty and ESCAPE_BYTE in data:
            head = data[: data.index(bytes([ESCAPE_BYTE]))]
            if head:
                await session.send(head)
            await _drain_for(drain)
            return "escape"
        await session.send(data)


async def _drain_for(seconds: float) -> None:
    """Give the output pump a moment to print what is still in flight."""
    if seconds > 0:
        await asyncio.sleep(seconds)


async def _pump_output(session: Session, console: RawConsole) -> None:
    """Port → terminal."""
    async for chunk in session.stream():
        console.write(chunk)


async def run_connect(args: argparse.Namespace, *, console: RawConsole | None = None) -> int:
    """Open the port and pass bytes through until the user quits (F23)."""
    settings = settings_from_args(args)
    device, identity = resolve_port(args.port)
    transport = open_port(device, settings)

    reopen = None
    if args.reconnect and identity is not None:
        reopen = identity_reopener(identity, settings)
    elif args.reconnect:
        _status(
            "[portside] --reconnect needs a real port (URLs have no device identity); "
            "continuing without it.",
            quiet=args.quiet,
        )

    session = Session(
        transport,
        settings=settings,
        identity=identity,
        reconnect=ReconnectPolicy(enabled=args.reconnect and reopen is not None),
        reopen=reopen,
    )
    session.events.subscribe(_make_status_reporter(args.quiet))

    await session.open()
    con = console or create_console()
    _status(f"[portside] connected to {device} at {settings.describe()}", quiet=args.quiet)
    if con.is_tty:
        _status("[portside] press Ctrl-] to quit", quiet=args.quiet)

    try:
        with con:
            output = asyncio.create_task(_pump_output(session, con))
            user_input = asyncio.create_task(_pump_input(session, con, drain=args.drain))
            done, pending = await asyncio.wait(
                {output, user_input}, return_when=asyncio.FIRST_COMPLETED
            )
            for task in pending:
                task.cancel()
            await asyncio.gather(*pending, return_exceptions=True)
            for task in done:
                task.result()
    finally:
        await session.close()
    _status(
        f"[portside] closed {device} · in {session.bytes_in} B · out {session.bytes_out} B",
        quiet=args.quiet,
    )
    return EXIT_OK


# -- entry point ----------------------------------------------------------


def main(argv: Sequence[str] | None = None) -> int:
    """Console-script entry point.  Returns the process exit code."""
    parser = build_parser()
    args = parser.parse_args(argv)

    try:
        if args.list_ports or args.command == "list":
            return cmd_list(args)
        if args.command == "connect":
            return asyncio.run(run_connect(args))
    except PortsideError as error:
        print(f"portside: {error.display()}", file=sys.stderr)
        return exit_code_for(error)
    except KeyboardInterrupt:
        return EXIT_OK

    parser.print_help()
    return EXIT_USAGE


if __name__ == "__main__":  # pragma: no cover - module entry point
    raise SystemExit(main())
