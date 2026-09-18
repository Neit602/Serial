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
