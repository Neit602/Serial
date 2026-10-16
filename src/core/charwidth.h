#pragma once

// How many terminal cells a character occupies: 0 for combining marks and
// other zero-width characters, 2 for East Asian wide and emoji, 1 otherwise.
// A compact approximation of wcwidth(), sufficient for a serial console.

namespace portside {

int charWidth(char32_t c);

} // namespace portside
