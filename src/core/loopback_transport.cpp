#include "core/loopback_transport.h"

#include <QMutexLocker>

namespace portside {

LoopbackTransport::LoopbackTransport(LoopbackOptions options, SerialSettings settings)
    : m_options(std::move(options))
    , m_settings(settings)
{
}

void LoopbackTransport::feed(const QByteArray &data)
{
    if (data.isEmpty())
        return;
    {
        QMutexLocker lock(&m_mutex);
        m_buffer.append(data);
    }
    notifyReadable();
}

void LoopbackTransport::notifyReadable()
{
    // One pending notification at a time: readAll() drains everything.
    if (m_notifyPending.exchange(true))
        return;
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_notifyPending = false;
            if (m_open.load())
                Q_EMIT readyRead();
        },
        Qt::QueuedConnection);
}

void LoopbackTransport::setVanishAfter(std::optional<qint64> bytes)
{
    QMutexLocker lock(&m_mutex);
    m_options.vanishAfter = bytes;
    m_transferred = 0;
}

QByteArray LoopbackTransport::written() const
{
    QMutexLocker lock(&m_mutex);
    return m_written;
}

QList<double> LoopbackTransport::breaks() const
{
    QMutexLocker lock(&m_mutex);
    return m_breaks;
}

QList<SerialSettings> LoopbackTransport::appliedSettings() const
{
    QMutexLocker lock(&m_mutex);
    return m_applied;
}

std::optional<bool> LoopbackTransport::dtr() const
{
    QMutexLocker lock(&m_mutex);
    return m_dtr;
}

std::optional<bool> LoopbackTransport::rts() const
{
    QMutexLocker lock(&m_mutex);
    return m_rts;
}

int LoopbackTransport::openCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_openCount;
}

SerialSettings LoopbackTransport::settings() const
{
    QMutexLocker lock(&m_mutex);
    return m_settings;
}

void LoopbackTransport::open()
{
    if (m_open.load())
        return;
    if (m_options.failOpenWith)
        throw *m_options.failOpenWith;
    {
        QMutexLocker lock(&m_mutex);
        ++m_openCount;
        m_transferred = 0;
    }
    m_open = true;
    bool pending = false;
    {
        QMutexLocker lock(&m_mutex);
        pending = !m_buffer.isEmpty();
    }
    if (pending)
        notifyReadable();
}

void LoopbackTransport::close() noexcept
{
    m_open = false;
}

QByteArray LoopbackTransport::readAll()
{
    requireOpen();
    QByteArray chunk;
    {
        QMutexLocker lock(&m_mutex);
        chunk.swap(m_buffer);
    }
    account(chunk.size());
    return chunk;
}

qint64 LoopbackTransport::write(const QByteArray &data)
{
    requireOpen();
    {
        QMutexLocker lock(&m_mutex);
        m_written.append(data);
    }
    if (m_options.echo)
        feed(data);
    account(data.size());
    return data.size();
}

void LoopbackTransport::applySettings(const SerialSettings &settings)
{
    settings.validate();
    QMutexLocker lock(&m_mutex);
    m_settings = settings;
    m_applied << settings;
}

void LoopbackTransport::sendBreak(double seconds)
{
    requireOpen();
    if (!m_options.supportsBreak) {
        throw PortsideError(ErrorKind::NotSupported,
                            QStringLiteral("this driver cannot send a native break"), name());
    }
    QMutexLocker lock(&m_mutex);
    m_breaks << seconds;
}

bool LoopbackTransport::setDtr(bool value)
{
    requireOpen();
    if (!m_options.supportsControlLines)
        return false;
    QMutexLocker lock(&m_mutex);
    m_dtr = value;
    return true;
}

bool LoopbackTransport::setRts(bool value)
{
    requireOpen();
    if (!m_options.supportsControlLines)
        return false;
    QMutexLocker lock(&m_mutex);
    m_rts = value;
    return true;
}

ModemLines LoopbackTransport::modemLines()
{
    requireOpen();
    return m_options.modemLines;
}

void LoopbackTransport::requireOpen() const
{
    if (!m_open.load()) {
        throw PortsideError(ErrorKind::TransportClosed,
                            QStringLiteral("%1 is not open").arg(name()), name());
    }
}

void LoopbackTransport::account(qint64 count)
{
    if (count == 0)
        return;
    bool vanish = false;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_options.vanishAfter)
            return;
        m_transferred += count;
        vanish = m_transferred >= *m_options.vanishAfter;
    }
    if (vanish) {
        m_open = false;
        throw PortsideError(ErrorKind::DeviceVanished, QStringLiteral("%1 disappeared").arg(name()),
                            name(), QStringLiteral("Simulated unplug from LoopbackTransport."));
    }
}

} // namespace portside
