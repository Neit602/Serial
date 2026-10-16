#pragma once

// Searching the screen and scrollback (PRD F8).
//
// Literal search by default: an engineer looking for Gi1/0/1 or 10.0.0.1
// should not have to escape anything.  Regex is opt-in (DECISIONS D27).

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace portside {

struct SearchOptions
{
    bool caseSensitive = false;
    bool regex = false;
    bool wholeWord = false;
};

// One hit: a line index (0 = oldest kept line) and a [start, end) range in
// that line's text, in UTF-16 units.
struct Match
{
    int line = 0;
    int start = 0;
    int end = 0;

    int length() const { return end - start; }
    bool operator==(const Match &) const = default;
};

// Every match, oldest line first, left to right.  Matches are found per line:
// the emulator has already wrapped long output into lines, so a hit that spans
// a wrap is two matches, as it looks on screen.  Throws InvalidSettings for a
// bad regular expression.
QList<Match> findAll(const QStringList &lines, const QString &query,
                     const SearchOptions &options = {});

// The match after/before `current`, wrapping around.
std::optional<Match> findNext(const QList<Match> &matches, const std::optional<Match> &current);
std::optional<Match> findPrevious(const QList<Match> &matches, const std::optional<Match> &current);

} // namespace portside
