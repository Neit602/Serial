# Decisions

Where the PRD left room for interpretation, Portside takes the simplest option that keeps
behaviour identical on Linux, macOS and Windows. Each decision below says what was ambiguous,
what was chosen, and why.

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
