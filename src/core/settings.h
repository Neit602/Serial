#pragma once

// Serial port settings and their validation (PRD F3).

#include "core/enums.h"

#include <QMetaType>
#include <QString>
#include <QVariantMap>

namespace portside {

inline constexpr int kBaudMin = 50;
inline constexpr int kBaudMax = 4'000'000;
inline constexpr std::array<int, 4> kDataBitsChoices{5, 6, 7, 8};

// Parity, spelled the way the CLI and profiles spell it (N/E/O/M/S).
enum class Parity { None, Even, Odd, Mark, Space };
// 1.5 stop bits is only meaningful with 5 data bits on most hardware.
enum class StopBits { One, OnePointFive, Two };
enum class FlowControl { None, RtsCts, DsrDtr, XonXoff };

template <>
struct EnumNames<Parity>
{
    static constexpr std::array<std::pair<Parity, std::string_view>, 5> table{{
        {Parity::None, "N"},
        {Parity::Even, "E"},
        {Parity::Odd, "O"},
        {Parity::Mark, "M"},
        {Parity::Space, "S"},
    }};
};

template <>
struct EnumNames<StopBits>
{
    static constexpr std::array<std::pair<StopBits, std::string_view>, 3> table{{
        {StopBits::One, "1"},
        {StopBits::OnePointFive, "1.5"},
        {StopBits::Two, "2"},
    }};
};

template <>
struct EnumNames<FlowControl>
{
    static constexpr std::array<std::pair<FlowControl, std::string_view>, 4> table{{
        {FlowControl::None, "none"},
        {FlowControl::RtsCts, "rtscts"},
        {FlowControl::DsrDtr, "dsrdtr"},
        {FlowControl::XonXoff, "xonxoff"},
    }};
};

double stopBitsValue(StopBits bits);
std::optional<StopBits> stopBitsFromValue(double value);

// A set of line settings.  validate() is called wherever settings enter the
// system (CLI, profiles, dialogs, transports), so an invalid combination can
// never reach a port.
struct SerialSettings
{
    int baudrate = 115200;
    int dataBits = 8;
    Parity parity = Parity::None;
    StopBits stopBits = StopBits::One;
    FlowControl flowControl = FlowControl::None;

    // Throws PortsideError(InvalidSettings) if any field is out of range.
    void validate() const;

    // Short "115200 8N1 none" summary for status bars and banners.
    QString describe() const;

    // Plain data for TOML profiles.
    QVariantMap toMap() const;
    // Inverse of toMap(), validated.  Missing keys take their defaults.
    static SerialSettings fromMap(const QVariantMap &data);

    bool operator==(const SerialSettings &) const = default;
};

} // namespace portside

Q_DECLARE_METATYPE(portside::SerialSettings)
