#pragma once

// ZMODEM framing and the auto-start detector (PRD F18).  The engines are in
// zmodem.cpp; this header exposes the pieces that are worth testing on their
// own and the detector the terminals use.

#include <QByteArray>
#include <QByteArrayView>

#include <array>

namespace portside::transfer::zmodem {

enum FrameType : quint8 {
    ZRQINIT = 0,
    ZRINIT = 1,
    ZSINIT = 2,
    ZACK = 3,
    ZFILE = 4,
    ZSKIP = 5,
    ZNAK = 6,
    ZABORT = 7,
    ZFIN = 8,
    ZRPOS = 9,
    ZDATA = 10,
    ZEOF = 11,
    ZFERR = 12,
    ZCRC = 13,
    ZCHALLENGE = 14,
    ZCOMPL = 15,
    ZCAN = 16,
    ZFREECNT = 17,
    ZCOMMAND = 18,
    ZSTDERR = 19,
};

// Data subpacket terminators.
inline constexpr char ZCRCE = 'h'; // end of frame, header follows
inline constexpr char ZCRCG = 'i'; // frame continues, no reply expected
inline constexpr char ZCRCQ = 'j'; // frame continues, ZACK expected
inline constexpr char ZCRCW = 'k'; // end of frame, ZACK expected

// ZRINIT capability flags (ZF0).
inline constexpr quint8 CANFDX = 0x01;
inline constexpr quint8 CANOVIO = 0x02;
inline constexpr quint8 CANBRK = 0x04;
inline constexpr quint8 CANFC32 = 0x20;
inline constexpr quint8 ESCCTL = 0x40;
inline constexpr quint8 ESC8 = 0x80;

// ZFILE conversion option (ZF0): resume an interrupted file.
inline constexpr quint8 ZCRESUM = 3;

struct Header
{
    quint8 type = ZRQINIT;
    // ZP0..ZP3 as they go on the wire; ZF0 is data[3], ZF3 data[0].
    std::array<quint8, 4> data{};

    quint32 position() const;
    quint8 flag(int index) const { return data[std::size_t(3 - index)]; }

    static Header at(quint8 type, quint32 position);
    static Header flags(quint8 type, quint8 f0, quint8 f1 = 0, quint8 f2 = 0, quint8 f3 = 0);
};

// ZDLE escaping, stateful because a CR after '@' is escaped (Telenet).
class Escaper
{
public:
    explicit Escaper(bool escapeControls = false)
        : m_escapeControls(escapeControls)
    {
    }
    void setEscapeControls(bool escape) { m_escapeControls = escape; }
    void put(QByteArray &out, quint8 byte);
    void put(QByteArray &out, QByteArrayView bytes);

private:
    bool m_escapeControls;
    quint8 m_last = 0;
};

QByteArray encodeHexHeader(const Header &header);
QByteArray encodeBinaryHeader(const Header &header, bool crc32, Escaper &escaper);
QByteArray encodeSubpacket(QByteArrayView data, char end, bool crc32, Escaper &escaper);

// Watches terminal output for the start of a ZMODEM transfer, across reads.
//
// "**\x18B00" is ZRQINIT: the device ran `sz` and wants to send.  "**\x18B01"
// is ZRINIT: it ran `rz` and is waiting for a file.
class StartDetector
{
public:
    enum class Event { None, SenderReady, ReceiverReady };

    Event feed(QByteArrayView data);
    void reset() { m_matched = 0; }

private:
    int m_matched = 0;
};

} // namespace portside::transfer::zmodem
