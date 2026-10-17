#pragma once

// The check values the transfer protocols use.  Table-driven, exact to the
// published algorithms (each has a check value for "123456789" in the tests).

#include <QByteArrayView>
#include <QtGlobal>

namespace portside::transfer {

// CRC-16/XMODEM: polynomial 0x1021, initial 0, MSB first.  XMODEM, YMODEM
// and ZMODEM's 16-bit frames.  Pass the previous value to continue a CRC.
quint16 crc16(QByteArrayView data, quint16 crc = 0);

// CRC-32 as zlib and ZMODEM's 32-bit frames compute it (reflected 0x04C11DB7,
// inverted in and out).  crc32(b, crc32(a)) == crc32(a + b).
quint32 crc32(QByteArrayView data, quint32 crc = 0);

// Kermit's type-3 block check: CRC-CCITT reflected (0x8408), initial 0, no
// final inversion — the value as Kermit puts it on the wire.
quint16 crcKermit(QByteArrayView data);

// XMODEM's 8-bit arithmetic checksum.
quint8 checksum8(QByteArrayView data);

} // namespace portside::transfer
