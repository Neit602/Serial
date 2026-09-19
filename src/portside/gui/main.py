"""``portside-gui`` entry point.

Kept small and import-light: the PRD asks for a cold start under two seconds,
and the heavy Qt imports only happen once we know Qt is actually installed.
"""

from __future__ import annotations

import sys
from collections.abc import Sequence

__all__ = ["main"]

_MISSING_QT = """portside-gui needs PySide6, which is not installed.

Install it with:

    pip install "portside[gui]"

The command line and the terminal UI work without it:

    portside --list
    portside connect <port> --baud 115200
"""


def main(argv: Sequence[str] | None = None) -> int:
    """Start the desktop application.  Returns the process exit code."""
    try:
        from PySide6.QtCore import QCoreApplication
        from PySide6.QtWidgets import QApplication
    except ImportError:
        print(_MISSING_QT, file=sys.stderr)
        return 1

    from portside import __version__
    from portside.gui.main_window import APPLICATION, ORGANISATION, MainWindow

    arguments = list(sys.argv if argv is None else [sys.argv[0], *argv])
    QCoreApplication.setOrganizationName(ORGANISATION)
    QCoreApplication.setApplicationName(APPLICATION)
    QCoreApplication.setApplicationVersion(__version__)

    app = QApplication(arguments)
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":  # pragma: no cover - module entry point
    raise SystemExit(main())
