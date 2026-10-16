#pragma once

// Break signalling, native and emulated (PRD F12).
//
// A break is a longer-than-a-character span of logical zero on the line.
// Cisco, Juniper and most boot monitors use it to interrupt boot or drop to
// ROMMON, so it has to work even when the adapter's driver does not implement
// it.
//
// The emulated fallback exploits how a UART frames a byte: at a low enough
// baud rate, one start bit plus eight zero bits already lasts longer than the
// break we want, so sending 0x00 at that rate looks like a break to the
// receiver.

#include "core/enums.h"
#include "core/settings.h"

#include <QMetaType>
#include <QString>

#include <functional>

namespace portside {

class Transport;

// A byte on the wire is 1 start bit + 8 data bits + 1 stop bit.  Only the
// start bit and the eight zero bits are low, which is what the receiver sees
// as a break condition.
inline constexpr int kBreakBits = 9;

enum class BreakMode { Auto, Native, Emulated };

template <>
struct EnumNames<BreakMode>
{
    static constexpr std::array<std::pair<BreakMode, std::string_view>, 3> table{{
        {BreakMode::Auto, "auto"},
        {BreakMode::Native, "native"},
        {BreakMode::Emulated, "emulated"},
    }};
};

// What actually happened, so the UI can say so.
struct BreakResult
{
    bool emulated = false;
    double duration = 0.0;
    int baudrate = 0;
    // Zero bytes sent; more than one when the lowest baud rate is still too
    // fast to cover the requested duration with a single byte.
    int bytesSent = 0;

    // One line for the status bar or the log.
    QString describe() const;
};

// Highest baud rate at which one zero byte lasts at least `duration`, floored
// at `minimum` — no driver is obliged to accept an arbitrarily slow rate.
int baudForBreak(double duration, int minimum = kBaudMin);

// How many zero bytes cover `duration` at `baudrate` (DECISIONS D18).
int zeroBytesForBreak(double duration, int baudrate);

using Sleeper = std::function<void(double seconds)>;

// Send a break on `transport`.  Auto tries the driver's own break and falls
// back to the emulated one; Native and Emulated force a single strategy, which
// matters when a driver claims to support break but does not assert it.
BreakResult sendBreak(Transport &transport, double duration = 0.25,
                      BreakMode mode = BreakMode::Auto, const Sleeper &sleep = {});

} // namespace portside

Q_DECLARE_METATYPE(portside::BreakResult)
