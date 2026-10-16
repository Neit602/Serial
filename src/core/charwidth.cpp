#include "core/charwidth.h"

#include <algorithm>
#include <array>
#include <iterator>

namespace portside {

namespace {

struct Range
{
    char32_t first;
    char32_t last;
};

constexpr std::array<Range, 18> kZeroWidth{{
    {0x0300, 0x036F}, // combining diacritical marks
    {0x0483, 0x0489},
    {0x0591, 0x05BD},
    {0x0610, 0x061A},
    {0x064B, 0x065F},
    {0x0E31, 0x0E31},
    {0x0E34, 0x0E3A},
    {0x0E47, 0x0E4E},
    {0x1AB0, 0x1AFF},
    {0x1DC0, 0x1DFF},
    {0x200B, 0x200F}, // zero-width space, joiners, direction marks
    {0x202A, 0x202E},
    {0x2060, 0x2064},
    {0x20D0, 0x20FF},
    {0xFE00, 0xFE0F}, // variation selectors
    {0xFE20, 0xFE2F},
    {0xFEFF, 0xFEFF},
    {0xE0100, 0xE01EF},
}};

constexpr std::array<Range, 17> kWide{{
    {0x1100, 0x115F}, // Hangul Jamo
    {0x231A, 0x231B},
    {0x2329, 0x232A},
    {0x23E9, 0x23EC},
    {0x25FD, 0x25FE},
    {0x2614, 0x2615},
    {0x2E80, 0x303E}, // CJK radicals .. CJK symbols
    {0x3041, 0x33FF},
    {0x3400, 0x4DBF},
    {0x4E00, 0x9FFF}, // CJK unified ideographs
    {0xA000, 0xA4CF},
    {0xAC00, 0xD7A3}, // Hangul syllables
    {0xF900, 0xFAFF},
    {0xFE30, 0xFE4F},
    {0xFF00, 0xFF60}, // fullwidth forms
    {0xFFE0, 0xFFE6},
    {0x1F300, 0x1FAFF}, // emoji and pictographs
}};

template <std::size_t N>
bool inRanges(const std::array<Range, N> &ranges, char32_t c)
{
    const auto it = std::upper_bound(ranges.begin(), ranges.end(), c,
                                     [](char32_t value, const Range &r) { return value < r.first; });
    if (it == ranges.begin())
        return false;
    return c <= std::prev(it)->last;
}

} // namespace

int charWidth(char32_t c)
{
    if (c < 0x300)
        return 1;
    if (inRanges(kZeroWidth, c))
        return 0;
    if (inRanges(kWide, c) || (c >= 0x20000 && c <= 0x3FFFD))
        return 2;
    return 1;
}

} // namespace portside
