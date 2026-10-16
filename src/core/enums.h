#pragma once

// String names for the enums that appear in profiles, on the command line and
// in the UI.  Each enum specialises EnumNames<E> with its (value, name) table;
// the free functions below then give every enum the same three operations.

#include <QString>
#include <QStringList>
#include <QStringView>

#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace portside {

template <typename E>
struct EnumNames; // specialised next to each enum: static constexpr table

template <typename E>
QString enumName(E value)
{
    for (const auto &[candidate, name] : EnumNames<E>::table) {
        if (candidate == value)
            return QString::fromLatin1(name.data(), qsizetype(name.size()));
    }
    return {};
}

template <typename E>
std::optional<E> enumFromName(QStringView text)
{
    for (const auto &[candidate, name] : EnumNames<E>::table) {
        if (text == QLatin1StringView(name.data(), qsizetype(name.size())))
            return candidate;
    }
    return std::nullopt;
}

template <typename E>
QStringList enumNameList()
{
    QStringList names;
    for (const auto &entry : EnumNames<E>::table)
        names << QString::fromLatin1(entry.second.data(), qsizetype(entry.second.size()));
    return names;
}

template <typename E>
constexpr auto enumValues()
{
    std::array<E, EnumNames<E>::table.size()> values{};
    for (std::size_t i = 0; i < values.size(); ++i)
        values[i] = EnumNames<E>::table[i].first;
    return values;
}

} // namespace portside
