# Portside — M1 (Core) Implementation Plan

Status: **approved and implemented** (see *M1 outcome* at the end). Scope is milestone **M1**
from [PRD.md](PRD.md) only:
transport interface, pySerial transport, port discovery, session core, CLI `--list` / `connect`
in raw pass-through mode.

Exit criteria (PRD): *"Loopback and socat tests pass on all 3 OSes in CI."*

Out of scope for M1 (explicitly deferred): pyte emulation, Textual TUI, logging/timestamps,
break + paced paste (M2), PySide6 GUI (M3), file transfers (M4), direct USB (M6).

---

## 0. Conventions used below

- **Req IDs** are the PRD functional requirement IDs. "(partial)" means M1 lands the
  UI-independent core of that requirement and a later milestone completes it.
- Every task lists how it is tested. Test layout mirrors the source tree under `tests/`.
- Test tiers: `unit` (no hardware, no OS deps), `loop` (pySerial `loop://` URL),
  `socat` (integration, marked `@pytest.mark.socat`, skipped on Windows / when socat is absent).

---

## 1. Repository and tooling scaffold

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 1.1 | `pyproject.toml` (hatchling, src layout, `requires-python = ">=3.11"`), runtime deps pinned to the PRD stack; `pyudev` behind a `[project.optional-dependencies]` / environment marker `sys_platform == "linux"`; console script `portside = portside.cli.main:main` | — | `pip install -e .` in CI; import smoke test |
| 1.2 | Package skeleton `src/portside/{core,transport,emulation,transfer,profiles,logging,platform,gui,tui,cli}` — M1 fills core/transport/platform/cli, the rest get a docstring-only `__init__.py` so the layout is fixed from day one; `py.typed` marker | — | `test_layout.py` asserts every package imports and `py.typed` ships |
| 1.3 | Tool config: ruff (lint + format), mypy `--strict` over `src/` and `tests/`, pytest (`asyncio_mode=auto`, custom markers `socat`, `slow`) | — | Linters run in CI as their own job |
| 1.4 | `portside.logging` name shadows stdlib `logging` inside the package — all modules use absolute imports and the package never does `import logging` from within a `from . import` context | — | mypy --strict + a unit test importing `portside.logging` and stdlib `logging` in the same module |

**Resolved:** `uv` is not installed on this machine. I will still write the
project for uv (`uv.lock`, `uv sync`, uv-based CI) and, locally, fall back to
`python -m venv` + `pip` to run tests unless you want me to install uv first
(`curl -LsSf https://astral.sh/uv/install.sh | sh` — a network install, so I'll ask before running it).

---

## 2. Typed error model (F6)

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 2.1 | `core/errors.py`: `PortsideError` base → `TransportError` → `PortNotFoundError`, `PortBusyError`, `PermissionDeniedError`, `DeviceVanishedError`, `InvalidSettingsError`, `TransportClosedError`. Each carries `port`, a short `message`, and a **`remedy: str \| None`** the UI can display verbatim | F6 | Unit: attributes present, `str()` stable, all subclasses catchable as `PortsideError` |
| 2.2 | `platform/*.permission_remedy()` — Linux returns the exact group fix, choosing `dialout` vs `uucp` by inspecting which group owns the device node (falls back to `dialout`): `sudo usermod -aG dialout $USER` then log out/in; macOS returns a driver/claimed-port hint; Windows returns a "port held by another app" hint | F6 | Unit per platform module with a monkeypatched `os.stat`/group lookup; the Linux `usermod` string is asserted literally |
| 2.3 | `transport/errors.py::map_serial_exception()` — translate `serial.SerialException` / `OSError` (errno EACCES, EBUSY, ENOENT, ENODEV, EIO) into the typed errors above, including Windows `ERROR_ACCESS_DENIED (5)` and `ERROR_FILE_NOT_FOUND (2)` | F6 | Unit: table-driven over synthetic exceptions for all three OSes — runs everywhere, no real ports needed |

---

## 3. Transport layer

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 3.1 | `core/settings.py`: frozen `SerialSettings` dataclass — baud (50–4,000,000 + custom), data bits 5–8, parity N/E/O/M/S, stop 1/1.5/2, flow control none/rtscts/dsrdtr/xonxoff — as enums, with `validate()` raising `InvalidSettingsError` | F3 | Unit + **hypothesis**: valid combos round-trip through `to_dict`/`from_dict`; out-of-range values always raise |
| 3.2 | `transport/base.py`: `Transport` ABC — `open()`, `close()`, `read(max_bytes, timeout)`, `write(data)`, `apply_settings()`, `send_break(duration)`, `set_dtr/set_rts`, `get_modem_lines() -> ModemLines(cts, dsr, dcd, ri)`, `cancel_read()`, `is_open`. Blocking-by-design; the async bridge lives in the session | F3, F4, F12 (partial) | Unit: an abstract-conformance test parametrised over every concrete transport, so `LoopbackTransport` and `PySerialTransport` are held to one contract |
| 3.3 | `transport/serial_transport.py`: `PySerialTransport` — wraps `serial.Serial`/`serial_for_url`, all exceptions funnelled through `map_serial_exception`, `cancel_read()` uses pySerial's `cancel_read` where available | F3, F4, F6 | `loop`: open `loop://`, settings applied, write→read round-trip, `close()` idempotent, post-close ops raise `TransportClosedError` |
| 3.4 | `transport/loopback.py`: `LoopbackTransport` — in-memory, deterministic; scriptable faults (`fail_open_with=PermissionDeniedError`, `vanish_after_n_bytes=…`) so the session's error paths are testable without hardware | F5, F6 | Used by session tests; its own unit tests for echo, EOF, fault injection |
| 3.5 | Modem-line + DTR/RTS handling tolerant of drivers that don't implement them (`NotImplementedError` → `None` in `ModemLines`, never a crash) | F4 | Unit with a fake `serial.Serial` whose properties raise |

---

## 4. Discovery and device identity

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 4.1 | `transport/discovery.py`: `PortInfo` dataclass — `device`, `description`, `vid`, `pid`, `serial_number`, `manufacturer`, `product`, `chip_family`, `driver`, `stable_path` | F1 | Unit against a monkeypatched `serial.tools.list_ports.comports()` fixture set (FTDI, CP2102, CH340, PL2303, CDC-ACM, plain COM1) |
| 4.2 | `transport/chips.py`: VID:PID → chip family table (FTDI 0403, Silicon Labs 10c4, WCH 1a86, Prolific 067b, common CDC-ACM), unknown → `None` (never a guess) | F1 | Unit table test; unknown VID returns `None` |
| 4.3 | `platform/` port filtering: **macOS lists `/dev/cu.*` and hides `/dev/tty.*`**; Linux adds `/dev/serial/by-id` stable paths and the bound kernel driver (from sysfs, best-effort); Windows passes through pySerial's COM naming incl. >COM9 | F1 | Unit: each platform module fed a synthetic port list, asserted on the *filtered* result — all three run on every CI OS (no real devices) |
| 4.4 | `core/identity.py`: `DeviceIdentity` = VID:PID + serial number (+ description fallback when the adapter has no serial), with a stable `key()` string safe as a TOML table name — this is what M2+ profiles key off | F2 | Unit: same adapter on `COM3` and `/dev/ttyUSB0` yields the same key; two identical chips with different serials differ; serial-less adapters degrade deterministically |
| 4.5 | `platform/hotplug.py`: `watch_ports()` async iterator — pyudev on Linux (optional import, guarded), 1 s polling fallback elsewhere; emits `PortAdded` / `PortRemoved` within 2 s | F1, F5 | Unit: polling watcher driven by a fake clock + fake `comports()` detects add/remove inside the budget; pyudev path unit-tested with a stubbed module, skipped when pyudev is absent |

---

## 5. Session core (asyncio)

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 5.1 | `core/session.py`: `Session` — owns one transport, state machine `DISCONNECTED → CONNECTING → CONNECTED → RECONNECTING → CLOSED`, byte counters in/out, `async send(bytes)`, `rx` stream for consumers | F22 (partial: counters/status the UI shows) | Unit with `LoopbackTransport`: state transitions, counters, clean `close()` from every state |
| 5.2 | Thread↔asyncio bridge: one dedicated reader thread per session calling `transport.read()`, handing buffers to the loop via `call_soon_threadsafe`; writes serialised through a single-worker executor so ordering is preserved. **The consumer/UI thread never touches the port.** | Architecture constraint; NFR latency/throughput | Unit: interleaved concurrent writes arrive in order; a stress test pushes ~5 MB through the loopback transport asserting zero loss/reordering; `close()` joins the thread within a timeout (no hang, no leaked thread — asserted via `threading.enumerate()`) |
| 5.3 | Backpressure: bounded rx queue with a documented drop/pause policy so a slow consumer can't grow memory without bound | NFR memory/throughput | Unit: slow consumer, assert bounded memory and that the policy is applied (not silently unbounded) |
| 5.4 | Auto-reconnect supervisor: on `DeviceVanishedError`, re-scan by `DeviceIdentity` (not by port name) with capped backoff, re-apply settings, emit status events; opt-in in M1's CLI via `--reconnect` | F5, F2 | Unit: loopback transport set to vanish after N bytes; assert `RECONNECTING` → `CONNECTED`, settings re-applied, and that a device returning under a *different* port name is still matched by identity |
| 5.5 | `core/events.py`: typed event objects (`StateChanged`, `BytesReceived`, `ErrorRaised`, `PortListChanged`) on a fan-out bus — the single seam the TUI/GUI will attach to in M2/M3 | F22 (partial) | Unit: multiple subscribers, unsubscribe, a slow/raising subscriber never kills the session |

---

## 6. CLI (raw pass-through)

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 6.1 | `cli/main.py` (argparse): `portside --list` — table with device, description, VID:PID, chip, serial, driver; `--json` for scripts; `--verbose` adds stable paths | F1, F23 (partial) | Unit: `capsys` over a monkeypatched port list; JSON output parsed and schema-asserted; empty-list case prints a helpful message, exit code 0 |
| 6.2 | `portside connect <port> --baud N [--data-bits --parity --stop-bits --flow --reconnect]` — raw pass-through, no emulation, no translation | F3, F23 (partial), F7 (raw mode half) | Unit: argument parsing → `SerialSettings` mapping, including rejection of bad values with exit code 2 |
| 6.3 | `platform/console.py`: raw stdin/stdout for the pass-through loop — termios/tty on POSIX, msvcrt on Windows, behind one interface; always restored via `finally`, including on `SIGINT`/exception | Cross-platform constraint | Unit: POSIX path with a pty asserts the termios state is restored; Windows path unit-tested with a stubbed `msvcrt`; a non-tty stdin (piped input) takes a documented fallback path so CI can drive it |
| 6.4 | Escape key to quit (`Ctrl-]`, mentioned on connect banner), clean shutdown, and **exit codes**: 0 ok, 2 usage, 3 permission denied, 4 busy, 5 not found/vanished — so scripts and the future TUI can react | F6, F23 | Unit: each typed error is driven through the CLI entry point and the exit code + the human-readable remedy on stderr are asserted |
| 6.5 | End-to-end CLI test over `loop://` and over a socat pair: pipe bytes in, assert bytes out, assert the process exits cleanly | F23 | `loop` + `socat` |

---

## 7. Integration, CI, docs

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 7.1 | `tests/conftest.py`: `socat_pair` fixture spawning `socat -d -d pty,raw,echo=0 pty,raw,echo=0`, parsing both PTY paths, guaranteed teardown; `pytest.mark.skipif` on Windows / missing socat — **skips cleanly, never fails** | Exit criteria | Meta-test: the fixture's skip path is exercised by forcing `shutil.which` to return `None` |
| 7.2 | Integration suite over the socat pair: open both ends, full-duplex transfer, settings application, unplug simulated by killing socat → asserts `DeviceVanishedError` and no crash/hang | F5, F6, NFR stability | `socat` |
| 7.3 | `.github/workflows/ci.yml`: matrix ubuntu/macos/windows-latest × Python 3.11 and 3.13; jobs = ruff (lint+format check), mypy --strict, pytest with coverage; uv with a cached lockfile; socat installed on ubuntu/macos so the integration tier actually runs there | Exit criteria | The workflow itself is the test; I'll validate the YAML locally and rely on the first push for the real matrix |
| 7.4 | `README.md`: install/run for Linux, macOS, Windows (pipx/uv/pip), the dialout-vs-uucp permission note, quickstart for `--list` and `connect`, dev setup | — | Manual; commands cross-checked against the CI job steps |
| 7.5 | `docs/DECISIONS.md`: every ambiguity resolved, with rationale | Instruction 6 | Reviewed at M1 close |

---

## 8. Decisions already taken (will be recorded in docs/DECISIONS.md)

1. **PRD filename** — the exported doc was `docs/Portside — Cross-Platform Serial Terminal (PRD).md`; renamed to `docs/PRD.md` as the prompt specifies.
2. **Break in M1** — `send_break` is on the `Transport` ABC (it belongs to the interface the PRD
   defines) but *emulated* break and duration policy are M2 (F12). M1 only proves the native call.
3. **Raw mode only** — no CR/LF translation, no local echo, no charset handling in M1 (F9/F11 are M2).
4. **Reconnect is opt-in in the CLI** (`--reconnect`) so the default pass-through stays predictable
   for scripting; the session core supports it either way.
5. **Escape key `Ctrl-]`** to leave `connect`, matching the common telnet convention; configurable later.
6. **`portside.logging`** is the PRD-specified package name and shadows the stdlib module inside the
   package; handled with absolute imports rather than renaming the package.
7. **Serial-less adapters** (many CH340s report no serial number) fall back to
   VID:PID + port description for `DeviceIdentity`, and the identity is flagged `weak=True` so M2's
   profile auto-apply can decide how much to trust it.

## 9. Open questions for you (not blocking — I have a default for each)

1. **uv install** — install uv locally (network) or run tests via venv+pip locally while CI uses uv?
   Default: venv+pip locally, uv in CI.
2. **License** — PRD open question lists MIT or Apache-2.0. Default: MIT, `LICENSE` added at M1.
3. **git** — this directory is not a git repo yet. Default: `git init`, branch `m1-core`, commit at M1 close.


---

## M1 outcome

Every task in §1–§7 is implemented, with 281 tests passing and ruff, `ruff format --check` and
`mypy --strict` clean over `src/` and `tests/`. Where the implementation diverged from the plan:

| Plan said | What shipped | Why |
| --- | --- | --- |
| `PortInfo` in `transport/discovery.py` | `core/ports.py`, re-exported from discovery | The platform layer needs it too; keeping it in `transport` created an import cycle |
| `resolve_port` raises for unknown port names | Unknown names are passed to the driver | Discovery misses RFCOMM nodes, PTYs and some on-board UARTs — see [DECISIONS.md](DECISIONS.md) D5 |
| `set_dtr`/`set_rts` return `None` | They return `bool` | PTYs and several adapters have no such line; that is a limitation to report, not a session-ending error (D4) |
| `--eof-drain` | `--drain` | It now applies to `Ctrl-]` as well as EOF, so the device's last bytes are never cut off (D7) |
| — | `tests/` is a package (`__init__.py` files) | Lets the integration tests import the socat helpers from `tests/conftest.py` |

Two product bugs were found by the socat tier and fixed in the product rather than the tests:
DTR/RTS raising `ENOTTY` on ports that lack those lines, and `connect` refusing any port that
enumeration did not list.

Deferred to M2 as planned: emulated break and break duration policy (F12), CR/LF translation,
local echo, character sets (F9, F11), logging and timestamps (F15, F16), paced paste (F13).
