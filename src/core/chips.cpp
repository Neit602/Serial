#include "core/chips.h"

#include <array>

namespace portside {

namespace {

struct Chip
{
    quint16 vid;
    quint16 pid;
    const char *name;
};

// Sources: vendor datasheets and the Linux drivers.
constexpr std::array kChips{
    // FTDI
    Chip{0x0403, 0x6001, "FTDI FT232R"},
    Chip{0x0403, 0x6010, "FTDI FT2232"},
    Chip{0x0403, 0x6011, "FTDI FT4232"},
    Chip{0x0403, 0x6014, "FTDI FT232H"},
    Chip{0x0403, 0x6015, "FTDI FT-X"},
    // Silicon Labs
    Chip{0x10C4, 0xEA60, "Silicon Labs CP2102/CP2109"},
    Chip{0x10C4, 0xEA70, "Silicon Labs CP2105"},
    Chip{0x10C4, 0xEA71, "Silicon Labs CP2108"},
    // WCH
    Chip{0x1A86, 0x7523, "WCH CH340"},
    Chip{0x1A86, 0x7522, "WCH CH340K"},
    Chip{0x1A86, 0x5523, "WCH CH341"},
    Chip{0x1A86, 0x55D4, "WCH CH9102"},
    // Prolific
    Chip{0x067B, 0x2303, "Prolific PL2303"},
    Chip{0x067B, 0x23A3, "Prolific PL2303GC"},
    Chip{0x067B, 0x23C3, "Prolific PL2303GS"},
    // Common CDC-ACM boards
    Chip{0x2E8A, 0x0005, "Raspberry Pi Pico (CDC)"},
    Chip{0x2341, 0x0043, "Arduino Uno (CDC)"},
    Chip{0x2341, 0x8036, "Arduino Leonardo (CDC)"},
    Chip{0x239A, 0x8014, "Adafruit (CDC)"},
    Chip{0x1366, 0x1015, "SEGGER J-Link (CDC)"},
    Chip{0x0483, 0x374B, "ST-LINK/V2-1 (CDC)"},
    Chip{0x303A, 0x1001, "Espressif USB-JTAG/serial"},
};

struct Vendor
{
    quint16 vid;
    const char *name;
};

// Vendor names for adapters whose exact product ID is not in the table.
constexpr std::array kVendors{
    Vendor{0x0403, "FTDI"},
    Vendor{0x10C4, "Silicon Labs"},
    Vendor{0x1A86, "WCH"},
    Vendor{0x067B, "Prolific"},
    Vendor{0x2E8A, "Raspberry Pi"},
    Vendor{0x2341, "Arduino"},
    Vendor{0x239A, "Adafruit"},
    Vendor{0x303A, "Espressif"},
    Vendor{0x0483, "STMicroelectronics"},
    Vendor{0x1366, "SEGGER"},
    Vendor{0x0525, "Linux gadget (CDC)"},
    Vendor{0x05AC, "Apple"},
};

} // namespace

QString vendorName(quint16 vid)
{
    for (const auto &vendor : kVendors) {
        if (vendor.vid == vid)
            return QString::fromLatin1(vendor.name);
    }
    return {};
}

QString chipFamily(std::optional<quint16> vid, std::optional<quint16> pid)
{
    if (!vid || !pid)
        return {};
    for (const auto &chip : kChips) {
        if (chip.vid == *vid && chip.pid == *pid)
            return QString::fromLatin1(chip.name);
    }
    const QString vendor = vendorName(*vid);
    if (!vendor.isEmpty())
        return QStringLiteral("%1 %2").arg(vendor).arg(*pid, 4, 16, QLatin1Char('0'));
    return {};
}

} // namespace portside
