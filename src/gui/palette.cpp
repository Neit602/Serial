#include "gui/palette.h"

#include <algorithm>
#include <cmath>

namespace portside::gui {

namespace {

TerminalPalette make(ThemeName name, const char *background, const char *foreground,
                     const char *cursor, const char *selection, const char *match,
                     const std::array<const char *, 16> &ansi)
{
    TerminalPalette palette;
    palette.name = name;
    palette.background = QColor(QLatin1String(background));
    palette.foreground = QColor(QLatin1String(foreground));
    palette.cursor = QColor(QLatin1String(cursor));
    palette.selection = QColor(QLatin1String(selection));
    palette.match = QColor(QLatin1String(match));
    for (std::size_t i = 0; i < ansi.size(); ++i)
        palette.ansi[i] = QColor(QLatin1String(ansi[i]));
    return palette;
}

const TerminalPalette kDark = make(
    ThemeName::Dark, "#14171c", "#d8dee9", "#7fd1b9", "#2f4f73", "#8a6d1f",
    {"#3b4048", "#e06c75", "#98c379", "#d19a66", "#61afef", "#c678dd", "#56b6c2", "#c8ccd4",
     "#5c6370", "#ff7b86", "#b6e3a1", "#e5c07b", "#82c7ff", "#e39ef7", "#7fe0e8", "#ffffff"});

const TerminalPalette kLight = make(
    ThemeName::Light, "#fdfdfd", "#22262b", "#0a6b57", "#bcd8f7", "#f5dd87",
    {"#22262b", "#c02b35", "#3f7a2e", "#a06000", "#1a5fb4", "#8b2fa8", "#0f6f78", "#6b7280",
     "#4b5563", "#e0404b", "#4f9a3a", "#b97a00", "#2a76d2", "#a848c4", "#148a94", "#111318"});

// Pure black and white with saturated accents: every pair here clears WCAG
// AAA against the background, which is what the accessibility requirement
// needs.
const TerminalPalette kHighContrast = make(
    ThemeName::HighContrast, "#000000", "#ffffff", "#ffff00", "#0000aa", "#ffff00",
    {"#000000", "#ff5f5f", "#5fff5f", "#ffff5f", "#5f9fff", "#ff5fff", "#5fffff", "#ffffff",
     "#8a8a8a", "#ff8787", "#87ff87", "#ffff87", "#87bfff", "#ff87ff", "#87ffff", "#ffffff"});

// xterm's 256-colour table above the 16 a theme controls.
QColor xterm256(int index)
{
    if (index >= 232) {
        const int level = 8 + (index - 232) * 10;
        return QColor(level, level, level);
    }
    const int cube = index - 16;
    const auto component = [](int value) { return value == 0 ? 0 : 55 + value * 40; };
    return QColor(component(cube / 36), component((cube / 6) % 6), component(cube % 6));
}

double luminance(const QColor &colour)
{
    const auto channel = [](double value) {
        value /= 255.0;
        return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(colour.red()) + 0.7152 * channel(colour.green())
           + 0.0722 * channel(colour.blue());
}

} // namespace

QColor TerminalPalette::resolve(const Color &colour, bool isBackground) const
{
    if (colour.isDefault())
        return isBackground ? background : foreground;
    if (colour.isIndexed()) {
        const int index = colour.index();
        return index < 16 ? ansi[std::size_t(index)] : xterm256(index);
    }
    return QColor::fromRgb(colour.rgbValue());
}

const TerminalPalette &theme(ThemeName name)
{
    switch (name) {
    case ThemeName::Light:
        return kLight;
    case ThemeName::HighContrast:
        return kHighContrast;
    case ThemeName::Dark:
        break;
    }
    return kDark;
}

ThemeName themeFromName(const QString &name)
{
    return enumFromName<ThemeName>(name).value_or(ThemeName::Dark);
}

double contrastRatio(const QColor &a, const QColor &b)
{
    const double first = luminance(a);
    const double second = luminance(b);
    const double lighter = std::max(first, second);
    const double darker = std::min(first, second);
    return (lighter + 0.05) / (darker + 0.05);
}

} // namespace portside::gui
