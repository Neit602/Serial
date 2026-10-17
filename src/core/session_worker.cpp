#include "core/session_worker.h"

#include "core/stream_claim.h"
#include "core/transport.h"

#include <QMutexLocker>

namespace portside {

SessionWorker::SessionWorker(qsizetype rxLimit)
    : m_rxLimit(qMax<qsizetype>(1, rxLimit))
{
}

void SessionWorker::adopt(Transport *transport)
{
    if (m_current != nullptr) {
        m_current->disconnect(this);
        m_current->close();
    }
    transport->setParent(this);
    m_current = transport;
    ++m_generation;
    m_failureReported = false;
    connect(transport, &Transport::readyRead, this, &SessionWorker::onReadyRead);
    connect(transport, &Transport::failed, this, &SessionWorker::reportFailure);
}

void SessionWorker::openCurrent(const SerialSettings &settings)
{
    m_current->applySettings(settings);
    m_current->open();
    m_failureReported = false;
}

void SessionWorker::reportFailure(const PortsideError &error)
{
    if (m_failureReported || m_current == nullptr)
        return;
    m_failureReported = true;
    // A dead port keeps signalling; stop listening before telling anyone.
    m_current->close();
    if (m_claim) {
        m_claim->fail(error);
        // A reconnected port belongs to the terminal again.
        m_claim.reset();
    }
    Q_EMIT transportFailed(error, m_generation);
}

void SessionWorker::setClaim(std::shared_ptr<StreamClaim> claim)
{
    m_claim = std::move(claim);
    if (m_claim) {
        qint64 dropped = 0;
        const QByteArray pending = takeReceived(&dropped);
        if (!pending.isEmpty()) {
            m_claim->push(pending);
            Q_EMIT claimedReceived(pending);
        }
    }
}

void SessionWorker::writeClaimed(const QByteArray &data)
{
    if (m_current == nullptr || m_failureReported)
        throw PortsideError(ErrorKind::TransportClosed, QStringLiteral("the port is not open"));
    try {
        const qint64 written = m_current->write(data);
        m_current->flushOutput();
        Q_EMIT claimedWritten(written);
    } catch (const PortsideError &error) {
        if (error.isTransportError())
            reportFailure(error);
        throw;
    }
}

void SessionWorker::shutdown() noexcept
{
    if (m_current != nullptr) {
        m_current->disconnect(this);
        m_current->close();
    }
}

void SessionWorker::onReadyRead()
{
    if (m_current == nullptr || m_failureReported)
        return;
    QByteArray data;
    try {
        data = m_current->readAll();
    } catch (const PortsideError &error) {
        reportFailure(error);
        return;
    } catch (const std::exception &error) {
        reportFailure(PortsideError(ErrorKind::Transport,
                                    QStringLiteral("read failed: %1").arg(QString::fromUtf8(error.what())),
                                    m_current->name()));
        return;
    }
    if (data.isEmpty())
        return;
    if (m_claim) {
        m_claim->push(data);
        Q_EMIT claimedReceived(data);
        return;
    }
    {
        QMutexLocker lock(&m_rxMutex);
        m_rxSize += data.size();
        m_rx.push_back(std::move(data));
        // Bounded memory beats unbounded lag: drop the oldest data first
        // (DECISIONS D3).  The newest chunk is always kept.
        while (m_rxSize > m_rxLimit && m_rx.size() > 1) {
            m_rxSize -= m_rx.front().size();
            m_dropped += m_rx.front().size();
            m_rx.pop_front();
        }
    }
    if (!m_notifyPending.exchange(true))
        Q_EMIT received();
}

QByteArray SessionWorker::takeReceived(qint64 *dropped)
{
    // Clear the flag first: data arriving after the swap re-arms it.
    m_notifyPending = false;
    std::deque<QByteArray> chunks;
    {
        QMutexLocker lock(&m_rxMutex);
        chunks.swap(m_rx);
        m_rxSize = 0;
        if (dropped != nullptr)
            *dropped = m_dropped;
        m_dropped = 0;
    }
    if (chunks.size() == 1)
        return std::move(chunks.front());
    QByteArray joined;
    qsizetype total = 0;
    for (const QByteArray &chunk : chunks)
        total += chunk.size();
    joined.reserve(total);
    for (const QByteArray &chunk : chunks)
        joined += chunk;
    return joined;
}

} // namespace portside
