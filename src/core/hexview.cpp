#include "core/hexview.h"

#include <QStringList>

#include <stdexcept>

namespace portside {

QString HexRow::hexColumns() const
{
    QStringList pairs;
    for (const char byte : data)
        pairs << QStringLiteral("%1").arg(uchar(byte), 2, 16, QLatin1Char('0'));
    while (pairs.size() < width)
        pairs << QStringLiteral("  ");
    const int half = width / 2;
    return pairs.mid(0, half).join(QLatin1Char(' ')) + QStringLiteral("  ")
           + pairs.mid(half).join(QLatin1Char(' '));
}

QString HexRow::asciiColumns() const
{
    QString text;
    text.reserve(data.size());
    for (const char byte : data) {
        const auto value = uchar(byte);
        text += value >= 0x20 && value <= 0x7E ? QLatin1Char(char(value)) : QLatin1Char('.');
    }
    return text;
}

QString HexRow::render() const
{
    return QStringLiteral("%1  %2  |%3|")
        .arg(offset, 8, 16, QLatin1Char('0'))
        .arg(hexColumns(), asciiColumns());
}

QList<HexRow> hexRows(const QByteArray &data, qint64 offset, int width)
{
    if (width <= 0)
        throw std::invalid_argument("width must be positive");
    QList<HexRow> rows;
    for (qsizetype start = 0; start < data.size(); start += width)
        rows.append({offset + start, data.mid(start, width), width});
    return rows;
}

QString formatHex(const QByteArray &data, qint64 offset, int width, bool stripHighBit)
{
    QByteArray bytes = data;
    if (stripHighBit) {
        for (char &byte : bytes)
            byte = char(uchar(byte) & 0x7F);
    }
    QStringList lines;
    for (const HexRow &row : hexRows(bytes, offset, width))
        lines << row.render();
    return lines.join(QLatin1Char('\n'));
}

} // namespace portside
