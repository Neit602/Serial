"""Platform layer: the only place (with :mod:`portside.transport`) that may
contain operating-system specific code.

Use :func:`get_platform` to obtain the backend for the current OS; tests use
:func:`platform_for` to exercise all three backends on any runner.
"""

from __future__ import annotations

import sys
from functools import lru_cache

from portside.platform.base import PlatformSupport

__all__ = ["PlatformSupport", "get_platform", "platform_for", "platform_name"]


def platform_name(sys_platform: str | None = None) -> str:
    """Map a ``sys.platform`` string to a Portside platform name."""
    raw = sys_platform if sys_platform is not None else sys.platform
    if raw.startswith("linux"):
        return "linux"
    if raw == "darwin":
        return "macos"
    if raw in {"win32", "cygwin"}:
        return "windows"
    # BSDs and anything else behave close enough to Linux minus sysfs.
    return "posix"


def platform_for(name: str) -> PlatformSupport:
    """Construct the backend called ``name`` (``linux``/``macos``/``windows``)."""
    if name == "linux":
        from portside.platform.linux import LinuxPlatform

        return LinuxPlatform()
    if name == "macos":
        from portside.platform.macos import MacOSPlatform

        return MacOSPlatform()
    if name == "windows":
        from portside.platform.windows import WindowsPlatform

        return WindowsPlatform()
    if name == "posix":
        from portside.platform.posix import GenericPosixPlatform

        return GenericPosixPlatform()
    raise ValueError(f"unknown platform {name!r}")


@lru_cache(maxsize=1)
def get_platform() -> PlatformSupport:
    """The backend for the OS Portside is running on."""
    return platform_for(platform_name())
