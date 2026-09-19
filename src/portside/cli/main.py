"""``portside`` command line front end (PRD F23).

Commands:

* ``portside --list`` — every port with the details F1 asks for, optionally as
  JSON for scripts.
* ``portside connect <port> --baud N`` — opens the terminal UI when stdout is a
  terminal, giving emulation, scrollback, logging, break and paced paste.
* ``portside connect <port> --raw`` — M1's raw pass-through, with no emulation
  and no translation.  Piped input or output selects this automatically, so
  scripts behave exactly as they did before the TUI existed.
* ``portside profiles`` — list, show, export and import saved profiles (F20).
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from portside import __version__
from portside.core.breaks import BreakMode
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
from portside.core.textio import Backspace, LineEnding, TextPolicy
from portside.logging.session_log import LogFormat, LogSettings, SessionLogger
from portside.logging.timestamps import TimestampMode
from portside.platform.console import RawConsole, create_console
from portside.profiles.store import Profile, ProfileError, ProfileStore
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
    ProfileError: EXIT_USAGE,
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
        "--profile",
        "-p",
        metavar="NAME",
        help="apply a saved profile; explicit options still win over it",
    )
    connect.add_argument(
        "--raw",
        action="store_true",
        help="raw pass-through with no terminal emulation (the default when piped)",
    )
    connect.add_argument("--local-echo", action="store_true", help="echo typed characters locally")
    connect.add_argument(
        "--send-ending",
        choices=[e.value for e in LineEnding],
        help="what Enter sends (default cr)",
    )
    connect.add_argument(
        "--backspace",
        choices=[b.value for b in Backspace],
        help="what the backspace key sends (default del)",
    )
    connect.add_argument(
        "--break-mode",
        choices=[m.value for m in BreakMode],
        help="native break, emulated break, or auto (default auto)",
    )
    connect.add_argument(
        "--break-duration",
        type=float,
        metavar="SECONDS",
        help="break length in seconds (default 0.25)",
    )
    connect.add_argument("--log", metavar="PATH", help="log this session to PATH")
    connect.add_argument(
        "--log-format",
        choices=[f.value for f in LogFormat],
        default=LogFormat.TEXT.value,
        help="text (decoded, timestamped) or raw (byte-exact)",
    )
    connect.add_argument(
        "--timestamps",
        choices=[t.value for t in TimestampMode],
        help="per-line timestamps in the log (default off)",
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

    profiles = sub.add_parser("profiles", help="list, show, export and import profiles")
    profiles.add_argument(
        "action",
        nargs="?",
        default="list",
        choices=["list", "show", "export", "import", "delete"],
        help="what to do (default: list)",
    )
    profiles.add_argument("name", nargs="?", help="profile name, or path for import")
    profiles.add_argument("--file", metavar="PATH", help="file to export to or import from")

    sub.add_parser("gui", help="open the desktop application (needs the 'gui' extra)")
    return parser


def _explicitly_given(argv: Sequence[str] | None, *flags: str) -> bool:
    """Whether the user typed one of ``flags`` (rather than taking a default)."""
    if argv is None:
        argv = sys.argv[1:]
    return any(arg == flag or arg.startswith(f"{flag}=") for arg in argv for flag in flags)


def settings_from_args(
    args: argparse.Namespace,
    profile: Profile | None = None,
    argv: Sequence[str] | None = None,
) -> SerialSettings:
    """Build validated :class:`SerialSettings` from arguments and profile (F3).

    A profile supplies the defaults; anything typed on the command line wins,
    which is what makes ``--profile lab --baud 9600`` do the obvious thing.
    """
    base = profile.serial if profile else SerialSettings()
    baud = args.baud if _explicitly_given(argv, "--baud", "-b") or not profile else base.baudrate
    data_bits = (
        args.data_bits if _explicitly_given(argv, "--data-bits") or not profile else base.data_bits
    )
    parity = (
        Parity(args.parity) if _explicitly_given(argv, "--parity") or not profile else base.parity
    )
    stop_bits = (
        StopBits(float(args.stop_bits))
        if _explicitly_given(argv, "--stop-bits") or not profile
        else base.stop_bits
    )
    flow = (
        FlowControl(args.flow)
        if _explicitly_given(argv, "--flow") or not profile
        else base.flow_control
    )
    return SerialSettings(
        baudrate=baud,
        data_bits=data_bits,
        parity=parity,
        stop_bits=stop_bits,
        flow_control=flow,
    )


def text_policy_from_args(args: argparse.Namespace, profile: Profile | None = None) -> TextPolicy:
    """Line discipline from the profile, overridden by explicit flags (F9)."""
    base = profile.text if profile else TextPolicy()
    return TextPolicy(
        send_ending=LineEnding(args.send_ending) if args.send_ending else base.send_ending,
        receive_ending=base.receive_ending,
        local_echo=args.local_echo or base.local_echo,
        backspace=Backspace(args.backspace) if args.backspace else base.backspace,
    )


def logger_from_args(
    args: argparse.Namespace, profile: Profile | None, port: str
) -> SessionLogger | None:
    """Build a session logger when logging was asked for (F15, F16)."""
    profile_logging = profile.logging if profile else None
    stamps = (
        TimestampMode(args.timestamps)
        if args.timestamps
        else (profile_logging.timestamps if profile_logging else TimestampMode.OFF)
    )
    if args.log:
        target = Path(args.log)
        settings = LogSettings(
            directory=target.parent if target.parent != Path() else Path.cwd(),
            template=target.name,
            log_format=LogFormat(args.log_format),
            timestamps=stamps,
            auto_start=True,
        )
    elif profile_logging is not None and profile_logging.auto_start:
        settings = profile_logging
    else:
        return None
    return SessionLogger(settings=settings, port=port, profile=profile.name if profile else "")


def load_profile(name: str | None) -> Profile | None:
    """Load a named profile, or return ``None`` when none was requested (F20)."""
    if not name:
        return None
    return ProfileStore().load(name)


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


def use_tui(args: argparse.Namespace, console: RawConsole | None = None) -> bool:
    """Whether to open the terminal UI for this invocation (F23).

    The TUI needs a real terminal at both ends.  Piped input or output selects
    raw pass-through automatically, so every M1-era script keeps working
    unchanged.
    """
    if args.raw:
        return False
    if console is not None:
        return console.is_tty and sys.stdout.isatty()
    return sys.stdin.isatty() and sys.stdout.isatty()


async def _open_session(
    args: argparse.Namespace,
    profile: Profile | None,
    settings: SerialSettings,
) -> tuple[Session, str]:
    """Resolve the port, build the session and open it."""
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

    break_mode = (
        BreakMode(args.break_mode)
        if args.break_mode
        else (profile.break_mode if profile else BreakMode.AUTO)
    )
    session = Session(
        transport,
        settings=settings,
        identity=identity,
        reconnect=ReconnectPolicy(enabled=args.reconnect and reopen is not None),
        reopen=reopen,
        break_mode=break_mode,
    )
    session.events.subscribe(_make_status_reporter(args.quiet))
    await session.open()
    return session, device


async def run_connect(
    args: argparse.Namespace,
    *,
    console: RawConsole | None = None,
    argv: Sequence[str] | None = None,
) -> int:
    """Open the port, then run either the TUI or raw pass-through (F23)."""
    profile = load_profile(getattr(args, "profile", None))
    settings = settings_from_args(args, profile, argv)
    session, device = await _open_session(args, profile, settings)
    logger = logger_from_args(args, profile, device)

    if use_tui(args, console):
        from portside.tui.app import PortsideTUI

        duration = args.break_duration or (profile.break_duration if profile else 0.25)
        app = PortsideTUI(
            session,
            text_policy=text_policy_from_args(args, profile),
            logger=logger,
            profile=profile,
            break_duration=duration,
        )
        try:
            await app.run_async()
        finally:
            await session.close()
        return EXIT_OK

    return await _run_pass_through(args, session, device, console, logger)


async def _run_pass_through(
    args: argparse.Namespace,
    session: Session,
    device: str,
    console: RawConsole | None,
    logger: SessionLogger | None,
) -> int:
    """M1's raw mode: bytes in, bytes out, no emulation."""
    con = console or create_console()
    detach = logger.attach(session) if logger is not None else None
    if logger is not None:
        logger.start()
    _status(f"[portside] connected to {device} at {session.settings.describe()}", quiet=args.quiet)
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
        if detach is not None:
            detach()
    _status(
        f"[portside] closed {device} · in {session.bytes_in} B · out {session.bytes_out} B",
        quiet=args.quiet,
    )
    return EXIT_OK


# -- portside profiles ----------------------------------------------------


def cmd_profiles(args: argparse.Namespace, store: ProfileStore | None = None) -> int:
    """Implement ``portside profiles`` (F20)."""
    profiles = store or ProfileStore()
    action = args.action

    if action == "list":
        names = profiles.names()
        if not names:
            print(f"No profiles yet. They will be stored in {profiles.directory}")
            return EXIT_OK
        for profile in profiles.all():
            devices = ", ".join(profile.device_keys) or "-"
            print(f"{profile.name}\t{profile.serial.describe()}\t{devices}")
        return EXIT_OK

    if not args.name:
        print(f"portside: 'profiles {action}' needs a profile name", file=sys.stderr)
        return EXIT_USAGE

    if action == "show":
        profile = profiles.load(args.name)
        for key, value in profile.to_dict().items():
            print(f"{key} = {value}")
        return EXIT_OK

    if action == "export":
        target = Path(args.file) if args.file else Path(f"{args.name}.toml")
        print(profiles.export(profiles.load(args.name), target))
        return EXIT_OK

    if action == "import":
        source = Path(args.file) if args.file else Path(args.name)
        imported = profiles.import_file(source)
        print(profiles.save(imported))
        return EXIT_OK

    profiles.delete(args.name)
    print(f"deleted {args.name}")
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
            return asyncio.run(run_connect(args, argv=argv))
        if args.command == "profiles":
            return cmd_profiles(args)
        if args.command == "gui":
            from portside.gui import main as gui_main

            return gui_main([])
    except PortsideError as error:
        print(f"portside: {error.display()}", file=sys.stderr)
        return exit_code_for(error)
    except KeyboardInterrupt:
        return EXIT_OK

    parser.print_help()
    return EXIT_USAGE


if __name__ == "__main__":  # pragma: no cover - module entry point
    raise SystemExit(main())
