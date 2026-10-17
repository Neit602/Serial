#include "core/transfer/crc.h"

#include <array>

namespace portside::transfer {

namespace {

constexpr std::array<quint16, 256> makeCrc16Table()
{
    std::array<quint16, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        unsigned value = i << 8;
        for (int bit = 0; bit < 8; ++bit)
            value = (value & 0x8000) ? (value << 1) ^ 0x1021 : value << 1;
        table[i] = quint16(value);
    }
    return table;
}

constexpr std::array<quint32, 256> makeCrc32Table()
{
    std::array<quint32, 256> table{};
    for (quint32 i = 0; i < 256; ++i) {
        quint32 value = i;
        for (int bit = 0; bit < 8; ++bit)
            value = (value & 1) ? (value >> 1) ^ 0xEDB88320u : value >> 1;
        table[i] = value;
    }
    return table;
}

constexpr std::array<quint16, 256> makeKermitTable()
{
    std::array<quint16, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        unsigned value = i;
        for (int bit = 0; bit < 8; ++bit)
            value = (value & 1) ? (value >> 1) ^ 0x8408 : value >> 1;
        table[i] = quint16(value);
    }
    return table;
}

constexpr auto kCrc16 = makeCrc16Table();
constexpr auto kCrc32 = makeCrc32Table();
constexpr auto kKermit = makeKermitTable();

} // namespace

quint16 crc16(QByteArrayView data, quint16 crc)
{
    for (const char byte : data)
        crc = quint16((crc << 8) ^ kCrc16[((crc >> 8) ^ quint8(byte)) & 0xFF]);
    return crc;
}

quint32 crc32(QByteArrayView data, quint32 crc)
{
    crc = ~crc;
    for (const char byte : data)
        crc = (crc >> 8) ^ kCrc32[(crc ^ quint8(byte)) & 0xFF];
    return ~crc;
}

quint16 crcKermit(QByteArrayView data)
{
    quint16 crc = 0;
    for (const char byte : data)
        crc = quint16((crc >> 8) ^ kKermit[(crc ^ quint8(byte)) & 0xFF]);
    return crc;
}

quint8 checksum8(QByteArrayView data)
{
    unsigned sum = 0;
    for (const char byte : data)
        sum += quint8(byte);
    return quint8(sum);
}

} // namespace portside::transfer
