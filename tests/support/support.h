#pragma once

// Shared helpers for the Qt Test suites.

#include "core/loopback_transport.h"
#include "core/session.h"

#include <QByteArray>
#include <QFuture>
#include <QString>
#include <QTemporaryDir>

#include <functional>
#include <memory>

namespace portside::test {

// A session over a loopback transport, plus a pointer to that transport for
// assertions (owned by the session).
struct LoopbackSession
{
    std::unique_ptr<Session> session;
    LoopbackTransport *transport = nullptr;
};

LoopbackSession makeLoopbackSession(LoopbackOptions options = {}, SessionOptions sessionOptions = {});

// Spin the event loop until `predicate` holds or `timeoutMs` passes.
bool waitFor(const std::function<bool()> &predicate, int timeoutMs = 5000);

// Wait for a future while keeping the event loop running, then return it.
template <typename T>
QFuture<T> settle(QFuture<T> future, int timeoutMs = 5000)
{
    waitFor([&] { return future.isFinished(); }, timeoutMs);
    return future;
}

// Collects everything a session receives.
class Collector : public QObject
{
    Q_OBJECT

public:
    explicit Collector(Session *session);
    QByteArray data;
};

// Read a whole file (empty if it does not exist).
QByteArray readFile(const QString &path);
void writeFile(const QString &path, const QByteArray &data);

} // namespace portside::test
