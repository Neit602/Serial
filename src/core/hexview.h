#pragma once

// Hex and mixed hex/ASCII rendering (PRD F10).
//
// The layout is the classic `hexdump -C` shape, which most engineers can read
// at a glance.

#include "core/enums.h"

#include <QByteArray>
#include <QList>
#include <QString>

namespace portside {

// How received data is presented (F10).
enum class ViewMode { Text, Hex, Mixed };

template <>
struct EnumNames<ViewMode>
{
    static constexpr std::array<std::pair<ViewMode, std::string_view>, 3> table{{
        {ViewMode::Text, "text"},
        {ViewMode::Hex, "hex"},
        {ViewMode::Mixed, "mixed"},
    }};
};

// One rendered row: offset, the raw bytes, and their printable form.
struct HexRow
{
    qint64 offset = 0;
    QByteArray data;
    int width = 16;

    // Space-separated hex pairs, padded so rows stay aligned, with a gap in
    // the middle that makes counting bytes easier by eye.
    QString hexColumns() const;
    // Printable ASCII, non-printables shown as '.' (hexdump convention).
    QString asciiColumns() const;
    // "00000010  48 65 6c 6c ...  |Hello|" — one complete line.
    QString render() const;
};

// Split `data` into rows of `width` bytes, numbered from `offset`.
QList<HexRow> hexRows(const QByteArray &data, qint64 offset = 0, int width = 16);

// Render `data` as a hex dump.  `stripHighBit` masks bit 7 first, so the hex
// view and the text view agree about what the device sent.
QString formatHex(const QByteArray &data, qint64 offset = 0, int width = 16,
                  bool stripHighBit = false);

} // namespace portside
