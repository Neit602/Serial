"""PySide6 desktop front end (PRD M3).

Importing this package does **not** import Qt: the CLI and TUI must keep
working on a machine without PySide6 installed.  Import the modules inside it
(or call :func:`portside.gui.main.main`) when Qt is actually needed.
"""

__all__ = ["main"]


def main(argv: list[str] | None = None) -> int:
    """Start the desktop app (see :mod:`portside.gui.main`)."""
    from portside.gui.main import main as _main

    return _main(argv)
