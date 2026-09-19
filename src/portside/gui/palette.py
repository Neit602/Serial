"""Colour palettes for the terminal widget and the app chrome (PRD F8).

Three themes, including a high-contrast one for the accessibility requirement.
The terminal's ANSI colours come from the same place as the chrome, so a theme
switch changes both together.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import StrEnum

from PySide6.QtGui import QColor

__all__ = ["THEMES", "TerminalPalette", "ThemeName", "theme"]


class ThemeName(StrEnum):
    """Available themes (F8, plus the PRD's high-contrast requirement)."""

    DARK = "dark"
    LIGHT = "light"
    HIGH_CONTRAST = "high-contrast"


@dataclass(frozen=True, slots=True)
class TerminalPalette:
    """Colours for one theme."""

    name: ThemeName
    background: str
    foreground: str
    cursor: str
    selection: str
    match: str
    #: The eight ANSI colours and their bright variants, by pyte's names.
    ansi: dict[str, str] = field(default_factory=dict)

    def colour(self, name: str, *, default: str) -> QColor:
        """Resolve a pyte colour name to a Qt colour."""
        if name in {"default", ""}:
            return QColor(default)
        if name in self.ansi:
            return QColor(self.ansi[name])
        if len(name) == 6:
            try:
                int(name, 16)
            except ValueError:
                return QColor(default)
            return QColor(f"#{name}")
        return QColor(default)

    def fg(self, name: str) -> QColor:
        """Foreground colour for a cell."""
        return self.colour(name, default=self.foreground)

    def bg(self, name: str) -> QColor:
        """Background colour for a cell."""
        return self.colour(name, default=self.background)


def _ansi(
    black: str,
    red: str,
    green: str,
    brown: str,
    blue: str,
    magenta: str,
    cyan: str,
    white: str,
    bright: dict[str, str],
) -> dict[str, str]:
    base = {
        "black": black,
        "red": red,
        "green": green,
        "brown": brown,
        "blue": blue,
        "magenta": magenta,
        "cyan": cyan,
        "white": white,
    }
    return {**base, **bright}


DARK = TerminalPalette(
    name=ThemeName.DARK,
    background="#14171c",
    foreground="#d8dee9",
    cursor="#7fd1b9",
    selection="#2f4f73",
    match="#8a6d1f",
    ansi=_ansi(
        "#3b4048",
        "#e06c75",
        "#98c379",
        "#d19a66",
        "#61afef",
        "#c678dd",
        "#56b6c2",
        "#c8ccd4",
        {
            "brightblack": "#5c6370",
            "brightred": "#ff7b86",
            "brightgreen": "#b6e3a1",
            "brightbrown": "#e5c07b",
            "brightblue": "#82c7ff",
            "brightmagenta": "#e39ef7",
            "brightcyan": "#7fe0e8",
            "brightwhite": "#ffffff",
        },
    ),
)

LIGHT = TerminalPalette(
    name=ThemeName.LIGHT,
    background="#fdfdfd",
    foreground="#22262b",
    cursor="#0a6b57",
    selection="#bcd8f7",
    match="#f5dd87",
    ansi=_ansi(
        "#22262b",
        "#c02b35",
        "#3f7a2e",
        "#a06000",
        "#1a5fb4",
        "#8b2fa8",
        "#0f6f78",
        "#6b7280",
        {
            "brightblack": "#4b5563",
            "brightred": "#e0404b",
            "brightgreen": "#4f9a3a",
            "brightbrown": "#b97a00",
            "brightblue": "#2a76d2",
            "brightmagenta": "#a848c4",
            "brightcyan": "#148a94",
            "brightwhite": "#111318",
        },
    ),
)

#: Pure black on white with saturated accents: every pair here clears WCAG AAA
#: against the background, which is what the accessibility requirement needs.
HIGH_CONTRAST = TerminalPalette(
    name=ThemeName.HIGH_CONTRAST,
    background="#000000",
    foreground="#ffffff",
    cursor="#ffff00",
    selection="#0000aa",
    match="#ffff00",
    ansi=_ansi(
        "#000000",
        "#ff5f5f",
        "#5fff5f",
        "#ffff5f",
        "#5f9fff",
        "#ff5fff",
        "#5fffff",
        "#ffffff",
        {
            "brightblack": "#8a8a8a",
            "brightred": "#ff8787",
            "brightgreen": "#87ff87",
            "brightbrown": "#ffff87",
            "brightblue": "#87bfff",
            "brightmagenta": "#ff87ff",
            "brightcyan": "#87ffff",
            "brightwhite": "#ffffff",
        },
    ),
)

THEMES: dict[ThemeName, TerminalPalette] = {
    ThemeName.DARK: DARK,
    ThemeName.LIGHT: LIGHT,
    ThemeName.HIGH_CONTRAST: HIGH_CONTRAST,
}


def theme(name: ThemeName | str) -> TerminalPalette:
    """Look up a palette by name, falling back to dark."""
    try:
        return THEMES[ThemeName(name)]
    except ValueError:
        return THEMES[ThemeName.DARK]
