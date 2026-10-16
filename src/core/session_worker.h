#pragma once

// The half of a Session that lives on its I/O thread.  Not public API.
//
// It owns the transports (the current one and any retired by a reconnect),
// turns readyRead into a bounded receive buffer, and reports a lost device
// exactly once per transport.  Everything except takeReceived() runs on the
// I/O thread.

#include "core/errors.h"
#include "core/settings.h"

#include <QByteArray>
#include <QMutex>
#include <QObject>

#include <atomic>
#include <deque>

namespace portside {

class Transport;

class SessionWorker : public QObject
{
    Q_OBJECT

public:
    explicit SessionWorker(qsizetype rxLimit);

    // Before the thread starts (or on the I/O thread): make `transport` the
    // current one.  The worker takes ownership; the previous transport is
    // closed and kept until the worker dies, so pointers a test holds stay
    // valid.
    void adopt(Transport *transport);
    Transport *current() const { return m_current; }
    // Bumped on every adopt(), so a failure from a retired transport can be
    // told apart from one on the current transport.
    quint64 generation() const { return m_generation; }

    // Apply settings and open the current transport.  Throws PortsideError.
    void openCurrent(const SerialSettings &settings);
    // Report a failure seen by the session (a failed write).  Idempotent per
    // transport.
    void reportFailure(const PortsideError &error);
    // Close the current transport; called once, at session close.
    void shutdown() noexcept;

    // Any thread: everything received since the last call, oldest first, and
    // the number of bytes dropped since then because the buffer was full.
    QByteArray takeReceived(qint64 *dropped);

Q_SIGNALS:
    // The receive buffer went from empty to non-empty.
    void received();
    void transportFailed(const portside::PortsideError &error, quint64 generation);

private:
    void onReadyRead();

    Transport *m_current = nullptr;
    quint64 m_generation = 0;
    bool m_failureReported = false;

    QMutex m_rxMutex;
    std::deque<QByteArray> m_rx;
    qsizetype m_rxSize = 0;
    qint64 m_dropped = 0;
    const qsizetype m_rxLimit;
    std::atomic<bool> m_notifyPending{false};
};

} // namespace portside
