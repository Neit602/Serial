#include "core/breaks.h"

#include "core/errors.h"
#include "core/transport.h"

#include <QThread>

#include <algorithm>
#include <cmath>

namespace portside {

QString BreakResult::describe() const
{
    const long millis = std::lround(duration * 1000.0);
    if (!emulated)
        return QStringLiteral("native break, %1 ms").arg(millis);
    return QStringLiteral("emulated break, %1 ms at %2 baud (%3 zero byte%4)")
        .arg(millis)
        .arg(baudrate)
        .arg(bytesSent)
        .arg(bytesSent == 1 ? QString() : QStringLiteral("s"));
}

int baudForBreak(double duration, int minimum)
{
    if (duration <= 0)
        return minimum;
    const int ideal = int(kBreakBits / duration);
    return std::max(minimum, ideal);
}

int zeroBytesForBreak(double duration, int baudrate)
{
    const double perByte = double(kBreakBits) / baudrate;
    // A hair of tolerance so 0.18 s at 50 baud is one byte, not two.
    return std::max(1, int(std::ceil(duration / perByte - 1e-9)));
}

namespace {

void defaultSleep(double seconds)
{
    QThread::msleep(static_cast<unsigned long>(std::lround(seconds * 1000.0)));
}

// Drop to a low baud rate, send 0x00, then restore the line settings.
BreakResult emulatedBreak(Transport &transport, double duration, const Sleeper &sleep)
{
    const SerialSettings original = transport.settings();
    const int baudrate = baudForBreak(duration);
    const int count = zeroBytesForBreak(duration, baudrate);
    SerialSettings slow = original;
    slow.baudrate = baudrate;
    try {
        transport.applySettings(slow);
        transport.write(QByteArray(count, '\0'));
        transport.flushOutput();
        // flushOutput only guarantees the driver has the bytes; wait out the
        // time they take to clock onto the wire before restoring the rate.
        (sleep ? sleep : Sleeper(defaultSleep))(double(count) * kBreakBits / baudrate);
    } catch (...) {
        // The line settings must come back even if the write failed, or the
        // session would be left at 50 baud.
        try {
            transport.applySettings(original);
        } catch (...) {
        }
        throw;
    }
    transport.applySettings(original);
    return {true, duration, baudrate, count};
}

} // namespace

BreakResult sendBreak(Transport &transport, double duration, BreakMode mode, const Sleeper &sleep)
{
    if (duration <= 0)
        throw PortsideError(ErrorKind::Generic, QStringLiteral("break duration must be positive"),
                            transport.name());

    if (mode == BreakMode::Auto || mode == BreakMode::Native) {
        try {
            transport.sendBreak(duration);
            return {false, duration, 0, 0};
        } catch (const PortsideError &error) {
            if (error.kind() != ErrorKind::NotSupported)
                throw;
            if (mode == BreakMode::Native) {
                throw PortsideError(
                    ErrorKind::NotSupported,
                    QStringLiteral("%1 cannot send a native break").arg(transport.name()),
                    transport.name(),
                    QStringLiteral("This adapter's driver does not implement a break condition.\n"
                                   "Use the emulated break instead (break mode 'emulated', or "
                                   "'auto' which falls back to it automatically)."));
            }
        }
    }
    return emulatedBreak(transport, duration, sleep);
}

} // namespace portside
