#pragma once

// Two ByteChannels joined back to back, for running a sending and a
// receiving protocol engine against each other with no port in between.
// A filter on either direction can corrupt, drop or watch the bytes.

#include "core/transfer/transfer.h"

#include <QByteArray>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

namespace portside::test {

class PipeEnd;

class Pipe
{
public:
    void push(const QByteArray &data);
    QByteArray take(int timeoutMs);

private:
    std::mutex m_mutex;
    std::condition_variable m_ready;
    QByteArray m_data;
};

// A filter sees each write and returns what reaches the other end.
using PipeFilter = std::function<QByteArray(const QByteArray &)>;

class PipeEnd : public ByteChannel
{
public:
    PipeEnd(std::shared_ptr<Pipe> in, std::shared_ptr<Pipe> out)
        : m_in(std::move(in))
        , m_out(std::move(out))
    {
    }

    void setFilter(PipeFilter filter) { m_filter = std::move(filter); }

    QByteArray read(int timeoutMs) override { return m_in->take(timeoutMs); }
    void write(const QByteArray &data) override
    {
        m_out->push(m_filter ? m_filter(data) : data);
    }

private:
    std::shared_ptr<Pipe> m_in;
    std::shared_ptr<Pipe> m_out;
    PipeFilter m_filter;
};

struct PipePair
{
    PipePair();
    std::unique_ptr<PipeEnd> left;
    std::unique_ptr<PipeEnd> right;
};

// Flip one bit of the byte at `offset` in the stream the filter sees.
PipeFilter corruptAt(qint64 offset);
// Drop `count` bytes starting at `offset`.
PipeFilter dropAt(qint64 offset, qint64 count);

} // namespace portside::test
