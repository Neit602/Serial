#pragma once

// Qt key events → serial bytes (PRD F7, F9).
//
// Enter and Backspace follow the session's TextPolicy, so they behave as the
// profile says on every OS.  Function keys and Ctrl combinations go to the
// device — Ctrl-C, Ctrl-Z and friends belong to the switch (DECISIONS D23).

#include "core/textio.h"

#include <QByteArray>

class QKeyEvent;

namespace portside::gui {

// What a key press should put on the wire, or an empty array for keys the
// window keeps for itself (PgUp/PgDn scroll the local scrollback).
// `applicationCursor` is the terminal's DECCKM mode: full-screen programs ask
// for ESC O A rather than ESC [ A.
QByteArray keyToBytes(const QKeyEvent &event, const TextPolicy &policy,
                      bool applicationCursor = false);

} // namespace portside::gui
