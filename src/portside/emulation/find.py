"""Searching the screen and scrollback (PRD F8).

Literal search by default: an engineer looking for ``Gi1/0/1`` or ``10.0.0.1``
should not have to escape anything.  Regex is opt-in.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

from portside.core.errors import InvalidSettingsError

__all__ = ["Match", "SearchOptions", "find_all", "find_next", "find_prev"]


@dataclass(frozen=True, slots=True)
class SearchOptions:
    """How to search (F8)."""

    case_sensitive: bool = False
    regex: bool = False
    whole_word: bool = False


@dataclass(frozen=True, slots=True)
class Match:
    """One hit: a line index (0 = oldest kept line) and a column range."""

    line: int
    start: int
    end: int

    @property
    def length(self) -> int:
        """Width of the match in characters."""
        return self.end - self.start


def _compile(query: str, options: SearchOptions) -> re.Pattern[str]:
    """Build the pattern, turning a bad regex into a typed error."""
    pattern = query if options.regex else re.escape(query)
    if options.whole_word:
        pattern = rf"\b(?:{pattern})\b"
    flags = 0 if options.case_sensitive else re.IGNORECASE
    try:
        return re.compile(pattern, flags)
    except re.error as exc:
        raise InvalidSettingsError(f"invalid search pattern {query!r}: {exc}") from exc


def find_all(
    lines: list[str] | tuple[str, ...],
    query: str,
    options: SearchOptions | None = None,
) -> list[Match]:
    """Every match in ``lines``, ordered oldest line first, left to right.

    Matches are found per line: the emulator has already wrapped long output
    into lines, so a hit that spans a wrap is two matches, as it looks on screen.
    """
    if not query:
        return []
    pattern = _compile(query, options or SearchOptions())
    matches: list[Match] = []
    for index, line in enumerate(lines):
        for found in pattern.finditer(line):
            if found.end() > found.start():
                matches.append(Match(line=index, start=found.start(), end=found.end()))
    return matches


def find_next(matches: list[Match], current: Match | None) -> Match | None:
    """The match after ``current``, wrapping around to the first."""
    if not matches:
        return None
    if current is None:
        return matches[0]
    for match in matches:
        if (match.line, match.start) > (current.line, current.start):
            return match
    return matches[0]


def find_prev(matches: list[Match], current: Match | None) -> Match | None:
    """The match before ``current``, wrapping around to the last."""
    if not matches:
        return None
    if current is None:
        return matches[-1]
    for match in reversed(matches):
        if (match.line, match.start) < (current.line, current.start):
            return match
    return matches[-1]
