"""Turning a :class:`~portside.emulation.terminal.Snapshot` into Rich output.

Separated from the widget so the conversion — which is where colour handling
and run-merging live — can be tested without starting an app.
"""

from __future__ import annotations

from rich.style import Style
from rich.text import Text

from portside.emulation.find import Match
from portside.emulation.terminal import Cell, Line, Snapshot

__all__ = ["cell_style", "snapshot_to_text"]

#: pyte names the eight ANSI colours; anything else it reports as a hex string.
_NAMED_COLOURS = {
    "black",
    "red",
    "green",
    "brown",
    "blue",
    "magenta",
    "cyan",
    "white",
    "brightblack",
    "brightred",
    "brightgreen",
    "brightbrown",
    "brightblue",
    "brightmagenta",
    "brightcyan",
    "brightwhite",
}


def _colour(value: str) -> str | None:
    """Map a pyte colour name to something Rich understands."""
    if value in {"default", ""}:
        return None
    if value in _NAMED_COLOURS:
        # pyte calls dark yellow "brown"; Rich does not.
        return "yellow" if value == "brown" else value.replace("bright", "bright_")
    # pyte passes 256-colour and truecolour values through as hex digits.
    if len(value) == 6:
        try:
            int(value, 16)
        except ValueError:
            return None
        return f"#{value}"
    return None


def cell_style(cell: Cell) -> Style:
    """The Rich style for one cell."""
    return Style(
        color=_colour(cell.fg),
        bgcolor=_colour(cell.bg),
        bold=cell.bold or None,
        italic=cell.italics or None,
        underline=cell.underscore or None,
        strike=cell.strikethrough or None,
        reverse=cell.reverse or None,
        blink=cell.blink or None,
    )


def line_to_text(line: Line, *, highlights: tuple[tuple[int, int], ...] = ()) -> Text:
    """Render one row, merging runs of identical style into single spans."""
    text = Text()
    if not line:
        return text
    run: list[str] = []
    run_style = cell_style(line[0])
    for cell in line:
        style = cell_style(cell)
        if style == run_style:
            run.append(cell.char)
            continue
        text.append("".join(run), run_style)
        run, run_style = [cell.char], style
    text.append("".join(run), run_style)
    for start, end in highlights:
        # The search highlight sits on top of whatever the device coloured.
        text.stylize("reverse bold", start, end)
    return text


def snapshot_to_text(
    snapshot: Snapshot,
    *,
    matches: tuple[Match, ...] = (),
    first_line: int = 0,
    show_cursor: bool = True,
) -> Text:
    """Render a whole snapshot, with optional search highlights and cursor.

    ``first_line`` is the absolute index of the snapshot's top row, so search
    matches found in the full scrollback can be located on screen.
    """
    by_line: dict[int, list[tuple[int, int]]] = {}
    for match in matches:
        by_line.setdefault(match.line - first_line, []).append((match.start, match.end))

    rendered = Text()
    for index, line in enumerate(snapshot.lines):
        highlights = tuple(by_line.get(index, ()))
        row = line_to_text(line, highlights=highlights)
        if (
            show_cursor
            and not snapshot.cursor.hidden
            and index == snapshot.cursor.y
            and snapshot.cursor.x < len(line)
        ):
            row.stylize("reverse", snapshot.cursor.x, snapshot.cursor.x + 1)
        rendered.append_text(row)
        if index != len(snapshot.lines) - 1:
            rendered.append("\n")
    return rendered
