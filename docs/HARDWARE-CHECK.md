# Hardware check (M2 exit criterion)

The PRD's exit criterion for M2 is:

> Can configure a real Cisco switch from the TUI on each OS.

CI cannot assert that — it has no switch and no USB adapter. What CI *does* run is a
scripted console (`tests/integration/fake_switch.py`) on the far end of a real socat serial
pair: prompts, an enable/password step, echo, a configurable think-time before each prompt,
and break detection. Everything between it and the user is the real thing — pySerial, the
session core, the emulator, pacing, logging and the TUI.

This checklist covers the other half. Run it once per release, on **Linux, macOS and
Windows**, and record the result in the release notes.

## What you need

- A switch or router with a console port (the checklist says "switch" throughout).
- A USB-serial adapter — ideally one of each: FTDI, CP210x, CH34x, PL2303, and a Cisco USB
  console cable.
- A config file to paste (`switch.cfg` below).

## Setup

```bash
pipx install portside          # or: uv run portside
portside --list                # the adapter should appear with its chip and serial number
portside connect <port> --baud 9600
```

Cisco console ports are 9600 8N1 by default; some newer gear is 115200.

## Checklist

| # | Step | Expected | Req |
| --- | --- | --- | --- |
| 1 | `portside --list` with the adapter plugged in | Port, description, VID:PID, chip family and serial number are all shown | F1 |
| 2 | Unplug and re-plug the adapter, list again | The entry disappears and returns within ~2 s | F1 |
| 3 | `portside connect <port> --baud 9600`, press Enter | The switch prompt appears; the status bar shows port, `9600 8N1`, byte counters and the modem lines | F7, F22, F4 |
| 4 | Type `show version` and Enter | Output renders, including any colour and box drawing; no stray characters | F7 |
| 5 | `show running-config` on a long config | Output scrolls; PgUp/PgDn move through the scrollback and new output does not yank the view back | F8 |
| 6 | Press F3, search for an interface name | Matches are highlighted; Enter moves between them | F8 |
| 7 | Press F6 | The hex view shows the same bytes; F6 again returns to text | F10 |
| 8 | Press F5, run a command, press F5 again | A log file is written, containing exactly what you saw | F15 |
| 9 | Press F7 to cycle timestamps, log again | Lines carry absolute / elapsed / delta stamps as selected | F16 |
| 10 | Reboot the switch and press F2 during boot | The switch drops to ROMMON — this is the break test, and the one most likely to expose a driver that cannot do it natively | F12 |
| 11 | If step 10 fails, reconnect with `--break-mode emulated` and repeat | The emulated break interrupts boot; the status line reports the baud rate it used | F12 |
| 12 | Press F4 and paste `switch.cfg` | Every line arrives, in order, with no dropped characters; the progress indicator advances | F13 |
| 13 | Repeat step 12 with a profile whose `wait_for_prompt` is `[>#]\s*$` | Lines are sent only after the switch prompts | F13 |
| 14 | Unplug the adapter mid-session | Portside reports the device vanished and does not crash or hang | F5, stability |
| 15 | With `--reconnect`, re-plug the adapter | The session reconnects, even if the port came back under a different name | F5, F2 |
| 16 | Save a profile, unplug, re-plug, reconnect | The profile is applied automatically for that adapter | F20, F2 |
| 17 | Run the same profile file on the other two OSes | Identical behaviour; the profile needs no edits | Goal 1 |

### Sample config for step 12

```
configure terminal
hostname lab-sw1
interface GigabitEthernet1/0/1
 description uplink to core
 switchport mode trunk
end
```

### Permission check (Linux only)

Before adding yourself to the `dialout`/`uucp` group, try connecting once. Portside should
refuse with the exact command for *your* distribution — `dialout` on Debian/Ubuntu, `uucp` on
Arch — rather than a generic error. That is requirement F6, and it is worth verifying on a
fresh machine where the group has not yet been granted.

## Known limits of the CI substitute

- The fake switch sees a break as a NUL byte, because that is what an emulated break looks
  like on a PTY. A **native** break (`--break-mode native`) cannot be tested without hardware;
  only step 10 covers it.
- No real driver is exercised, so driver-specific quirks (CH34x clones, counterfeit FTDI,
  PL2303 revisions) only show up in this checklist.
- Timing on a PTY is not serial timing: pacing is verified for *ordering and waiting*, not for
  what a 9600-baud console does to a 400-line paste.
