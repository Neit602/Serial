#include "core/timestamps.h"

#include <QDateTime>

#include <cmath>
#include <cstdlib>

namespace portside {

QString formatAbsolute(double moment)
{
    const auto millis = qint64(std::llround(moment * 1000.0));
    const QDateTime local = QDateTime::fromMSecsSinceEpoch(millis);
    const int offset = local.offsetFromUtc();
    const int minutes = std::abs(offset) / 60;
    return local.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz"))
           + QStringLiteral("%1%2:%3")
                 .arg(offset < 0 ? QLatin1Char('-') : QLatin1Char('+'))
                 .arg(minutes / 60, 2, 10, QLatin1Char('0'))
                 .arg(minutes % 60, 2, 10, QLatin1Char('0'));
}

QString formatInterval(double seconds)
{
    if (seconds < 0)
        seconds = 0;
    auto whole = qint64(seconds);
    auto millis = qint64(std::llround((seconds - double(whole)) * 1000.0));
    if (millis == 1000) { // rounding crossed a second boundary
        ++whole;
        millis = 0;
    }
    const qint64 hours = whole / 3600;
    const qint64 minutes = (whole % 3600) / 60;
    const qint64 secs = whole % 60;
    return QStringLiteral("%1:%2:%3.%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'))
        .arg(millis, 3, 10, QLatin1Char('0'));
}

Timestamper::Timestamper(TimestampMode mode, double startedAt)
    : m_mode(mode)
    , m_startedAt(startedAt)
{
}

void Timestamper::reset(double now)
{
    m_startedAt = now;
    m_previous.reset();
}

QString Timestamper::prefix(double now)
{
    switch (m_mode) {
    case TimestampMode::Off:
        return {};
    case TimestampMode::Absolute:
        return QStringLiteral("[%1] ").arg(formatAbsolute(now));
    case TimestampMode::Elapsed:
        return QStringLiteral("[%1] ").arg(formatInterval(now - m_startedAt));
    case TimestampMode::Delta: {
        const double delta = m_previous ? now - *m_previous : 0.0;
        m_previous = now;
        return QStringLiteral("[+%1] ").arg(formatInterval(delta));
    }
    }
    return {};
}

double wallClock()
{
    return double(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

} // namespace portside
