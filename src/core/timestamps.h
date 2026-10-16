#pragma once

// Per-line timestamps for session logs (PRD F16).
//
// Four modes, all pure functions of (line, arrival time), so the same
// formatter serves the log writer and any future exporter.  A line is stamped
// with the time its *first* byte arrived (DECISIONS D20).
//
// Times are seconds since the Unix epoch, as doubles, so tests can inject an
// exact clock.

#include "core/enums.h"

#include <QString>

#include <optional>

namespace portside {

enum class TimestampMode { Off, Absolute, Elapsed, Delta };

template <>
struct EnumNames<TimestampMode>
{
    static constexpr std::array<std::pair<TimestampMode, std::string_view>, 4> table{{
        {TimestampMode::Off, "off"},
        {TimestampMode::Absolute, "absolute"},
        {TimestampMode::Elapsed, "elapsed"},
        {TimestampMode::Delta, "delta"},
    }};
};

// Local time with its UTC offset, so logs from several machines line up:
// 2026-10-16T14:03:07.250+02:00.
QString formatAbsolute(double moment);
// HH:MM:SS.mmm — readable at a glance and sortable.
QString formatInterval(double seconds);

// Knows when the session started and when the last line arrived, which is
// what elapsed and delta need.
class Timestamper
{
public:
    explicit Timestamper(TimestampMode mode = TimestampMode::Off, double startedAt = 0.0);

    TimestampMode mode() const { return m_mode; }
    void setMode(TimestampMode mode) { m_mode = mode; }

    // Restart the elapsed clock (F16 says it must be resettable).
    void reset(double now);
    // The prefix for a line whose first byte arrived at `now`; empty when
    // stamping is off, so callers need no special case.
    QString prefix(double now);
    QString formatLine(const QString &line, double now) { return prefix(now) + line; }

private:
    TimestampMode m_mode;
    double m_startedAt;
    std::optional<double> m_previous;
};

// Seconds since the epoch, now.
double wallClock();

} // namespace portside
