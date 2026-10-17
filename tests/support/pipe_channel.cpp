#include "support/pipe_channel.h"

#include <chrono>

namespace portside::test {

void Pipe::push(const QByteArray &data)
{
    if (data.isEmpty())
        return;
    {
        std::lock_guard lock(m_mutex);
        m_data += data;
    }
    m_ready.notify_all();
}

QByteArray Pipe::take(int timeoutMs)
{
    std::unique_lock lock(m_mutex);
    m_ready.wait_for(lock, std::chrono::milliseconds(qMax(0, timeoutMs)),
                     [this] { return !m_data.isEmpty(); });
    QByteArray out;
    out.swap(m_data);
    return out;
}

PipePair::PipePair()
{
    auto forward = std::make_shared<Pipe>();
    auto backward = std::make_shared<Pipe>();
    left = std::make_unique<PipeEnd>(backward, forward);
    right = std::make_unique<PipeEnd>(forward, backward);
}

PipeFilter corruptAt(qint64 offset)
{
    auto seen = std::make_shared<qint64>(0);
    return [seen, offset](const QByteArray &data) {
        QByteArray out = data;
        if (offset >= *seen && offset < *seen + data.size())
            out[offset - *seen] = char(out[offset - *seen] ^ 0x04);
        *seen += data.size();
        return out;
    };
}

PipeFilter dropAt(qint64 offset, qint64 count)
{
    auto seen = std::make_shared<qint64>(0);
    return [seen, offset, count](const QByteArray &data) {
        QByteArray out;
        for (qsizetype i = 0; i < data.size(); ++i) {
            const qint64 position = *seen + i;
            if (position < offset || position >= offset + count)
                out += data[i];
        }
        *seen += data.size();
        return out;
    };
}

} // namespace portside::test
