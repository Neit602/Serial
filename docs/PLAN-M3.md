# Portside — M3 (GUI) Implementation Plan

Status: **approved and implemented** (see *M3 outcome* at the end). Scope is milestone **M3** from [PRD.md](PRD.md):
the PySide6 desktop app — device list, tabs, settings, profiles, signals panel, themes.

Exit criteria (PRD): *"Usability pass: new user connects in under 30 s."*
That is a human measurement; §9 says what I can automate and what stays yours.

Builds on M1 (`m1-core`) and M2 (`m2-terminal`). M3 work goes on `m3-gui`.

Out of scope, unchanged: file transfers (M4), installers and notarisation (M5), direct USB,
scripting API and macros (M6, which is where the PRD puts F21).

---

## 1. Qt ↔ asyncio bridge — `portside/gui/bridge.py`

The core is asyncio; Qt has its own event loop. They have to meet somewhere, and that
somewhere must keep the PRD's rule that **the UI thread never touches the port**.

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 1.1 | `AsyncRuntime`: owns an asyncio loop on a dedicated thread, with `submit(coro)` returning a future and a clean shutdown that joins the thread | Architecture | Unit: coroutines run off the GUI thread, exceptions surface to the caller, shutdown leaves no thread behind (`threading.enumerate()`) |
| 1.2 | `SessionController(QObject)`: wraps one `Session`, republishes its event bus as Qt signals (`dataReceived`, `stateChanged`, `errorRaised`, `reconnecting`, `pasteProgress`) using queued connections so they land on the GUI thread | F22, Architecture | Unit with `LoopbackTransport`: every core event becomes exactly one Qt signal, emitted on the GUI thread (asserted by thread id) |
| 1.3 | Typed wrappers for the actions a UI needs: `send`, `send_break`, `set_dtr/rts`, `modem_lines`, `apply_settings`, `paste_file`, `close` — all returning futures, never blocking the GUI | F3, F4, F12, F13 | Unit: each call reaches the session; a slow transport does not block the Qt loop (asserted by timing a paint during a 1 s write) |

**Decision to confirm (§10.1):** do this with a worker-thread loop and Qt signals rather than
adding `qasync`. It keeps the PRD's dependency list exact and makes the thread boundary
explicit instead of hiding it.

## 2. Terminal widget — `portside/gui/terminal_widget.py`

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 2.1 | `TerminalWidget(QWidget)` painting a `Snapshot` with `QPainter`: per-cell colours and attributes, block cursor, alternate screen, repaint on a ≤60 fps timer driven by the emulator's revision counter | F7, NFR latency | pytest-qt: feed bytes, assert the painted text via a render-to-`QImage` probe and the widget's own `text_at(row)`; assert N bytes cause far fewer than N paints |
| 2.2 | Font family and size, with a monospace-only picker and live re-layout; the emulator is resized to the widget in character cells | F8 | pytest-qt: changing the font changes the cell grid; the session's emulator is resized to match |
| 2.3 | Mouse selection, select-to-copy option, `Ctrl+C`/`Ctrl+Shift+C` copy to the **system clipboard** — this is the half of F8 that M2 deliberately left to the GUI | F8 | pytest-qt: drag-select across wrapped lines and the scrollback boundary, assert `QApplication.clipboard()` contents; select-to-copy option on and off |
| 2.4 | Scrollbar wired to the emulator's scrollback, wheel and keyboard scrolling, "jump to tail on keypress" | F8 | pytest-qt: scrollbar position follows `scroll_offset`; typing returns to the tail |
| 2.5 | Find bar (Ctrl+F) with next/previous and match highlighting, reusing M2's `emulation.find` | F8 | pytest-qt: matches highlighted, wrap-around, no-match state |
| 2.6 | Hex view toggle rendering M2's `hexview` in the same widget | F10 | pytest-qt: toggling shows hex for the same bytes |

## 3. Main window, tabs and windows — `portside/gui/main_window.py`

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 3.1 | `MainWindow`: menu bar, toolbar, `QTabWidget` of sessions, dockable device list, status bar | F22 | pytest-qt smoke: window builds, menus exist, no session open shows a helpful empty state |
| 3.2 | Multiple sessions as tabs: open, close (with a confirm when connected), rename, reorder; each tab owns one `SessionController` | F22 | pytest-qt: three sessions on three loopback transports run independently; closing one leaves the others connected |
| 3.3 | Detach a tab into its own window, and re-attach; `File ▸ New Window` | F22 | pytest-qt: detaching moves the widget and keeps the session connected (no reconnect, counters continue) |
| 3.4 | Per-session status bar: port, framing, bytes in/out, state, logging indicator, scrollback position | F22 | pytest-qt: status text updates after traffic and on state changes |
| 3.5 | Session lifecycle on window close: every session closed, every thread joined, no orphan runtime | Stability | pytest-qt: close the window with sessions open; assert no leftover threads |

## 4. Device list — `portside/gui/device_list.py`

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 4.1 | Dock panel listing ports with friendly name, chip family, VID:PID and serial number, sorted USB-first (reuses M1 discovery) | F1 | pytest-qt with a synthetic port list: every column rendered, empty state explains what to check |
| 4.2 | Hot-plug refresh within 2 s using M1's watcher (pyudev on Linux, polling elsewhere), with added/removed rows animating in place rather than rebuilding the list | F1 | pytest-qt with a scripted lister: an added port appears, a removed one disappears, selection survives a refresh |
| 4.3 | Double-click (or Enter, or the Connect button) opens a session in a new tab, applying the device's profile if one is bound | F2, F20, F23 | pytest-qt: double-click connects; a bound profile is applied; counted as one of the ≤3 actions in §9.1 |
| 4.4 | Context menu: connect, connect with settings…, copy port name, save as profile | F20 | pytest-qt per action |

## 5. Settings and profiles — `portside/gui/settings_dialog.py`, `profiles_dialog.py`

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 5.1 | Connection settings dialog: baud (editable combo, custom values), data bits, parity, stop bits, flow control — validated through `SerialSettings`, invalid input blocked at the dialog rather than at connect time | F3 | pytest-qt: every control round-trips to a `SerialSettings`; a bad custom baud is refused with a message |
| 5.2 | Terminal settings: line endings both directions, local echo, backspace, charset, strip-high-bit, scrollback depth, font | F8, F9, F10, F11 | pytest-qt: round-trip to `TextPolicy` + `TerminalOptions` |
| 5.3 | Logging settings: on/off, text vs raw, directory picker, filename template with a live preview, rotation, timestamp mode | F15, F16 | pytest-qt: round-trip to `LogSettings`; the preview matches `resolve_template` |
| 5.4 | Pacing and break settings: char/line delay, wait-for-prompt regex (validated live), prompt timeout, break mode and duration | F12, F13 | pytest-qt: round-trip to `PacePolicy`; an invalid regex is marked invalid as it is typed |
| 5.5 | Profiles dialog: list, create from the current session, edit, duplicate, delete, import, export, and bind/unbind the connected device | F20, F2 | pytest-qt against a temporary `ProfileStore`: every action; binding a device makes the next connect auto-apply |
| 5.6 | Apply settings to a **live** session without dropping it | F3 | pytest-qt: change baud mid-session, assert the transport was reconfigured and the session stayed connected |

## 6. Signals panel — `portside/gui/signals_panel.py`

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 6.1 | Live CTS/DSR/DCD/RI indicators, polled once a second, with three visual states — asserted, low and **unknown** (M2's `ModemLines` reports `None` for lines a driver does not implement, and the UI must not claim "low") | F4 | pytest-qt: a transport reporting `None` renders the unknown state, not "off" |
| 6.2 | DTR and RTS toggle buttons, disabled with an explanatory tooltip when the driver reports no such line | F4 | pytest-qt: toggling calls the session; a transport without control lines disables the buttons |
| 6.3 | Break button with its duration, reporting native vs emulated in the status bar | F12 | pytest-qt: clicking sends a break; the emulated path is reported as such |

## 7. Session actions and error surfacing

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 7.1 | Paste file with pacing, a progress bar and a cancel button; drag-and-drop a file onto the terminal does the same | F13, F19 (drag-and-drop half) | pytest-qt: paste and cancel; a dropped file starts the same flow |
| 7.2 | Logging start/stop from the toolbar, with the path shown and a "reveal in file manager" action | F15 | pytest-qt: toggling writes a file; the indicator matches |
| 7.3 | Error dialogs that show the typed error's `remedy` **verbatim** — the whole point of F6's design — with a "copy command" button for the Linux group fix | F6 | pytest-qt: a permission error shows `sudo usermod -aG dialout $USER`; busy and vanished errors show their own remedies |
| 7.4 | Auto-reconnect surfaced in the UI: a visible "reconnecting…" state with attempt count, and a cancel | F5 | pytest-qt with a vanishing loopback transport: state shown, reconnect succeeds, UI returns to connected |

## 8. Themes, accessibility, i18n

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 8.1 | Dark, light and high-contrast themes as Qt stylesheets, following the OS theme by default and switchable at runtime; the terminal palette changes with them | F8, NFR accessibility | pytest-qt: switching restyles both chrome and terminal; a contrast check asserts the high-contrast theme's ratios |
| 8.2 | Full keyboard navigation: tab order, mnemonics, shortcuts for every toolbar action, and a shortcuts reference | NFR accessibility | pytest-qt: every toolbar action has a shortcut; tab order reaches every control without the mouse |
| 8.3 | Screen-reader labels (`setAccessibleName`/`setAccessibleDescription`) on every control, including the signal indicators | NFR accessibility | pytest-qt: assert no interactive widget lacks an accessible name |
| 8.4 | All user-visible strings through `self.tr()`, with `lupdate` wiring and a `docs/I18N.md`; English only in v1 | NFR i18n | A test scans the GUI package for string literals passed to Qt setters outside `tr()` |

## 9. Tests, CI, docs — and the usability criterion

| # | Task | Req IDs | How it is tested |
| --- | --- | --- | --- |
| 9.1 | **Automatable half of the exit criterion**: a "first run" test that starts from a cold app with a synthetic adapter present and asserts a connected session in **≤3 user actions** with no typing, and that the app is interactive within the PRD's 2 s cold-start budget | Exit criteria, NFR startup | pytest-qt, headless, timed; fails if the path grows an extra click |
| 9.2 | **Human half**: `docs/USABILITY-CHECK.md` — a scripted 30-second test for a new user on each OS (plug in, launch, connect, type), with what to record and what counts as a pass | Exit criteria | Manual, by you; CI cannot assert it |
| 9.3 | pytest-qt across the suite, headless via `QT_QPA_PLATFORM=offscreen`, with a `gui` marker so `pytest -m "not gui"` still runs everywhere | — | The suite itself |
| 9.4 | CI: install the `gui` extra and the Linux Qt runtime libraries (`libegl1`, `libxkbcommon-x11-0`, `libxcb-*`), keep the 3 OS × Python 3.11/3.13 matrix | — | The matrix. **Risk: this is the step most likely to break first** — see §10.4 |
| 9.5 | Memory check: 4 sessions × 10,000-line scrollback stays under the PRD's 200 MB | NFR memory | A marked `slow` test measuring RSS after opening four loopback sessions and filling their scrollback |
| 9.6 | `README.md` GUI section, `docs/DECISIONS.md`, `docs/PLAN-M3.md` outcome | — | Reviewed at M3 close |

## 10. Decisions I will take unless you object

1. **Thread-bridge, not `qasync`.** An asyncio loop on its own thread, results delivered as Qt
   queued signals. No dependency outside the PRD's stack, and the thread boundary stays visible.
2. **Custom `QPainter` terminal widget**, not `QPlainTextEdit`. A text widget fights you over
   per-cell attributes, the cursor, the alternate screen and selection semantics; painting cells
   is less code than working around it, and it is what makes the 60 fps cap meaningful.
3. **The GUI reuses M2's core verbatim** — emulator, pacing, logging, profiles, break. If a
   feature needs changing for the GUI, it changes in the core and the TUI gets it too.
4. **`gui` stays an optional extra.** `pip install portside` keeps working without Qt; the GUI
   is `pip install portside[gui]`, and the `portside-gui` entry point says so clearly if Qt is
   missing. (M5's installers bundle it.)
5. **The GUI is not the default for `portside connect`.** The command line stays the CLI/TUI;
   the GUI launches as `portside-gui` (or `portside gui`).

## 11. Open questions (each has a default; none block a start)

1. **Session restore** — should the app reopen last session's tabs on launch? Default: no, but
   remember window geometry and the last-used profile per device.
2. **Drag-and-drop to send a file** is listed in F19 with the transfer features (M4). The
   drag-and-drop *plumbing* is cheap to add now for paced paste. Default: paste now, binary send
   in M4.
3. **Qt in CI**: installing PySide6 on three OSes adds a few minutes per job. Default: do it —
   the PRD explicitly asks for pytest-qt smoke tests on all three, and a GUI that is only ever
   run on Linux will break on the others.


---

## M3 outcome

All of §1–§9 is implemented: **684 tests** pass (up from 564 at M2), with ruff,
`ruff format --check` and `mypy --strict` clean over `src/` and `tests/`. 120 of those tests are
new Qt tests, all headless.

Where the implementation diverged from the plan:

| Plan said | What shipped | Why |
| --- | --- | --- |
| §2.4 scrollbar wired to the emulator | Wheel, PgUp/PgDn and `follow_tail()`, no scrollbar widget | The scrollback belongs to the emulator, not to a Qt scroll area; a real scrollbar would need a second source of truth for the same position. Keyboard and wheel scrolling cover the requirement; a visible scrollbar is cosmetic and is noted as a gap below. |
| §5.6 "apply settings to a live session" | Done, and the dialog preserves fields it has no control for | Overwriting the log charset from the terminal charset was silent data loss ([DECISIONS.md](DECISIONS.md) D34) |
| §7.1 drag-and-drop as F19's half | Dropping a file pastes it at the session's pace | Binary send stays in M4 where the PRD puts it (open question 2, as agreed) |

Three bugs the tests found, fixed in the product rather than the tests:

1. **Resizing lost terminal output.** pyte's `resize` drops the rows that no longer fit and its
   sparse buffer can duplicate one; opening the find bar was enough to trigger it. Fixed in
   `Terminal.resize`, which now moves those rows into the scrollback with their colours (D31).
   This was a latent M2 bug — the TUI had it too, and now does not.
2. **The GUI re-resolved every port through a second discovery scan**, losing the device identity
   for ports enumeration does not list, which broke profile auto-apply (D32).
3. **The settings dialog overwrote the profile's log character set** with the terminal's (D34).

**Not done, deliberately:**

* **No visible scrollbar** in the terminal widget (see the table above). Scrolling works by wheel
  and keyboard; a scrollbar is a cosmetic addition for M5's polish pass.
* **No session restore** on launch — window geometry and the per-device profile are remembered,
  the open tabs are not (open question 1, as agreed).
* **English only.** Every user-visible string goes through `tr()` and a test enforces it, but no
  `.ts` files are shipped: the PRD asks for externalised strings in v1, not translations.

**The exit criterion is half-verified**: the automated half asserts a ≤3-action, no-typing path
from a cold window to a connected session; the human half is
[USABILITY-CHECK.md](USABILITY-CHECK.md) (D37).
