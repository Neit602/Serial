#pragma once

// Application stylesheets for the three themes (PRD F8, accessibility).
//
// Plain Qt style sheets rather than a theming framework: three palettes is not
// enough to justify a dependency, and this way the terminal palette and the
// chrome are defined side by side and cannot drift apart.

#include "gui/palette.h"

#include <QString>

namespace portside::gui {

QString stylesheet(ThemeName name);

} // namespace portside::gui
