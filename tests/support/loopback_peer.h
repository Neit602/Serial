#pragma once

// The device's end of a loopback transport, as a ByteChannel: a protocol
// engine on a test thread reads what the session wrote and writes what the
// session will receive.  The transport must not echo.

#include "core/loopback_transport.h"
#include "core/transfer/transfer.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QThread>

namespace portside::test {

class LoopbackPeer : public ByteChannel
{
public:
    explicit LoopbackPeer(LoopbackTransport *transport)
        : m_transport(transport)
    {
    }

    QByteArray read(int timeoutMs) override
    {
        QElapsedTimer clock;
        clock.start();
        for (;;) {
            if (!m_transport)
                throw PortsideError(ErrorKind::DeviceVanished, QStringLiteral("the loopback is gone"));
            const QByteArray all = m_transport->written();
            if (all.size() > m_offset) {
                const QByteArray fresh = all.mid(m_offset);
                m_offset = all.size();
                return fresh;
            }
            if (clock.elapsed() >= timeoutMs)
                return {};
            QThread::msleep(2);
        }
    }

    void write(const QByteArray &data) override
    {
        if (m_transport)
            m_transport->feed(data);
    }

private:
    QPointer<LoopbackTransport> m_transport;
    qsizetype m_offset = 0;
};

} // namespace portside::test
