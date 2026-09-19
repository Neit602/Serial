"""Non-functional requirements the GUI has to meet (memory, i18n, a11y)."""

from __future__ import annotations

import ast
import gc
import re
from collections.abc import Iterator
from pathlib import Path

import pytest
from PySide6.QtCore import QSettings
from pytestqt.qtbot import QtBot

import portside.gui
from portside.core.session import Session
from portside.gui.controller import SessionController
from portside.gui.runtime import AsyncRuntime
from portside.gui.session_view import SessionView
from portside.transport.loopback import LoopbackTransport

pytestmark = pytest.mark.gui

GUI_PACKAGE = Path(portside.gui.__file__).parent

#: Qt setters whose argument is shown to the user and therefore needs tr().
_USER_FACING_SETTERS = {
    "setWindowTitle",
    "setPlaceholderText",
    "setStatusTip",
    "setWhatsThis",
}

#: Literals that are deliberately not translated: the product name, and a
#: regular expression shown as an example of what to type.
_NOT_PROSE = {"Portside", r"[>#]\s*$"}


def _string_literals_passed_to(tree: ast.AST, names: set[str]) -> Iterator[tuple[str, int]]:
    """Yield (value, line) for string literals passed to the named setters."""
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Attribute):
            continue
        if node.func.attr not in names:
            continue
        for argument in node.args:
            if (
                isinstance(argument, ast.Constant)
                and isinstance(argument.value, str)
                and argument.value.strip()
                and argument.value not in _NOT_PROSE
            ):
                yield argument.value, argument.lineno


def test_user_facing_strings_go_through_tr() -> None:
    """NFR i18n: UI strings must be externalisable for translation."""
    offenders: list[str] = []
    for path in sorted(GUI_PACKAGE.glob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"))
        for value, line in _string_literals_passed_to(tree, _USER_FACING_SETTERS):
            offenders.append(f"{path.name}:{line}: {value!r}")
    assert not offenders, "user-visible strings not wrapped in tr():\n" + "\n".join(offenders)


def test_importing_portside_does_not_pull_in_qt() -> None:
    """The CLI and TUI must work on a machine without PySide6 installed.

    Checked in a fresh interpreter, because Qt is already loaded in this one.
    """
    import subprocess
    import sys

    probe = (
        "import sys; import portside.gui, portside.cli.main; "
        "print('qt' if any(m.startswith('PySide6') for m in sys.modules) else 'clean')"
    )
    result = subprocess.run(
        [sys.executable, "-c", probe], capture_output=True, text=True, timeout=120, check=True
    )
    assert result.stdout.strip() == "clean", "importing portside must not import Qt"


def test_the_entry_point_explains_a_missing_qt() -> None:
    from portside.gui.main import _MISSING_QT

    assert "pip install" in _MISSING_QT
    assert "portside connect" in _MISSING_QT, "say what still works without Qt"


def _current_rss_mb() -> float | None:
    """Resident set size right now, in MB, or None if it cannot be read.

    ``resource.getrusage`` reports the process *peak*, which never falls and so
    depends on whatever ran earlier in the session; that is useless for
    measuring one feature.  Linux exposes the current value, which is what this
    test needs.
    """
    try:
        statm = Path("/proc/self/statm").read_text(encoding="ascii").split()
    except OSError:
        return None
    import os

    return int(statm[1]) * os.sysconf("SC_PAGE_SIZE") / (1024 * 1024)


@pytest.mark.slow
def test_four_sessions_with_full_scrollback_stay_within_budget(
    qtbot: QtBot, tmp_path: Path
) -> None:
    """NFR: under 200 MB with 4 sessions and 10,000-line scrollback each.

    The budget is for the whole app, but the part a test can measure reliably
    is the growth these four sessions cause, so that is what is asserted — the
    baseline is whatever the interpreter and Qt already hold.
    """
    baseline = _current_rss_mb()
    if baseline is None:
        pytest.skip("current RSS is only readable from /proc on Linux")

    runtime = AsyncRuntime()
    runtime.start()
    views: list[SessionView] = []
    try:
        for index in range(4):
            transport = LoopbackTransport(f"/dev/fake{index}", echo=False)
            session = Session(transport)
            runtime.run(session.open())
            view = SessionView(SessionController(session, runtime))
            qtbot.addWidget(view)
            views.append(view)
            # Fill the scrollback the way a boot log would.
            for line in range(10_000):
                view.view.feed(f"line {line} of session {index}\r\n".encode())
        gc.collect()
        after = _current_rss_mb()
        assert after is not None
        growth = after - baseline
        assert growth < 200, (
            f"four sessions with full scrollback added {growth:.0f} MB "
            f"({baseline:.0f} MB -> {after:.0f} MB)"
        )
    finally:
        for view in views:
            view.close_session()
        runtime.stop()


def test_keyboard_shortcuts_are_unique(qtbot: QtBot, tmp_path: Path) -> None:
    """Two actions sharing a shortcut means one of them silently never fires."""
    from PySide6.QtGui import QAction

    from portside.gui.main_window import MainWindow
    from portside.profiles.store import ProfileStore

    window = MainWindow(
        store=ProfileStore(tmp_path / "profiles"),
        lister=lambda: [],
        settings=QSettings(str(tmp_path / "s.ini"), QSettings.Format.IniFormat),
    )
    qtbot.addWidget(window)
    try:
        seen: dict[str, str] = {}
        duplicates: list[str] = []
        for action in window.findChildren(QAction):
            shortcut = action.shortcut().toString()
            if not shortcut:
                continue
            if shortcut in seen and seen[shortcut] != action.iconText():
                duplicates.append(f"{shortcut}: {seen[shortcut]} and {action.iconText()}")
            seen[shortcut] = action.iconText()
        assert not duplicates, "duplicate shortcuts: " + ", ".join(duplicates)
    finally:
        window.close()


def test_high_contrast_theme_meets_a_contrast_ratio() -> None:
    """NFR accessibility: the high-contrast theme must actually be high contrast."""
    from portside.gui.palette import THEMES, ThemeName

    palette = THEMES[ThemeName.HIGH_CONTRAST]

    def luminance(colour: str) -> float:
        value = colour.lstrip("#")
        channels = [int(value[i : i + 2], 16) / 255 for i in (0, 2, 4)]
        adjusted = [
            channel / 12.92 if channel <= 0.03928 else ((channel + 0.055) / 1.055) ** 2.4
            for channel in channels
        ]
        return 0.2126 * adjusted[0] + 0.7152 * adjusted[1] + 0.0722 * adjusted[2]

    def ratio(first: str, second: str) -> float:
        light, dark = sorted((luminance(first), luminance(second)), reverse=True)
        return (light + 0.05) / (dark + 0.05)

    assert ratio(palette.foreground, palette.background) >= 7.0, "body text must clear WCAG AAA"
    for name, colour in palette.ansi.items():
        if re.fullmatch(r"#[0-9a-f]{6}", colour) and name != "black":
            assert ratio(colour, palette.background) >= 4.5, f"{name} is too dim to read"
