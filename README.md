# Portside

A cross-platform serial terminal for Linux, macOS and Windows, written in Python.

Portside talks to routers, switches, embedded boards and lab gear over USB-serial,
native RS-232 and Bluetooth serial ports — with the same features, the same profile
files and the same behaviour on all three operating systems.

> **Status: milestone M3 (GUI).** The core, the terminal emulator, the Textual TUI, the
> PySide6 desktop app, session logging, break signalling, paced paste and TOML profiles
> are implemented and tested. File transfers (M4) and signed installers (M5) come next —
> see [docs/PRD.md](docs/PRD.md).

## What works today

- `portside --list` — every serial port with description, VID:PID, chip family,
  serial number and (on Linux) the bound driver and stable `/dev/serial/by-id` path.
- `portside connect <port> --baud N` — a full terminal: VT100/xterm emulation with
  colour, the alternate screen and line drawing, 10,000 lines of scrollback, search,
  and a hex view.
- **Break**, including an emulated break for adapters whose driver has none — the
  thing you need to interrupt a boot and get to ROMMON.
- **Paced paste**: per-character and per-line delays, and an optional wait-for-prompt
  regex, so a pasted config arrives intact on gear with no usable flow control.
- **Session logs**: text (decoded, timestamped) or raw (byte-exact), filename
  templates, size-based rotation, and timestamps that are absolute, elapsed or per-line
  deltas.
- **Profiles** in TOML that auto-apply by *device*: the same adapter gets the same
  settings whether it appears as `/dev/ttyUSB0`, `/dev/cu.usbserial-A50285BI` or `COM9`.
- **A desktop app** (`portside-gui`): device list with hot-plug, sessions as tabs or
  separate windows, live CTS/DSR/DCD/RI indicators and DTR/RTS toggles, settings and
  profile dialogs, selection and clipboard, dark/light/high-contrast themes.
- Full line settings: baud 50–4,000,000, data bits 5–8, parity N/E/O/M/S,
  stop bits 1/1.5/2, flow control none/RTS-CTS/DSR-DTR/XON-XOFF; line endings, local
  echo and backspace are per-profile, never an OS default.
- Auto-reconnect that follows the *device*, not the port name (`--reconnect`).
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

# Connect. On a terminal this opens the TUI; F1 shows the keys.
portside connect /dev/ttyUSB0 --baud 115200
portside connect COM3 --baud 9600 --parity E --data-bits 7 --stop-bits 2
portside connect /dev/cu.usbserial-A50285BI --baud 115200 --flow rtscts

# Survive a replug of the same adapter
portside connect /dev/ttyUSB0 --baud 115200 --reconnect

# Log the session while you work
portside connect /dev/ttyUSB0 --baud 9600 --log session.log --timestamps elapsed

# Raw pass-through, exactly as in M1 (and what piping selects automatically)
portside connect /dev/ttyUSB0 --baud 9600 --raw
printf 'show version\r' | portside connect /dev/ttyUSB0 --baud 9600 --quiet > out.txt
```

Exit codes: `0` success, `2` bad usage, `3` permission denied, `4` port busy,
`5` port not found or device gone, `1` anything else.

Portside also accepts pySerial URLs, which is handy without hardware:

```bash
portside connect loop:// --baud 115200      # echoes whatever you type
```

### In the TUI

| Key | Does |
| --- | --- |
| `F1` | Show the key map |
| `F2` | Send a break (emulated automatically if the driver has none) |
| `F3` | Find in the screen and scrollback |
| `F4` | Paste a file, paced; press again to cancel |
| `F5` | Start or stop logging |
| `F6` | Toggle the hex view |
| `F7` | Cycle timestamp modes |
| `F8` | Cycle dark / light / high-contrast themes |
| `F9` | Clear the screen and scrollback |
| `F10` | Quit |
| `PgUp` / `PgDn` | Move through the scrollback |

Everything else goes to the device — `Ctrl-C`, `Ctrl-Z`, arrows, function-key sequences
and all. Typing returns the view to the live tail.

### The desktop app

```bash
pipx install "portside[gui]"   # Qt is an optional extra
portside-gui                   # or: portside gui
```

Pick a device in the list on the left and press Enter (or double-click) — that is the
whole path to a connected terminal. Sessions open as tabs; `File ▸ Detach tab to window`
moves one into its own window without dropping the connection.

| Shortcut | Does |
| --- | --- |
| `Ctrl+N` / `Ctrl+Shift+N` | Connect / connect with settings first |
| `Ctrl+W` | Close the session |
| `Ctrl+B` | Send a break (emulated automatically if the driver has none) |
| `Ctrl+Shift+V` | Paste a file, paced (or just drop the file on the window) |
| `Ctrl+L` | Start or stop logging |
| `Ctrl+H` | Hex view |
| `Ctrl+F` | Find in the scrollback |
| `Ctrl+Shift+C` | Copy the selection |
| `Ctrl+,` / `Ctrl+P` | Session settings / profiles |
| `Ctrl+K` | Clear |

Everything else goes to the device, including `Ctrl+C` and `Ctrl+Z`.

The GUI needs Qt; the CLI and TUI do not, and importing Portside never pulls Qt in.

### Profiles

A profile stores the port settings, line discipline, break mode, pacing, logging and
terminal options, and can bind itself to a device so it is applied on sight.

```bash
portside profiles                          # list
portside profiles show cisco-lab
portside profiles export cisco-lab --file cisco-lab.toml
portside profiles import cisco-lab.toml    # e.g. from a colleague
portside connect /dev/ttyUSB0 --profile cisco-lab
portside connect /dev/ttyUSB0 --profile cisco-lab --baud 115200   # flags win
```

Profiles are plain TOML — meant to be edited by hand and kept in git. They live in
`~/.config/portside` (Linux), `~/Library/Application Support/Portside` (macOS) or
`%APPDATA%\Portside` (Windows), and the same file works on all three.

```toml
name = "cisco-lab"
device_keys = ["usb_0403_6001_A50285BI"]

[serial]
baudrate = 9600

[text]
send_ending = "cr"
backspace = "del"

[pacing]
line_delay = 0.05
wait_for_prompt = "[>#]\\s*$"

[break]
mode = "auto"
duration = 0.25
```

## Develop

```bash
git clone https://github.com/portside/portside
cd portside
uv sync --extra dev            # or: python -m venv .venv && .venv/bin/pip install -e ".[dev]"

uv run ruff check . && uv run ruff format --check .
uv run mypy
uv run pytest
QT_QPA_PLATFORM=offscreen uv run pytest -m gui   # the Qt tests, headless
```

The integration tests use a [socat](http://www.dest-unreach.org/socat/) virtual serial
pair, with a scripted console at the far end that prompts, echoes and notices a break —
close enough to a switch to exercise pacing, logging and the TUI end to end. Install
socat (`apt install socat`, `brew install socat`) to run them; they skip cleanly on
Windows and wherever socat is missing. The parts no CI can cover are listed in
[docs/HARDWARE-CHECK.md](docs/HARDWARE-CHECK.md).

CI runs ruff, mypy `--strict` and pytest on ubuntu-latest, macos-latest and
windows-latest against Python 3.11 and 3.13.

## Design in one paragraph

The core (`portside.core`, `portside.transport`) is UI-independent and asyncio-based.
pySerial's blocking reads run on a dedicated thread per session and cross into asyncio
through `call_soon_threadsafe`; writes are serialised on a single worker thread so they
can never interleave. A UI thread never touches a port. The Textual TUI attaches to the
session's event bus directly; the Qt GUI runs the same asyncio loop on a worker thread
and republishes those events as Qt signals. Everything platform-specific lives in
`portside.platform` and `portside.transport` behind typed interfaces, so the rest of the
code never branches on `sys.platform`. Paths come from `platformdirs`.

- [docs/PRD.md](docs/PRD.md) — product requirements
- [docs/PLAN.md](docs/PLAN.md) — the M1 task breakdown and outcome
- [docs/PLAN-M2.md](docs/PLAN-M2.md) — the M2 task breakdown and outcome
- [docs/PLAN-M3.md](docs/PLAN-M3.md) — the M3 task breakdown and outcome
- [docs/DECISIONS.md](docs/DECISIONS.md) — decisions taken where the PRD was ambiguous
- [docs/HARDWARE-CHECK.md](docs/HARDWARE-CHECK.md) — the per-release checks CI cannot run
- [docs/USABILITY-CHECK.md](docs/USABILITY-CHECK.md) — the 30-second first-connect test

## Licence

MIT — see [LICENSE](LICENSE).

Portside is original work. It is functionally inspired by the category of serial
terminals generally; it contains no code, text, artwork or naming from any other
serial application.
