"""Windows platform backend.

pySerial already returns COM names in the form Windows expects, including the
``\\\\.\\COM10`` escaping needed above COM9, so discovery needs no filtering
here.  What Windows users do need is a sharp explanation of error 5 (access
denied, almost always another program) and of missing vendor drivers.
"""

from __future__ import annotations

from portside.platform.base import PlatformSupport

__all__ = ["WindowsPlatform"]


class WindowsPlatform(PlatformSupport):
    """Port discovery and diagnostics for Windows."""

    name = "windows"

    def permission_remedy(self, port: str) -> str:
        return (
            f"Windows denied access to {port} (error 5).\n"
            "On Windows this almost always means another program already has the port "
            "open — PuTTY, Tera Term, Arduino IDE, a vendor tool, or another Portside "
            "session.  Close it and try again.\n"
            "If nothing holds the port, open Device Manager and check the adapter under "
            "'Ports (COM & LPT)' for a warning triangle; a driver marked with one needs "
            "reinstalling (Windows Update supplies FTDI, CP210x and PL2303; CH34x "
            "usually needs the vendor driver)."
        )

    def busy_remedy(self, port: str) -> str:
        return (
            f"{port} is already open in another program.\n"
            "Close the other terminal, IDE serial monitor or vendor tool, then try again.\n"
            "If the port was left behind by a crashed program, unplug and re-plug the "
            "adapter to force the driver to release it."
        )

    def not_found_remedy(self, port: str) -> str:
        return (
            f"No serial port named {port!r} was found.\n"
            "Run 'portside --list' to see the current ports, and check Device Manager "
            "under 'Ports (COM & LPT)'.  Note that ports above COM9 must be written as "
            "COM10, not \\\\.\\COM10, on the Portside command line."
        )
