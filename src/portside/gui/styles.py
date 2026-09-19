"""Application stylesheets for the three themes (PRD F8, accessibility).

Kept as plain Qt style sheets rather than a theming framework: three palettes
is not enough to justify a dependency, and this way the terminal palette and
the chrome are defined side by side and cannot drift apart.
"""

from __future__ import annotations

from portside.gui.palette import THEMES, ThemeName

__all__ = ["stylesheet"]

_TEMPLATE = """
QWidget {{
    background-color: {window};
    color: {text};
}}
QTreeWidget, QListWidget, QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit {{
    background-color: {field};
    color: {text};
    border: 1px solid {border};
    selection-background-color: {selection};
    selection-color: {text};
}}
QTreeWidget::item:selected, QListWidget::item:selected {{
    background-color: {selection};
    color: {text};
}}
QHeaderView::section {{
    background-color: {panel};
    color: {text};
    border: none;
    padding: 4px;
}}
QTabBar::tab {{
    background: {panel};
    color: {text};
    padding: 6px 12px;
    border: 1px solid {border};
}}
QTabBar::tab:selected {{
    background: {field};
    border-bottom: 2px solid {accent};
}}
QPushButton {{
    background-color: {panel};
    border: 1px solid {border};
    padding: 5px 12px;
}}
QPushButton:hover {{ border-color: {accent}; }}
QPushButton:disabled {{ color: {disabled}; }}
QToolBar {{ background: {panel}; border: none; spacing: 4px; }}
QStatusBar {{ background: {panel}; }}
QProgressBar {{ border: 1px solid {border}; text-align: center; }}
QProgressBar::chunk {{ background-color: {accent}; }}
"""

_CHROME = {
    ThemeName.DARK: {
        "window": "#1b1f24",
        "panel": "#22272e",
        "field": "#14171c",
        "text": "#d8dee9",
        "border": "#333a43",
        "accent": "#7fd1b9",
        "selection": "#2f4f73",
        "disabled": "#6b7280",
    },
    ThemeName.LIGHT: {
        "window": "#f4f5f7",
        "panel": "#e8eaed",
        "field": "#ffffff",
        "text": "#1f2329",
        "border": "#c6cad1",
        "accent": "#0a6b57",
        "selection": "#bcd8f7",
        "disabled": "#9aa0a6",
    },
    # Pure black and white with a saturated accent: every pair clears WCAG AAA.
    ThemeName.HIGH_CONTRAST: {
        "window": "#000000",
        "panel": "#000000",
        "field": "#000000",
        "text": "#ffffff",
        "border": "#ffffff",
        "accent": "#ffff00",
        "selection": "#0000aa",
        "disabled": "#8a8a8a",
    },
}


def stylesheet(name: ThemeName | str) -> str:
    """The Qt style sheet for a theme."""
    try:
        theme_name = ThemeName(name)
    except ValueError:
        theme_name = ThemeName.DARK
    assert theme_name in THEMES
    return _TEMPLATE.format(**_CHROME[theme_name])
