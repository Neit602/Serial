#include "gui/styles.h"

#include <QHash>

namespace portside::gui {

namespace {

const char *const kTemplate = R"(
QWidget {
    background-color: @window;
    color: @text;
}
QTreeWidget, QListWidget, QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit {
    background-color: @field;
    color: @text;
    border: 1px solid @border;
    selection-background-color: @selection;
    selection-color: @text;
}
QTreeWidget::item:selected, QListWidget::item:selected {
    background-color: @selection;
    color: @text;
}
QHeaderView::section {
    background-color: @panel;
    color: @text;
    border: none;
    padding: 4px;
}
QTabBar::tab {
    background: @panel;
    color: @text;
    padding: 6px 12px;
    border: 1px solid @border;
}
QTabBar::tab:selected {
    background: @field;
    border-bottom: 2px solid @accent;
}
QPushButton {
    background-color: @panel;
    border: 1px solid @border;
    padding: 5px 12px;
}
QPushButton:hover { border-color: @accent; }
QPushButton:disabled { color: @disabled; }
QToolBar { background: @panel; border: none; spacing: 4px; }
QStatusBar { background: @panel; }
QProgressBar { border: 1px solid @border; text-align: center; }
QProgressBar::chunk { background-color: @accent; }
)";

struct Chrome
{
    const char *window;
    const char *panel;
    const char *field;
    const char *text;
    const char *border;
    const char *accent;
    const char *selection;
    const char *disabled;
};

Chrome chromeFor(ThemeName name)
{
    switch (name) {
    case ThemeName::Light:
        return {"#f4f5f7", "#e8eaed", "#ffffff", "#1f2329", "#c6cad1", "#0a6b57", "#bcd8f7", "#9aa0a6"};
    case ThemeName::HighContrast:
        // Pure black and white with a saturated accent: every pair clears
        // WCAG AAA.
        return {"#000000", "#000000", "#000000", "#ffffff", "#ffffff", "#ffff00", "#0000aa", "#8a8a8a"};
    case ThemeName::Dark:
        break;
    }
    return {"#1b1f24", "#22272e", "#14171c", "#d8dee9", "#333a43", "#7fd1b9", "#2f4f73", "#6b7280"};
}

} // namespace

QString stylesheet(ThemeName name)
{
    const Chrome chrome = chromeFor(name);
    QString sheet = QString::fromLatin1(kTemplate);
    // Longest names first, so @text never clobbers a longer token.
    const std::pair<const char *, const char *> tokens[] = {
        {"@selection", chrome.selection}, {"@disabled", chrome.disabled},
        {"@window", chrome.window},       {"@border", chrome.border},
        {"@accent", chrome.accent},       {"@panel", chrome.panel},
        {"@field", chrome.field},         {"@text", chrome.text},
    };
    for (const auto &[token, colour] : tokens)
        sheet.replace(QLatin1String(token), QLatin1String(colour));
    return sheet;
}

} // namespace portside::gui
