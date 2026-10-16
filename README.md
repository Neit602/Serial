# Portside

A cross-platform serial terminal for Linux, macOS and Windows, written in C++ with Qt 6.

Portside talks to routers, switches, embedded boards and lab gear over USB-serial,
native RS-232 and Bluetooth serial ports — with the same features, the same profile
files and the same behaviour on all three operating systems.

> **Status: C++ rewrite of milestone M3.** The core, the terminal emulator, the
> command line, the Qt desktop app, session logging, break signalling, paced paste and
> TOML profiles are implemented and tested. The original Python implementation (M1–M3)
> is in git history on the `m3-gui` branch; what changed in the port is in
> [docs/DECISIONS.md](docs/DECISIONS.md) (D39 onwards). File transfers (M4) and signed
> installers (M5) come next — see [docs/PRD.md](docs/PRD.md).

## What works today

- `portside --list` — every serial port with description, VID:PID, chip family,
  serial number and (on Linux) the bound driver and stable `/dev/serial/by-id` path.
- **A desktop app** (`portside-gui`): device list with hot-plug, sessions as tabs or
  separate windows, a VT100/xterm terminal with colour, the alternate screen and line
  drawing, 10,000 lines of scrollback, find, a hex view, live CTS/DSR/DCD/RI indicators
  and DTR/RTS toggles, settings and profile dialogs, selection and clipboard, and
  dark/light/high-contrast themes.
- `portside connect <port>` — your terminal talks straight to the device, with break,
  DTR/RTS, logging, profiles and auto-reconnect a key press away.
- **Break**, including an emulated break for adapters whose driver has none — the
  thing you need to interrupt a boot and get to ROMMON.
- **Paced paste**: per-character and per-line delays, and an optional wait-for-prompt
  regex, so a pasted config arrives intact on gear with no usable flow control.
- **Session logs**: text (decoded, timestamped) or raw (byte-exact), filename
  templates, size-based rotation, and timestamps that are absolute, elapsed or per-line
  deltas.
- **Profiles** in TOML that auto-apply by *device*: the same adapter gets the same
  settings whether it appears as `/dev/ttyUSB0`, `/dev/cu.usbserial-A50285BI` or `COM9`.
- Full line settings: baud 50–4,000,000, data bits 5–8, parity N/E/O/M/S,
  stop bits 1/1.5/2, flow control none/RTS-CTS/DSR-DTR/XON-XOFF; line endings, local
  echo and backspace are per-profile, never an OS default.
- Auto-reconnect that follows the *device*, not the port name.
- Actionable errors: permission denied, port busy, port missing and device vanished
  each come with the fix for the platform you are on.

## Build

Portside needs **Qt 6.6 or newer** (Core, SerialPort, Widgets, Test), **CMake 3.21+**
and a C++20 compiler (GCC 11+, Clang 14+, MSVC 2022).

### Linux

```bash
# Debian/Ubuntu: sudo apt install qt6-base-dev libqt6serialport6-dev cmake ninja-build g++
# Arch:          sudo pacman -S qt6-base qt6-serialport cmake ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build        # installs portside and portside-gui
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
brew install qt cmake ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build
build/src/cli/portside --list
open build/src/gui/portside-gui.app
```

Portside lists the `/dev/cu.*` callout devices only. The matching `/dev/tty.*` devices
are hidden on purpose: opening them blocks until the DCD line is asserted, which looks
like a hang. No extra permissions are normally needed.

### Windows

Install Qt 6 (with the *Qt Serial Port* module) for MSVC from the Qt installer, then from
a *Developer PowerShell for VS 2022*:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --build build
build\src\cli\portside.exe --list
```

Ports are named `COM1`…`COM256`; write them plainly on the command line (`COM12`,
not `\\.\COM12`). CDC devices work out of the box; FTDI, CP210x and PL2303 drivers
arrive through Windows Update; CH34x adapters usually need the vendor driver.

`-DPORTSIDE_BUILD_GUI=OFF` builds just the command line, which needs only QtCore and
QtSerialPort — handy on a headless lab box.

## Use

### The desktop app

```bash
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
| `PgUp` / `PgDn` | Move through the scrollback |
| `F1` | Show every shortcut |

Everything else goes to the device, including `Ctrl+C`, `Ctrl+Z`, `Tab` and the arrow
keys (in the application mode full-screen programs ask for). On macOS the shortcuts use
`⌘` and the physical `Control` key goes to the device.

### The command line

```bash
# What is plugged in?
portside --list
portside --list --verbose          # adds driver and stable path
portside --list --json             # machine-readable

# Connect: your terminal talks to the device
portside connect /dev/ttyUSB0 --baud 115200
portside connect COM3 --baud 9600 --parity E --data-bits 7 --stop-bits 2
portside connect /dev/cu.usbserial-A50285BI --baud 115200 --flow rtscts

# Survive a replug of the same adapter
portside connect /dev/ttyUSB0 --baud 115200 --reconnect

# Log the session while you work
portside connect /dev/ttyUSB0 --baud 9600 --log session.log --timestamps elapsed

# Scripts: piped input is forwarded byte-exact and ends the session at EOF
printf 'show version\r' | portside connect /dev/ttyUSB0 --baud 9600 --quiet > out.txt
```

On a terminal, **`Ctrl-]`** opens a one-key menu:

| Then press | Does |
| --- | --- |
| `q` (or `Ctrl-]` again) | Quit |
| `b` | Send a break (emulated automatically if the driver has none) |
| `d` / `r` | Toggle DTR / RTS |
| `]` | Send a literal `Ctrl-]` to the device |

Enter and Backspace send what the profile says (`--send-ending`, `--backspace`), and
`--local-echo` echoes typing; `--raw` turns all of that off and sends keys byte-exact.
Device output goes to stdout and Portside's own messages to stderr, so
`portside connect … > capture.log` captures exactly what the device sent.

Exit codes: `0` success, `2` bad usage, `3` permission denied, `4` port busy,
`5` port not found or device gone, `1` anything else.

`loop://` is a built-in test port that echoes whatever you type:

```bash
portside connect loop://
```

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
`~/.config/Portside/profiles` (Linux), `~/Library/Application Support/Portside/profiles`
(macOS) or `%APPDATA%\Portside\profiles` (Windows), and the same file works on all three.
Profiles written by the Python Portside load unchanged.

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

`PORTSIDE_CONFIG_DIR` and `PORTSIDE_LOG_DIR` override the configuration and log
directories, for portable installs and for tests.

## Develop

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure        # everything; GUI tests run headless
ctest --test-dir build -L gui                     # just the Qt widget tests
ctest --test-dir build -L socat                   # just the virtual-serial-pair tests

# The same suite under AddressSanitizer and UBSan, as CI runs it
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan && ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan
```

The tests use Qt Test, one executable per area under `tests/`. The integration tests use
a [socat](http://www.dest-unreach.org/socat/) virtual serial pair, with a scripted
console at the far end that prompts, echoes and notices a break — close enough to a
switch to exercise pacing, logging and the CLI end to end. Install socat
(`apt install socat`, `brew install socat`) to run them; they skip cleanly on Windows and
wherever socat is missing. The parts no CI can cover are listed in
[docs/HARDWARE-CHECK.md](docs/HARDWARE-CHECK.md).

CI builds and tests on ubuntu-latest, macos-latest and windows-latest against Qt 6.8,
with warnings as errors on Linux, plus an AddressSanitizer + UBSan run.

## Design in one paragraph

The core (`src/core`, a static library on QtCore and QtSerialPort only) is UI-independent.
Each `Session` lives on its owner's thread and drives a transport that lives on an I/O
`QThread` of its own: every write and control call is queued to that thread, so bytes can
never interleave and nothing ever blocks the GUI, and received data comes back through a
bounded, drop-oldest buffer and a queued signal. Results of actions arrive as `QFuture`s,
events as Qt signals. The VT100/xterm emulator, the TOML reader and the
logger are self-contained. Everything platform-specific lives behind the `Platform`
interface and the transports, so the rest of the code never branches on the OS. The CLI
(`src/cli`) and the desktop app (`src/gui`) are thin layers over the core.

- [docs/instruction.html](docs/instruction.html) — the user guide: what Portside does and how to use it
- [docs/PRD.md](docs/PRD.md) — product requirements
- [docs/DECISIONS.md](docs/DECISIONS.md) — decisions taken where the PRD was ambiguous,
  and what changed in the C++ port
- [docs/PLAN.md](docs/PLAN.md), [docs/PLAN-M2.md](docs/PLAN-M2.md),
  [docs/PLAN-M3.md](docs/PLAN-M3.md) — the milestone breakdowns (written for the Python
  implementation)
- [docs/HARDWARE-CHECK.md](docs/HARDWARE-CHECK.md) — the per-release checks CI cannot run
- [docs/USABILITY-CHECK.md](docs/USABILITY-CHECK.md) — the 30-second first-connect test

## Licence

MIT — see [LICENSE](LICENSE).

Portside is original work. It is functionally inspired by the category of serial
terminals generally; it contains no code, text, artwork or naming from any other
serial application.
