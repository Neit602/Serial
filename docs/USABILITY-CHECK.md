# Usability check (M3 exit criterion)

The PRD's exit criterion for M3 is:

> Usability pass: new user connects in under 30 s.

That is a measurement of a *person*, not of code, so CI cannot assert it. What CI does assert
is the shape of the path (`tests/gui/tst_main_window.cpp`,
`aNewUserReachesAConnectedSessionInTwoActions`): the device is already listed when the window
opens, and reaching a connected terminal takes selecting it and activating it — no typing and
no dialogs. If anyone adds a step, that test fails.

This document is the other half: run it once per release, on **Linux, macOS and Windows**,
with a person who has not used Portside before.

## Setup

- A USB-serial adapter and a device that prints something when you press Enter (a switch, a
  dev board, or a second machine).
- Portside installed as a user would install it (`cmake --install`, or the packaged app once
  M5 exists). **Not** run from a build directory.
- A stopwatch.
- No profile files: delete or move `~/.config/Portside`, `~/Library/Application Support/Portside`
  or `%APPDATA%\Portside` first. The criterion is about a *new* user.

## The test

Say exactly this and nothing more:

> "This is a serial terminal. Connect to the device plugged into this machine and show me its
> output. I'll time you, but take as long as you need — I'm testing the app, not you."

Then start the stopwatch when they launch the app and stop it when device output is visible
on screen. **Do not help.** Note where they hesitate; that is the useful part.

## What to record

| # | Question | Pass |
| --- | --- | --- |
| 1 | Time from launch to visible device output | Under 30 s |
| 2 | Did they find the device list without being told? | Yes |
| 3 | Did they need the settings dialog to connect? | No — the default (115200 8N1) should just work, or the profile should apply itself |
| 4 | Did they try the wrong port first? If so, was it obvious why nothing happened? | An error, not silence |
| 5 | Did they understand the status bar (port, framing, byte counters)? | Asked no questions about it |
| 6 | On a permission error (Linux, user not in `dialout`/`uucp`): did the dialog tell them what to do? | They could fix it by pasting the command |
| 7 | Could they find how to quit, or close a session, unaided? | Yes |

Record the time and the hesitations per OS in the release notes. A fail on #1 is a fail of the
milestone; a fail on any other row is a bug worth filing.

## Deliberate things to watch for

- **The device list is the front door.** If the tester goes to the menus first, the list is not
  prominent enough.
- **Nothing should need typing.** If they type a port name or a baud rate to get connected, the
  defaults or the auto-applied profile are wrong.
- **Errors are the whole of F6.** A refused port must print the remedy, and on Linux the
  remedy must name the group that actually owns the device node (`dialout` on Debian/Ubuntu,
  `uucp` on Arch), with a "Copy fix" button that puts the command on the clipboard.

## Known limits of the automated half

- It counts *actions*, not seconds of human thought: a two-click path can still be confusing.
- It runs headless (`QT_QPA_PLATFORM=offscreen`), so it says nothing about fonts, DPI scaling,
  window decorations or how the app looks on a Retina display or a 4K Windows laptop.
- It uses a loopback transport, so no driver, no latency and no adapter quirks are involved.
  Those are covered by [HARDWARE-CHECK.md](HARDWARE-CHECK.md).
