# Decisions

Where the PRD left room for interpretation, Portside takes the simplest option that keeps
behaviour identical on Linux, macOS and Windows. Each decision below says what was ambiguous,
what was chosen, and why. D1–D38 were taken for the Python implementation; D39 onwards
record what the C++ / Qt 6 port kept, changed and fixed.

## M1 — Core

### D1. The PRD lives at `docs/PRD.md`
The exported document arrived as `docs/Portside — Cross-Platform Serial Terminal (PRD).md`.
Renamed to the path the build prompt and this repo's docs refer to. No content changed.

### D2. Blocking transports, threads owned by the session
The `Transport` interface is synchronous. Making it async would force every backend to
reimplement an event loop bridge; instead the session owns one reader thread and one
single-worker write executor per port. A single write worker is what guarantees that two
concurrent `send()` calls cannot interleave their bytes.

### D3. Receive backpressure drops the oldest data
A bounded queue (default 512 chunks) needs a policy when a consumer falls behind. Portside
drops the **oldest** buffered chunk and counts the loss in `Session.rx_dropped`. Rationale: on
a terminal the newest output is what the user is waiting for, and bounded memory is a stated
non-functional requirement. The counter makes the loss visible rather than silent; M2's logging
writes to disk from the event bus, ahead of this queue, so logs stay complete.

### D4. Unsupported control lines are reported, not raised
Many adapters (and every PTY) lack DTR/RTS or a native break. `set_dtr`/`set_rts` return
`False` when the driver has no such line, and `ModemLines` fields are `None` for "unknown"
rather than `False` for "low" — a UI can then grey the indicator instead of lying about it.
`send_break` raises `NotImplementedError`, which is the hook M2's emulated break (F12) uses.
None of these end a session.

### D5. Discovery is advisory; the driver decides whether a port exists
`serial.tools.list_ports` does not enumerate everything that works: Bluetooth RFCOMM nodes,
PTYs and some on-board UARTs are missing on at least one platform. A port name that discovery
does not know is therefore passed to the driver anyway, and the driver's failure is what
produces the typed error. The cost is that unlisted ports carry no `DeviceIdentity`, so they
cannot be auto-reconnected by device (F2/F5) — `--reconnect` says so when that applies.

### D6. Weak identities for adapters without a serial number
Most CH340s and many PL2303 clones report no serial number. Those get an identity built from
VID:PID plus the port description, flagged `weak=True`. It is still good enough to auto-apply a
profile (F20, M2), and the flag lets a UI warn that two identical adapters cannot be told apart.

### D7. `Ctrl-]` leaves `portside connect`, and both exits drain first
`Ctrl-]` follows the telnet convention and is unlikely to be needed by a device. On exit —
whether by `Ctrl-]` or EOF on piped input — Portside keeps reading the port for `--drain`
seconds (default 0.25) so the device's last bytes still reach the screen. Piped (non-tty) input
treats `0x1d` as ordinary data, since no user is pressing anything.

### D8. Exit codes are part of the CLI contract
`0` ok, `2` usage, `3` permission denied, `4` port busy, `5` port not found or device gone,
`1` anything else. Scripts and the future TUI branch on these rather than parsing messages.

### D9. Status on stderr, device data on stdout
`portside connect` writes only device bytes to stdout, so `portside connect … > capture.log`
captures exactly what the device sent. Connection banners, reconnect notices and errors go to
stderr. `--quiet` silences the banners.

### D10. The Linux permission remedy names the group that actually owns the node
The PRD specifies `sudo usermod -aG dialout $USER`. Portside prints exactly that, except it
substitutes the group that owns the device node when it can read it — `uucp` on Arch, for
instance — because pasting `dialout` on a system that uses `uucp` fixes nothing. When the node
cannot be inspected (it is gone, or never existed), it falls back to `dialout`.

### D11. macOS hides `/dev/tty.*`
Per the PRD, only callout devices (`/dev/cu.*`) are listed. The dial-in twins block on open
until DCD is asserted, which users experience as a hang. They can still be opened by typing the
name explicitly (see D5).

### D12. PySide6 and Textual are optional extras
The core and CLI do not import them, and PySide6 in particular has no wheel for every
interpreter in the CI matrix. They are declared as the `gui` and `tui` extras and become
required when M2 and M3 land. `pyudev` stays a Linux-only marker dependency and every use of it
is guarded, so a machine without it falls back to polling.

### D13. Local development may use pip; CI uses uv
`uv` was not available on the development machine. The project is configured for uv (`uv sync`,
`uv run` in CI) and works identically under `python -m venv` + `pip install -e ".[dev]"`. No
`uv.lock` is committed yet; CI resolves and caches by `pyproject.toml`. Commit a lockfile once
the dependency set stops moving.

### D14. Windows integration tests skip rather than install com0com
com0com needs an interactive, signed driver installation, which is not reasonable in CI. The
socat-based integration tier therefore runs on Linux and macOS and skips cleanly on Windows;
the unit and loopback tiers, including `loop://`, run everywhere. Every platform module is
exercised on every runner through synthetic port lists, so Windows-specific and macOS-specific
logic is still covered on Linux and vice versa.

### D15. Scope held to M1
`send_break` exists on the interface but emulated break, CR/LF translation, local echo,
character sets, logging and pacing are deliberately absent: they belong to M2 and would be
untestable without the emulator. `portside connect` is raw pass-through only.

## M2 — Terminal

### D16. The TUI is the default for interactive use; `--raw` keeps M1's behaviour
`portside connect` opens the terminal UI when both stdin and stdout are a terminal, and falls
back to raw pass-through automatically when either is piped. So every M1-era script keeps
working untouched, while a human gets emulation, scrollback and the command keys. `--raw`
forces the old behaviour explicitly.

### D17. Alternate screen and line drawing are implemented on top of pyte
pyte records the 1049/1047/47 private modes but does not swap buffers, and its stream never
dispatches `ESC ( 0`, so full-screen programs would paint over the primary screen (and pour
their redraws into the scrollback), and box-drawn tables would render as `lqk`. Portside keeps
two screens and applies charset designations itself, in stream order. See
`emulation/terminal.py`.

### D18. The emulated break repeats the zero byte when 50 baud is not slow enough
One zero byte at the 50-baud floor lasts 180 ms, short of the PRD's 250 ms default. Rather than
demanding a baud rate no driver is obliged to accept, Portside sends as many zero bytes as the
requested duration needs. Each repeat is separated by one stop bit; receivers detect the break
from the framing error either way, and a break that is too *short* simply fails to interrupt
the device, which is the worse failure.

### D19. A paced paste does not wait for a prompt after its last line
Nothing follows the last line, so waiting for one more prompt only delays the paste — and on a
device that stays quiet after the final `end`, it turns a successful paste into a spurious
timeout. Found by the tests, fixed in the product.

### D20. Timestamps mark when a line *started*
A line split across two reads carries one timestamp, taken when its first byte arrived. Stamping
on completion would skew every line by the device's own output rate.

### D21. Text logs are UTF-8 with LF, raw logs are byte-exact
Per the PRD, text logs normalise CR, LF and CRLF to LF on every platform — no CRLF logs on
Windows — and carry timestamps and Portside's own notes (break sent, paste finished). Raw logs
get none of that: they are exactly what came off the wire, for protocol debugging.

### D22. The logger listens to the event bus, not to the terminal
A scrolled, busy or hex-mode terminal must never cost the log a line, and the receive queue's
drop-oldest policy (D3) must never reach the file. The logger subscribes alongside the UI.

### D23. Function keys are Portside's; every control character is the device's
Ctrl-C, Ctrl-Z and friends belong to the switch, so commands live on F1–F10 (with PgUp/PgDn for
scrollback), which a serial console almost never needs. F1 shows the map, because nobody
remembers function keys.

### D24. Rendering is capped at 60 fps and driven by a revision counter
The emulator bumps a counter on every change; the widget repaints only when it moved. At high
baud rates a repaint per read would spend the whole session drawing (PRD risk register).

### D25. Profiles ship in M2, ahead of the PRD's milestone table
The table places profiles in M3, but F23 specifies `--profile X` on the TUI command line, and
everything a profile stores exists only after M2. The store, auto-apply by device identity and
import/export are here; M3 adds a GUI over them. `schema_version` is checked on load: a profile
from a newer Portside is refused rather than silently misread.

### D26. Explicit command-line options beat a profile
`--profile lab --baud 115200` uses the profile for everything except the baud rate. The CLI
distinguishes "typed by the user" from "argparse default" by inspecting argv.

### D27. Search is literal by default
An engineer looking for `Gi1/0/1` or `10.0.0.1` should not have to escape anything; regex is
opt-in, and a bad pattern is a typed error rather than a traceback.

### D28. The M2 exit criterion is split in two
"Configure a real Cisco switch from the TUI on each OS" cannot run in CI. The automated half is
a scripted console on a real socat serial pair, driven end to end through the TUI
(`tests/integration/test_fake_switch.py`). The hardware half is `docs/HARDWARE-CHECK.md`, to be
run per release on all three OSes. What CI proves and what it cannot is stated there explicitly.

## M3 — GUI

### D29. An asyncio loop on a worker thread, bridged by Qt signals
Qt will not share its event loop with asyncio. Portside runs the core's loop on a dedicated
thread (`gui/runtime.py`) and republishes session events as Qt signals, which Qt delivers to the
GUI thread through queued connections. The alternative — a loop-integration library such as
`qasync` — would add a dependency the PRD's stack does not list and would hide the thread
boundary rather than make it explicit. Every controller call returns a future; nothing in the
GUI ever waits on the port.

### D30. A painted terminal widget, not a text widget
`QPlainTextEdit` fights per-cell attributes, the block cursor, the alternate screen and
cell-accurate selection. `TerminalWidget` paints cells with `QPainter`, repainting from a 60 fps
timer only when the emulator's revision counter moves — the same batching rule the TUI uses, and
what makes high baud rates survivable.

### D31. Resizing must not lose output — fixed in the emulator
pyte's `resize` drops the top rows when a screen shrinks and forgets them, and its sparse buffer
can leave a stale copy of a dropped row behind. Found by a GUI test (opening the find bar resizes
the terminal). `Terminal.resize` now moves the rows that no longer fit into the scrollback,
keeping their colours, and rebuilds the screen from the rows that survive. Columns are still not
reflowed: pyte truncates rather than rewrapping, and reflowing would mean re-running the whole
scrollback through the emulator. The TUI gets the same fix for free.

### D32. The GUI connects from the `PortInfo` the user clicked
It does not re-resolve the port through discovery. That would cost a second scan on every
connect and would lose the device identity for ports enumeration does not list (D5) — which in
turn would break profile auto-apply and reconnect-by-identity for exactly the devices that need
them most.

### D33. Three visual states for modem lines, not two
`ModemLines` reports `None` for a line the driver does not implement (D4). The signals panel
renders that as "not reported by this driver", never as "low", and the DTR/RTS toggles disable
themselves with an explanatory tooltip when the driver refuses them. Claiming a line is low when
it is unknown would be a lie the user debugs for an hour.

### D34. The settings dialog never invents values it has no control for
Editing a profile in the GUI preserves fields the dialog does not show — the log's own character
set, for instance. Overwriting them would silently discard whatever a hand-edited profile set.

### D35. Qt is an optional extra, and importing Portside never imports it
`pip install portside` gives the CLI and the TUI with no Qt anywhere; `pip install portside[gui]`
adds the desktop app. `portside.gui.__init__` deliberately contains no Qt import, and a test in a
fresh interpreter asserts that importing Portside leaves `PySide6` out of `sys.modules`. Running
`portside-gui` without Qt prints how to install it and what still works without it.

### D36. The GUI is a separate entry point
`portside connect` remains the CLI/TUI; the desktop app is `portside-gui` (or `portside gui`).
Launching a window from a command that has always been a terminal program would surprise every
existing script and every ssh session.

### D37. The M3 exit criterion is split, like M2's
"New user connects in under 30 s" measures a person. The automated half asserts the *shape* of
the path — a cold app with a device present reaches a connected session in at most three actions,
no typing, no dialogs — and fails if anyone adds a step. The human half is
[USABILITY-CHECK.md](USABILITY-CHECK.md), to be run per release on all three OSes.

### D38. Memory is asserted as growth, not peak
`resource.getrusage` reports a process high-water mark that never falls, so an absolute
assertion depends on whatever ran earlier in the test session. The memory test measures the
growth the four sessions cause (from `/proc/self/statm` on Linux, skipped elsewhere), which is
the part actually attributable to the feature.

## The C++ / Qt 6 port

Portside was rewritten from Python (PySide6, pyte, Textual) in C++20 with Qt 6. The port
keeps the *behaviour* M1–M3 specified, not the code structure: decisions D1–D38 still hold
except where an entry below amends one. The PRD's stack table (Python, pySerial, pyte,
Textual, platformdirs, tomllib) is superseded by these entries.

### D39. Qt 6.6+ is the whole stack
QtSerialPort replaces pySerial and QSerialPortInfo replaces `serial.tools.list_ports`;
QStandardPaths replaces platformdirs, keeping the same directories on every OS
(`~/.config/Portside`, `~/Library/Application Support/Portside`, `%APPDATA%\Portside`, and
the platformdirs log directories). `PORTSIDE_CONFIG_DIR` and `PORTSIDE_LOG_DIR` override
them. 6.6 is the floor because the session's futures use `QtFuture::makeReadyValueFuture`.
Tests use Qt Test and CTest; there are no other dependencies.

### D40. One I/O thread per session replaces asyncio (amends D2, D29)
A `Session` lives on its owner's thread; its transport lives on a `QThread` of its own and
is called only there. Every write and control call is queued to that thread, which is what
now guarantees that two `send()`s cannot interleave (D2's single-worker executor, by other
means). Results come back as `QFuture`s and events as Qt signals; the GUI no longer needs
the asyncio-on-a-worker-thread bridge of D29. Because QSerialPort is event driven, the
transport interface is too (`readyRead`, `failed`) rather than a blocking `read()`.
`Session::open()` blocks for the driver's answer (milliseconds), exactly as the Python GUI
already did with `runtime.run(session.open())`.

### D41. Receive backpressure is bounded by bytes (amends D3, D22)
The I/O thread appends to a 4 MiB buffer and the owner drains it on a queued signal; when a
stalled owner lets it fill, the *oldest* bytes are dropped and counted in
`Session::rxDropped()`. The logger now listens on the owner's thread alongside the UI, so it
would only miss data after the owner had been frozen for tens of seconds at 921,600 baud —
a stall that would already be the bigger problem. Coalescing reads this way also means one
signal per burst rather than one per read.

### D42. A self-contained VT100/xterm emulator replaces pyte (amends D17, D31)
A VT500-style parser drives two screens. Because one parser feeds whichever screen is
active, output after `ESC [?1049h` in the *same* read now lands on the alternate screen (the
pyte-based version drew it on the primary until the next read). Beyond pyte's coverage it
answers cursor-position and device-attribute queries (the GUI sends the answers back down
the wire), honours DECCKM so full-screen programs get the cursor keys they ask for, and
handles scroll regions, insert/delete, 256-colour and truecolour, and double-width
characters. Combining marks are dropped rather than composed — rare on a serial console,
and it keeps a cell to one code point. Scrollback lines are stored trimmed, which is what
keeps four 10,000-line sessions at ~66 MB. Resizing smaller first discards blank rows
*below* the cursor and only then moves rows from the top into the scrollback (D31), so a
prompt at the top of a cleared screen stays put.

### D43. TOML is read and written in-house
A small TOML 1.0 reader (comments, all string forms, integers in any base, floats, arrays,
inline tables, dotted keys, `[tables]`, `[[arrays of tables]]`) and writer, rather than a
new dependency. Errors name the line. Dates and times are rejected with a clear message:
nothing in a profile is a date. Profiles written by the Python Portside load unchanged.

### D44. The Textual TUI is gone; `portside connect` is pass-through (amends D7, D16, D23)
The C++ port ships the desktop app and the command line only. `portside connect` always
connects the user's own terminal to the device, which does the VT100 work. On an
interactive terminal it applies the profile's line discipline (what Enter and Backspace
send, local echo) unless `--raw` is given; piped input stays byte-exact. `Ctrl-]` no longer
quits outright: it opens a one-key menu — `q` (or `Ctrl-]` again) quits, `b` sends a break,
`d`/`r` toggle DTR/RTS, `]` sends a literal `Ctrl-]` — because without the TUI's F-keys
this is the only way to send a break from a headless box. When the device is lost and no
reconnect follows, `connect` exits with code 5, as D8's table says.

### D45. Linux discovery hides phantom UARTs and reports the real driver
QSerialPortInfo lists every `ttyS*` node the 8250 driver registered, including the dozens
with no UART behind them. Ports whose sysfs `type` is 0 ("unknown") are hidden, which is
more reliable than pySerial's subsystem check on recent kernels. Since Linux 6.5 an on-board
UART's tty hangs off the serial core's `port`/`ctrl` shims, so the driver reported is the
first real one above them. There is no udev watcher: the device list polls once a second,
which meets F1's two seconds on every OS (the Python GUI polled too).

### D46. Exceptions inside the core, signals at its edge
The core throws a typed `PortsideError` (kind, message, port, remedy) wherever Python raised;
nothing throws through a Qt event handler — the session catches on the I/O thread and
re-delivers as a failed future or an `errorRaised` signal.

### D47. `loop://` is the only URL
pySerial's URL handlers are gone. `loop://` (an echoing in-memory port, as before) is built
in; any other `scheme://` is refused with an explanation.

### D48. Line-setting gaps behave as pySerial did
termios has no 1.5 stop bits and no DSR/DTR handshaking. 1.5 falls back to 2 where the
driver refuses it, and DSR/DTR flow control is applied through the DCB on Windows and is a
no-op elsewhere — what pySerial did on each OS. DTR and RTS are asserted on open, as
pySerial's defaults were.

### D49. Log details (amends D21)
A CRLF split across two reads is one line break in a text log, not two. Rotation numbers
past any rotated file already present (`name.N.log`) instead of restarting at 1, so a new
session never overwrites an earlier session's rotations; pruning removes the lowest numbers.

### D50. GUI behaviour fixed in the port
While auto-reconnect waits for a device, each failed attempt updates the status line instead
of opening an error dialog. PgUp/PgDn scroll the scrollback (the Python GUI swallowed them),
Tab reaches the device instead of moving focus, modem lines are read as soon as a session
opens, a cancelled paste is reported in the status line rather than as an error, and an
unknown font family falls back to the system monospace font.

### D51. Version 0.2.0 marks the rewrite
The CLI, the profile format and the exit codes are unchanged; the minor version bump says
the implementation underneath is new.

## M4 — file transfers

### D52. Received files never silently overwrite
The default overwrite policy is *refuse*. ZMODEM answers an existing name with ZSKIP and
Kermit declines the file in its attribute (or data) acknowledgement, so the rest of a batch
still arrives; XMODEM and YMODEM have no way to skip a file, so they stop with a message
that names the file and the fix. A profile (or `--overwrite`) can choose *rename*
(`name.1.ext`, `name.2.ext`, …) or *overwrite*. The CLI exits 1 when any file was skipped.

### D53. Downloads go to `NAME.part` and are renamed when complete
A broken transfer never leaves a file that looks finished. The modification time the sender
supplied (YMODEM, ZMODEM, Kermit) is applied after the rename. Names from the device are
reduced to their last path component and made safe for every filesystem (no `..`, no
directories, no Windows device names), so a device cannot write outside the chosen folder.

### D54. ZMODEM's ZCOMMAND is refused, always
A ZMODEM sender may ask the receiver to run a shell command. Portside never does: the
transfer stops with an error naming ZCOMMAND, and the device gets ZMODEM's cancel. Also not
implemented: sender-side crash recovery, compression and encryption. The receiver resumes
an interrupted download only when asked (`--resume`, the dialog's checkbox, or the sender's
ZCRESUM flag); ZMODEM keeps its `.part` file after a failure so that a resume is possible.

### D55. Kermit: long packets and attributes, no sliding windows or streaming
Packets up to 4,096 bytes when both sides offer them, type 1/2/3 block checks (type 3
preferred), control/8th-bit/repeat prefixing and attribute packets (size, date, binary
type). Sliding windows and streaming are not offered: on a serial console the gain is small
and the complexity is not. Transfers are always binary.

### D56. Logs during a transfer
While a transfer holds the port, a raw log still records every byte from the wire; a text
log gets one note when the transfer starts and one when it ends (its summary or error),
rather than a megabyte of protocol mojibake.

### D57. The port belongs to the transfer while it runs
Starting a transfer claims the session's byte stream: received bytes go to the protocol,
not the terminal, and the terminal's keys, pastes and breaks are refused until it ends. The
GUI shows the transfer in a panel inside the session's tab (file, per-file and overall
progress, speed, retries, cancel) rather than in a modal dialog, so other sessions stay
usable. In `portside connect`, `Ctrl-]` (or `Ctrl-C`) cancels a running receive.

### D58. Every protocol is in-house
The PRD's stack table named a Python `xmodem` package for XMODEM. The C++ port implements
XMODEM, YMODEM, ZMODEM and Kermit itself, sharing one block framing for XMODEM and YMODEM;
interoperability is checked against lrzsz and G-Kermit/C-Kermit (`tests/integration/tst_interop.cpp`).
Engines are plain blocking code on a thread of their own, talking to the session through
the claimed stream, so the GUI and the I/O thread never wait on a protocol timeout.

### D59. Where downloads go
The desktop app saves into the user's Downloads folder; `portside receive` into the current
directory, as command-line tools do. A profile's `[transfer] directory` overrides both, and
`--into` overrides the profile.

### D60. ZMODEM auto-start
The terminal watches for ZRQINIT (`**\x18B00`, the device ran `sz`) and ZRINIT
(`**\x18B01`, it ran `rz`). The desktop app offers to receive (or asks what to send);
declining sends ZMODEM's cancel so the device does not wait a minute for nothing.
`portside connect` prints a hint and receives on `Ctrl-]` `z`. The offer can be turned off
per profile (`zmodem_auto_start = false`), and is not repeated for five seconds after a
transfer, because a cancelled `sz` keeps knocking for a while.
