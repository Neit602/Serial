"""Fallback backend for POSIX systems that are neither Linux nor macOS.

Keeps Portside usable on the BSDs without pretending to know their sysfs
equivalents.
"""

from __future__ import annotations

from portside.platform.base import PlatformSupport

__all__ = ["GenericPosixPlatform"]


class GenericPosixPlatform(PlatformSupport):
    """Generic POSIX diagnostics."""

    name = "posix"

    def permission_remedy(self, port: str) -> str:
        return (
            f"You do not have permission to open {port}.\n"
            "Serial device nodes are usually owned by a group such as 'dialout' or "
            "'uucp'; add your user to the group that owns the device (check with "
            f"'ls -l {port}') and start a new login session."
        )

    def busy_remedy(self, port: str) -> str:
        return (
            f"{port} is open in another program.\n"
            f"Find it with 'fuser -v {port}' or 'lsof {port}', close it, and try again."
        )
