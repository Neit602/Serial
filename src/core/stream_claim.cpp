#include "core/stream_claim.h"

#include "core/session_worker.h"

#include <QMetaObject>

#include <chrono>
#include <future>

namespace portside {

StreamClaim::StreamClaim(SessionWorker *worker)
    : m_worker(worker)
{
}

StreamClaim::~StreamClaim() = default;

QByteArray StreamClaim::read(int timeoutMs)
{
    std::unique_lock lock(m_mutex);
    m_ready.wait_for(lock, std::chrono::milliseconds(qMax(0, timeoutMs)),
                     [this] { return !m_buffer.isEmpty() || m_failure.has_value(); });
    if (!m_buffer.isEmpty()) {
        QByteArray out;
        out.swap(m_buffer);
        return out;
    }
    if (m_failure)
        throw *m_failure;
    return {};
}

void StreamClaim::write(const QByteArray &data)
{
    if (data.isEmpty())
        return;
    auto promise = std::make_shared<std::promise<void>>();
    std::future<void> done = promise->get_future();
    {
        std::lock_guard lock(m_mutex);
        if (m_failure)
            throw *m_failure;
        SessionWorker *worker = m_worker;
        // Queued behind anything the session wrote before the claim.
        QMetaObject::invokeMethod(
            worker,
            [worker, data, promise] {
                try {
                    worker->writeClaimed(data);
                    promise->set_value();
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            },
            Qt::QueuedConnection);
    }
    // Wait for the transport to take the bytes, watching for the claim ending
    // under us (the session closing stops the I/O thread for good).
    while (done.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
        std::lock_guard lock(m_mutex);
        if (m_failure)
            throw *m_failure;
    }
    done.get();
}

void StreamClaim::push(const QByteArray &data)
{
    if (data.isEmpty())
        return;
    {
        std::lock_guard lock(m_mutex);
        m_buffer += data;
    }
    m_ready.notify_all();
}

void StreamClaim::fail(const PortsideError &error)
{
    {
        std::lock_guard lock(m_mutex);
        if (!m_failure)
            m_failure = error;
    }
    m_ready.notify_all();
}

bool StreamClaim::hasFailed() const
{
    std::lock_guard lock(m_mutex);
    return m_failure.has_value();
}

} // namespace portside
