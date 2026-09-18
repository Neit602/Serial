"""Typed error hierarchy (PRD F6).

Every error a front end may have to display carries a short human readable
``message`` and an optional ``remedy``: a platform-specific, copy-pasteable fix
produced by :mod:`portside.platform`.  UIs are expected to render the remedy
verbatim; they must never have to pattern-match on driver error strings.
"""

from __future__ import annotations

__all__ = [
    "DeviceVanishedError",
    "InvalidSettingsError",
    "PermissionDeniedError",
    "PortBusyError",
    "PortNotFoundError",
    "PortsideError",
    "SessionError",
    "TransportClosedError",
    "TransportError",
]


class PortsideError(Exception):
    """Base class for every error Portside raises deliberately."""

    def __init__(self, message: str, *, port: str | None = None, remedy: str | None = None) -> None:
        super().__init__(message)
        self.message = message
        self.port = port
        self.remedy = remedy

    def __str__(self) -> str:
        if self.port:
            return f"{self.port}: {self.message}"
        return self.message

    def display(self) -> str:
        """Message plus remedy, formatted for a terminal or a dialog box."""
        if self.remedy:
            return f"{self}\n\n{self.remedy}"
        return str(self)


class InvalidSettingsError(PortsideError):
    """A port setting is outside the range the PRD allows (F3)."""


class TransportError(PortsideError):
    """Base class for failures of the transport layer."""


class PortNotFoundError(TransportError):
    """The named port does not exist (wrong name, or never plugged in)."""


class PortBusyError(TransportError):
    """The port exists but is held open by another process."""


class PermissionDeniedError(TransportError):
    """The port exists but the current user may not open it."""


class DeviceVanishedError(TransportError):
    """The device disappeared while it was open (unplugged, or driver reset)."""


class TransportClosedError(TransportError):
    """An I/O operation was attempted on a transport that is not open."""


class SessionError(PortsideError):
    """The session was asked to do something its current state does not allow."""
