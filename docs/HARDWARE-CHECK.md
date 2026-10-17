# Hardware check (M2 and M4 exit criteria)

The PRD's exit criterion for M2 is:

> Can configure a real Cisco switch from the TUI on each OS.

The C++ port has no TUI (DECISIONS D44): the checklist runs in the desktop app, with the
command line covering the steps a headless box needs.

CI cannot assert any of this — it has no switch and no USB adapter. What CI *does* run is a
scripted console (`tests/support/fake_switch.cpp`) on the far end of a real socat serial
pair: prompts, an enable/password step, echo, a configurable think-time before each prompt,
and break detection. Everything between it and the user is the real thing — QSerialPort, the
session core, pacing, logging and the CLI (`tests/integration/tst_integration.cpp`).

This checklist covers the other half. Run it once per release, on **Linux, macOS and
Windows**, and record the result in the release notes.

## What you need

- A switch or router with a console port (the checklist says "switch" throughout).
- A USB-serial adapter — ideally one of each: FTDI, CP210x, CH34x, PL2303, and a Cisco USB
  console cable.
- A config file to paste (`switch.cfg` below).

## Setup

Build and install Portside as the README describes, then:

```bash
portside --list                # the adapter should appear with its chip and serial number
portside-gui                   # pick the adapter, Ctrl+Shift+N to set 9600 first
```

Cisco console ports are 9600 8N1 by default; some newer gear is 115200.

## Checklist

| # | Step | Expected | Req |
| --- | --- | --- | --- |
| 1 | `portside --list` with the adapter plugged in | Port, description, VID:PID, chip family and serial number are all shown | F1 |
| 2 | Unplug and re-plug the adapter, list again | The entry disappears and returns within ~2 s | F1 |
| 3 | Connect at 9600, press Enter | The switch prompt appears; the status line shows port, `9600 8N1`, byte counters, and the CTS/DSR/DCD/RI indicators | F7, F22, F4 |
| 4 | Type `show version` and Enter | Output renders, including any colour and box drawing; no stray characters | F7 |
| 5 | `show running-config` on a long config | Output scrolls; PgUp/PgDn and the wheel move through the scrollback and new output does not yank the view back | F8 |
| 6 | `Ctrl+F`, search for an interface name | Matches are highlighted and counted; Enter moves between them | F8 |
| 7 | `Ctrl+H` | The hex view shows the same bytes; `Ctrl+H` again returns to text | F10 |
| 8 | `Ctrl+L`, run a command, `Ctrl+L` again | A log file is written, containing exactly what you saw | F15 |
| 9 | Set timestamps in Session settings (`Ctrl+,`), log again | Lines carry absolute / elapsed / delta stamps as selected | F16 |
| 10 | Reboot the switch and press `Ctrl+B` during boot | The switch drops to ROMMON — this is the break test, and the one most likely to expose a driver that cannot do it natively | F12 |
| 11 | If step 10 fails, set Break mode to "Emulated only" and repeat | The emulated break interrupts boot; the status bar reports the baud rate it used | F12 |
| 12 | `Ctrl+Shift+V` (or drop the file on the window) and paste `switch.cfg` | Every line arrives, in order, with no dropped characters; the progress bar advances | F13 |
| 13 | Repeat step 12 with a profile whose `wait_for_prompt` is `[>#]\s*$` | Lines are sent only after the switch prompts | F13 |
| 14 | Unplug the adapter mid-session | The status line says the device is gone; Portside does not crash or hang | F5, stability |
| 15 | Re-plug the adapter | The session reconnects, even if the port came back under a different name | F5, F2 |
| 16 | Save a profile, unplug, re-plug, reconnect | The profile is applied automatically for that adapter | F20, F2 |
| 17 | Run the same profile file on the other two OSes | Identical behaviour; the profile needs no edits | Goal 1 |
| 18 | `portside connect <port> --baud 9600`, then `Ctrl-]` `b` during a reboot | The headless path also reaches ROMMON; `Ctrl-]` `q` quits | F12, F23 |
| 19 | `portside connect <port> --reconnect`, unplug and re-plug | Status lines on stderr report the loss and the reconnect | F5 |

## File transfers (M4)

Interop with lrzsz and Kermit runs in CI (`tst_interop`); real gear adds the parts no PTY
can: slow links, flow control and bootloaders with their own ideas about XMODEM.

| # | Step | Expected | Req |
| --- | --- | --- | --- |
| 20 | In ROMMON or U-Boot, start an XMODEM receive (`xmodem -c image`, `loadx`), then Transfer ▸ Send file with XMODEM (and again with XMODEM-1K) | The image arrives; the device verifies it; the panel shows progress and retries | F17 |
| 21 | U-Boot `loady`, then send with YMODEM | The file arrives with its exact size | F17 |
| 22 | On a Linux board, run `sz somefile` in the terminal | Portside offers to receive it; accepting saves it to Downloads with the right name and date | F18 |
| 23 | Run `rz` on the board | Portside asks what to send; the file arrives intact | F18 |
| 24 | Start a 10 MB ZMODEM send at 115200 and unplug the adapter halfway | Portside reports the loss within seconds, keeps a `.part` file, and the session reconnects when the adapter returns | F18, stability |
| 25 | Receive the same file again with "Continue an interrupted download" | Only the missing part is sent | F18 |
| 26 | `kermit -r` (or `gkermit -r`) on the board, send with Kermit; then the other way | Both directions arrive byte-identical | F19 |
| 27 | Drop a file on the window, choose Send | The transfer dialog opens with the file; sending works as in 20–26 | F19 |
| 28 | `portside send <port> image.bin -P xmodem` and `portside receive <port> -P zmodem --into /tmp` from a headless box | Progress on stderr; exit code 0; `Ctrl-C` cancels cleanly and the device stops waiting | F23 |

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
  like on a PTY. A **native** break cannot be tested without hardware; only step 10 covers
  it.
- No real driver is exercised, so driver-specific quirks (CH34x clones, counterfeit FTDI,
  PL2303 revisions) only show up in this checklist.
- Timing on a PTY is not serial timing: pacing is verified for *ordering and waiting*, not for
  what a 9600-baud console does to a 400-line paste.
- A PTY has no baud rate, so CI's transfers run at memory speed. Timeouts on a slow link,
  hardware flow control and a bootloader's small receive buffer are only covered by steps
  20–28.
