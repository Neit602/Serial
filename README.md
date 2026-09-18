# Portside

A cross-platform serial terminal for Linux, macOS and Windows, written in Python.

Portside talks to routers, switches, embedded boards and lab gear over USB-serial,
native RS-232 and Bluetooth serial ports — with the same features, the same profile
files and the same behaviour on all three operating systems.

> **Status: milestone M1 (core).** The transport layer, port discovery, the session
> core and a raw pass-through CLI are implemented and tested. Terminal emulation, the
> Textual TUI, logging, break/paced paste, the PySide6 GUI and file transfers land in
> later milestones — see [docs/PRD.md](docs/PRD.md).

## What works today

- `portside --list` — every serial port with description, VID:PID, chip family,
  serial number and (on Linux) the bound driver and stable `/dev/serial/by-id` path.
- `portside connect <port> --baud N` — raw pass-through: bytes in, bytes out, no
  emulation, no line-ending translation.
- Full line settings: baud 50–4,000,000, data bits 5–8, parity N/E/O/M/S,
  stop bits 1/1.5/2, flow control none/RTS-CTS/DSR-DTR/XON-XOFF.
- Auto-reconnect that follows the *device*, not the port name: an adapter that comes
  back as `/dev/ttyUSB1` or `COM9` is still recognised (`--reconnect`).
- Actionable errors: permission denied, port busy, port missing and device vanished
  each come with the fix for the platform you are on.

## Install

Portside needs **Python 3.11 or newer**.

### Linux

```bash
pipx install portside          # or: pip install --user portside
portside --list
```

Serial devices are owned by a group — `dialout` on Debian/Ubuntu, `uucp` on Arch.
If Portside reports a permission error it prints the exact command for your system;
the common one is:

```bash
sudo usermod -aG dialout $USER   # then log out and back in
```

### macOS

```bash
pipx install portside          # or: pip3 install --user portside
portside --list
```

Portside lists the `/dev/cu.*` callout devices only. The matching `/dev/tty.*` devices
are hidden on purpose: opening them blocks until the DCD line is asserted, which looks
like a hang. No extra permissions are normally needed.

### Windows

```powershell
py -m pip install --user portside
portside --list
```

Ports are named `COM1`…`COM256`; write them plainly on the command line (`COM12`,
not `\\.\COM12`). CDC devices work out of the box; FTDI, CP210x and PL2303 drivers
arrive through Windows Update; CH34x adapters usually need the vendor driver.

## Use

```bash
# What is plugged in?
portside --list
portside --list --verbose          # adds driver and stable path
portside --list --json             # machine-readable

# Connect (raw pass-through). Ctrl-] quits.
portside connect /dev/ttyUSB0 --baud 115200
portside connect COM3 --baud 9600 --parity E --data-bits 7 --stop-bits 2
portside connect /dev/cu.usbserial-A50285BI --baud 115200 --flow rtscts

# Survive a replug of the same adapter
portside connect /dev/ttyUSB0 --baud 115200 --reconnect

# Scriptable: stdin is piped, stdout is the device's output
printf 'show version\r' | portside connect /dev/ttyUSB0 --baud 9600 --quiet > out.txt
```

Exit codes: `0` success, `2` bad usage, `3` permission denied, `4` port busy,
`5` port not found or device gone, `1` anything else.

Portside also accepts pySerial URLs, which is handy without hardware:

```bash
portside connect loop:// --baud 115200      # echoes whatever you type
```

## Develop

```bash
git clone https://github.com/portside/portside
cd portside
uv sync --extra dev            # or: python -m venv .venv && .venv/bin/pip install -e ".[dev]"

uv run ruff check . && uv run ruff format --check .
uv run mypy
uv run pytest
```

The integration tests use a [socat](http://www.dest-unreach.org/socat/) virtual serial
pair. Install it (`apt install socat`, `brew install socat`) to run them; they skip
cleanly on Windows and wherever socat is missing.

CI runs ruff, mypy `--strict` and pytest on ubuntu-latest, macos-latest and
windows-latest against Python 3.11 and 3.13.

## Design in one paragraph

The core (`portside.core`, `portside.transport`) is UI-independent and asyncio-based.
pySerial's blocking reads run on a dedicated thread per session and cross into asyncio
through `call_soon_threadsafe`; writes are serialised on a single worker thread so they
can never interleave. A UI thread never touches a port. Everything platform-specific
lives in `portside.platform` and `portside.transport` behind typed interfaces, so the
rest of the code never branches on `sys.platform`. Paths come from `platformdirs`.

- [docs/PRD.md](docs/PRD.md) — product requirements
- [docs/PLAN.md](docs/PLAN.md) — the M1 task breakdown
- [docs/DECISIONS.md](docs/DECISIONS.md) — decisions taken where the PRD was ambiguous

## Licence

MIT — see [LICENSE](LICENSE).

Portside is original work. It is functionally inspired by the category of serial
terminals generally; it contains no code, text, artwork or naming from any other
serial application.
