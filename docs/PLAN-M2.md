# Portside — M2 (Terminal) Implementation Plan

Status: **approved and implemented** (see *M2 outcome* at the end). Scope is milestone **M2** from [PRD.md](PRD.md):
pyte emulation, the Textual TUI, logging, timestamps, break + emulated break, and paced paste.

Exit criteria (PRD): *"Can configure a real Cisco switch from the TUI on each OS."*
That last step needs hardware and is yours to run; §8 says how I get as close as CI can.

Builds on M1 ([PLAN.md](PLAN.md)), which is merged on `m1-core`. M2 work goes on `m2-terminal`.

Out of scope, unchanged: PySide6 GUI (M3), file transfers (M4), installers (M5), direct USB (M6).

---

## 1. Emulation core — `portside/emulation/` (F7, F8, F11)

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 1.1 | `decoder.py`: incremental byte→text decoder for UTF-8, ASCII, Latin-1, CP437, CP850, tolerant of a multi-byte character split across two reads; `strip_high_bit` option; undecodable bytes become U+FFFD, never an exception | F11, F10 | Unit: each charset round-trips its own range; a UTF-8 sequence fed one byte at a time decodes identically to one fed whole; **hypothesis**: arbitrary byte streams never raise and never lose length in ASCII/Latin-1 |
| 1.2 | `terminal.py`: `Terminal` wrapping `pyte.HistoryScreen` + `pyte.ByteStream` — `feed(bytes)`, `resize(cols, rows)`, `snapshot()` returning typed `Cell`/`Line` rows (char, fg, bg, bold, reverse, …), cursor position and visibility, alternate-screen flag | F7 | Unit against captured real-world byte streams: colours (SGR), cursor moves, clear, alternate screen enter/exit, DEC line drawing, wrapping; assertions on `snapshot()`, not on pyte internals |
| 1.3 | Scrollback: configurable, default 10,000 lines; `scroll_to(offset)`, `scroll_by(delta)`, "follow tail" flag that re-engages on new output | F8 | Unit: feed 20,000 lines into a 10,000 scrollback, assert the oldest are gone and the newest retained; scrolling up pins the view, new output does not steal it back |
| 1.4 | `find.py`: search scrollback + screen, case-insensitive by default, regex option; returns positions for the TUI to highlight; `find_next`/`find_prev` wrap | F8 | Unit: matches across the scrollback boundary, multiple hits per line, no match, invalid regex → typed `InvalidSettingsError` |
| 1.5 | `hexview.py`: `format_hex(data, offset, width=16)` → offset + hex + ASCII gutter; mixed and pure-hex modes; honours `strip_high_bit` | F10 | Golden tests on known buffers (including a partial final row and non-printable bytes); property test: the formatter never loses or reorders a byte |

## 2. Line discipline — `portside/core/textio.py` (F9, F14)

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 2.1 | `LineEndings` policy: on send and on receive independently, `cr` / `lf` / `crlf` / `none`; `local_echo` on/off; `backspace_sends` BS (0x08) or DEL (0x7f). All per-profile, never an OS default (PRD rule) | F9 | Unit table over every (in, out) combination; **hypothesis**: translate-then-untranslate is lossless for the reversible modes; a test asserts the defaults are identical on all three platforms |
| 2.2 | Software XON/XOFF emulation for drivers that lack it: 0x13 pauses the send queue, 0x11 resumes, with a configurable cap so a lost XON cannot wedge a session forever | F14 (P1) | Unit with `LoopbackTransport`: feeding XOFF stalls sending within one chunk, XON resumes, the cap expires and logs a warning |

## 3. Break, native and emulated — `portside/core/breaks.py` (F12)

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 3.1 | `Session.send_break(duration=0.25)` tries the native call first (M1 already raises `NotImplementedError` when the driver has none) | F12 | Unit: loopback records the native break and its duration |
| 3.2 | Emulated break: drop to the lowest supported baud, send `0x00`, restore the previous settings — with the low baud chosen so the start bit + eight zero bits exceed `duration`; always restores settings, including on error | F12 | Unit: a transport that refuses native break records the exact sequence (settings→0x00→settings) and the restore happens even when the write raises; a timing test asserts the computed baud yields ≥ the requested duration |
| 3.3 | `BreakMode` setting: `auto` (native, fall back to emulated), `native`, `emulated` — so a user whose driver lies about supporting break can force the fallback | F12 | Unit per mode, including "native forced but unsupported" → typed error with a remedy |

## 4. Paced paste — `portside/core/pacing.py` (F13)

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 4.1 | `PacePolicy(char_delay, line_delay, wait_for_prompt: str \| None, prompt_timeout, abort_on_timeout)`; `paste(session, text, policy)` as a cancellable coroutine reporting progress events | F13 | Unit with an injected clock: delays are applied per character and per line, total time matches the policy, cancellation stops mid-paste and leaves the session usable |
| 4.2 | Wait-for-prompt: after each line, wait until received data matches the regex before sending the next; on timeout either abort or continue per policy | F13 | Unit against a scripted fake device that only prompts after a delay; asserts no line is sent early, and that a never-appearing prompt hits the timeout path rather than hanging |
| 4.3 | `PasteProgress` / `PasteFinished` events so the TUI can show a progress bar and a cancel key | F13, F22 | Unit: event sequence for a 3-line paste, and for a cancelled one |

## 5. Session logging — `portside/logging/` (F15, F16)

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 5.1 | `timestamps.py`: `off`, `absolute` (ISO-8601, local with offset), `elapsed` since connect (resettable), `delta` between lines — as a pure formatter over (line, time) | F16 | Unit with a frozen clock for each mode, including the reset, sub-millisecond deltas and a line that arrives in two chunks (one timestamp, not two) |
| 5.2 | `session_log.py`: `SessionLogger` subscribing to the M1 event bus; `text` mode (translated, timestamped, UTF-8, LF internally per PRD) and `raw` mode (exact bytes, no timestamps); manual start/stop and auto-start-on-connect | F15, F16 | Unit on `tmp_path`: both modes byte-for-byte; auto mode opens a file on connect and closes it on disconnect; a reconnect appends rather than truncating |
| 5.3 | Filename templates: `{port}`, `{device}`, `{date}`, `{time}`, `{profile}`, `{n}` — resolved with `pathlib` only, sanitised for Windows-illegal characters, directory from `platformdirs` | F15 | Unit: every placeholder; a template containing `:` and `*` is made safe on all platforms; the same template yields the same relative layout on Linux, macOS and Windows |
| 5.4 | Size-based rotation with a keep-count; rotation is atomic enough that no byte is lost across the boundary | F15 | Unit: write past the limit, assert the split point, the ordering of parts, and that concatenating the parts reproduces the stream exactly |
| 5.5 | Writes happen off the event-loop thread (buffered, flushed on an interval and on close), so logging cannot stall I/O | NFR throughput | Unit: a 5 MB stream is logged completely while the session keeps reading; asserts no data loss and that the loop was never blocked longer than a threshold |

## 6. The TUI — `portside/tui/` (F7, F8, F22, F23)

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 6.1 | `TerminalWidget`: renders `Terminal.snapshot()` into Textual, repainting on a ≤60 fps tick rather than per byte (PRD risk: "batch reads, render at max 60 fps") | F7, NFR | `App.run_test()` pilot: feed bytes through a loopback session, assert the rendered text and that N bytes cause far fewer than N repaints |
| 6.2 | Key handling: keystrokes to the wire with the M1 latency budget, function/arrow/control keys mapped to their VT sequences, backspace per §2.1 | F7, F9 | Unit on the key→bytes map (arrows, F1-F12, Ctrl-A…Ctrl-Z, Home/End/PgUp/PgDn); pilot test for the common ones |
| 6.3 | Status bar: port, baud/framing, bytes in/out, live CTS/DSR/DCD/RI, session state, logging indicator | F22, F4 | Pilot test: status text updates after traffic, after a modem-line change and when logging starts |
| 6.4 | Commands and hotkeys: send break, paste-from-file with pacing, toggle hex view, toggle timestamps, start/stop logging, find, scrollback navigation, clear, quit — in a command palette *and* on keys, with a visible help panel | F8, F10, F12, F13, F15, F16 | Pilot tests per command against a loopback session; a test asserts every command is reachable by both key and palette |
| 6.5 | Copy / select-to-copy: mouse selection and a keyboard selection mode, copy to the system clipboard where the terminal supports OSC 52, with a documented fallback | F8 | Unit on the selection model (ranges across wrapped lines and the scrollback boundary); the clipboard path is stubbed in tests |
| 6.6 | Dark and light themes plus a high-contrast theme, switchable at runtime | F8, NFR accessibility | Pilot test: switching theme changes rendered styles; a contrast check asserts the high-contrast theme meets a minimum ratio |
| 6.7 | `portside connect <port> --baud N` opens the TUI when stdout is a terminal; `--raw` keeps M1's pass-through, and piped I/O stays pass-through automatically | F23 | Existing M1 CLI tests keep passing under `--raw`; new tests assert the TUI is chosen for a tty and never for a pipe |

## 7. Profiles — `portside/profiles/` (F20) — **proposed addition, say the word and I drop it**

The PRD's milestone table puts profiles in M3, but F23 specifies `--profile X` on the TUI command
line and everything a profile stores (line settings, line endings, timestamps, logging, pacing,
break mode) exists only after M2. Building the store now costs little; the GUI in M3 then only
adds a UI over it.

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 7.1 | TOML profile store with `schema_version` and a migration hook, in `platformdirs` config dir; save/load/list/delete | F20 | Unit on `tmp_path`: round-trip of every setting; an unknown future `schema_version` is refused with a clear error rather than silently misread |
| 7.2 | Auto-apply by `DeviceIdentity` (M1 F2), including the `weak=True` case, which asks rather than assumes when two identical adapters are present | F20, F2 | Unit: the same adapter under a new port name gets its profile; two serial-less twins do not silently share one |
| 7.3 | Import/export a profile to a file, and `--profile X` on the CLI | F20, F23 | Unit: export→import round-trip; CLI applies the profile and `--baud` still overrides it |

## 8. Testing, CI and the "real Cisco switch" criterion

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 8.1 | **Fake network device**: a scripted responder over the M1 socat pair that behaves like a console — `Switch>` prompt, `enable` → `Password:`, `configure terminal` → `Switch(config)#`, echoes input, and reports whether it saw a break | Exit criteria | It *is* the test harness; an integration test configures the fake switch end to end through the TUI with paced paste and wait-for-prompt, asserting the resulting "config" |
| 8.2 | Timing tests use injected clocks, never `sleep`, so the suite stays fast and CI-stable | — | The one exception (the emulated-break duration test) is marked `slow` and bounded |
| 8.3 | CI: add `textual` to the test extra (pinned `>=1.0`, since the M1 pin of `>=0.80` predates several API breaks — Textual is now 8.x), keep the matrix at 3 OSes × Python 3.11/3.13 | — | The matrix itself; TUI pilot tests run headless on all three |
| 8.4 | Update `README.md` (TUI keys, logging, pacing, profiles) and `docs/DECISIONS.md` | — | Reviewed at M2 close |
| 8.5 | **Hardware check is yours**: I will supply `docs/HARDWARE-CHECK.md` — a short script to run against a real switch on each OS (connect, break at the boot prompt, paste a config, check the log) | Exit criteria | Manual, by you; CI cannot assert it |

## 9. Decisions I will take unless you object (recorded in DECISIONS.md)

1. **TUI becomes the default for interactive use**; `--raw` preserves M1's exact behaviour, and piped I/O stays raw automatically, so every M1 script keeps working.
2. **Emulated break picks the lowest baud the port accepts** rather than a fixed 50 baud, because not every driver accepts 50; the chosen value is reported in the status bar.
3. **Logs are UTF-8 with LF internally** in text mode (PRD rule), with raw mode for byte-exact capture. No CRLF logs on Windows.
4. **Timestamps apply per received line, not per chunk**, so a line split across two reads is stamped once, at the time its first byte arrived.
5. **Find is literal by default, regex on request** — an IP address or `Gi1/0/1` should not need escaping.
6. **The renderer batches at 60 fps**; at high baud the screen updates as fast as a human can read while the scrollback and the log stay complete.

## 10. Open questions (each has a default; none block a start)

1. **Profiles (§7) in M2 or M3?** Default: build the store in M2, because `--profile` is part of F23.
2. **F14 XON/XOFF emulation and F10/F11 (hex view, charsets)** are P1 and not named in the PRD's M2 row. Default: include them — they are small, they live in the terminal layer, and splitting them out later costs more.
3. **Clipboard**: OSC 52 works in most modern terminals but not all (and not in tmux without config). Default: OSC 52, with a "copy to file" fallback and a note in the README.


---

## M2 outcome

All of §1–§8 is implemented: **564 tests** pass (up from 281 at M1), with ruff,
`ruff format --check` and `mypy --strict` clean over `src/` and `tests/`.

Where the implementation diverged from the plan:

| Plan said | What shipped | Why |
| --- | --- | --- |
| Wrap `pyte.HistoryScreen` + `ByteStream` | Two screens, two `pyte.Stream`s, our own decoder and charset handling | pyte neither swaps buffers for the alternate screen nor dispatches `ESC ( 0` ([DECISIONS.md](DECISIONS.md) D17) |
| `PasteAborted` | `PasteAbortedError` | Matches the naming rule the linter enforces across the codebase |
| Emulated break = one zero byte at a low baud | One *or more* zero bytes | 50 baud gives 180 ms; the PRD default is 250 ms (D18) |
| §6.5 clipboard via OSC 52 | Selection model only | Textual's own mouse selection and clipboard handling cover this; a second, conflicting implementation would have been worse. Not done — see below. |

Three bugs the tests found, fixed in the product rather than the tests:

1. A paced paste waited for a prompt after its final line, turning a successful paste into a
   spurious timeout (D19).
2. The TUI's `F5` log command produced an **empty file**: the logger was never attached to the
   session's event bus.
3. `ESC ( 0` designations were applied for a whole chunk before any of its text was drawn, so a
   chunk containing both a designation and text rendered with the wrong charset.

**Not done, deliberately:** §6.5's clipboard integration. Selection and copy are left to
Textual's built-in mouse selection for now; a Portside-specific OSC 52 path on top of it would
fight the terminal rather than help it. F8's "copy / select-to-copy" is therefore only partly
satisfied in the TUI and is carried into M3, where the Qt front end has a real clipboard.

**The exit criterion is half-verified**: the automated half runs in CI against a scripted
console on a real socat pair; the hardware half is [HARDWARE-CHECK.md](HARDWARE-CHECK.md) (D28).
