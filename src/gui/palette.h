#pragma once

// Colour palettes for the terminal widget and the app chrome (PRD F8).
//
// Three themes, including a high-contrast one for the accessibility
// requirement.  The terminal's ANSI colours come from the same place as the
// chrome, so a theme switch changes both together.

#include "core/enums.h"
#include "core/terminal.h"

#include <QColor>
#include <QString>

#include <array>

namespace portside::gui {

enum class ThemeName { Dark, Light, HighContrast };

} // namespace portside::gui

namespace portside {
template <>
struct EnumNames<gui::ThemeName>
{
    static constexpr std::array<std::pair<gui::ThemeName, std::string_view>, 3> table{{
        {gui::ThemeName::Dark, "dark"},
        {gui::ThemeName::Light, "light"},
        {gui::ThemeName::HighContrast, "high-contrast"},
    }};
};
} // namespace portside

namespace portside::gui {

struct TerminalPalette
{
    ThemeName name = ThemeName::Dark;
    QColor background;
    QColor foreground;
    QColor cursor;
    QColor selection;
    QColor match;
    // The eight ANSI colours, then their bright variants.
    std::array<QColor, 16> ansi;

    // Resolve a cell colour; `isBackground` picks which default applies.
    QColor resolve(const Color &colour, bool isBackground) const;
    QColor fg(const Color &colour) const { return resolve(colour, false); }
    QColor bg(const Color &colour) const { return resolve(colour, true); }
};

// Look up a palette by theme.
const TerminalPalette &theme(ThemeName name);
// By name, falling back to dark for anything unknown.
ThemeName themeFromName(const QString &name);

// WCAG 2 contrast ratio between two colours (1 to 21).
double contrastRatio(const QColor &a, const QColor &b);

} // namespace portside::gui
