"""Portside — a cross-platform serial terminal.

The package is split into a UI-independent core (:mod:`portside.core`,
:mod:`portside.transport`) and thin front ends (:mod:`portside.cli`,
:mod:`portside.tui`, :mod:`portside.gui`).  Only :mod:`portside.platform` and
:mod:`portside.transport` are allowed to contain operating-system specific code.
"""

__all__ = ["__version__"]

__version__ = "0.1.0"
