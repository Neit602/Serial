#include "support/support.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QTest>

namespace portside::test {

LoopbackSession makeLoopbackSession(LoopbackOptions options, SessionOptions sessionOptions)
{
    auto transport = std::make_unique<LoopbackTransport>(std::move(options));
    LoopbackTransport *raw = transport.get();
    auto session = std::make_unique<Session>(std::move(transport), std::move(sessionOptions));
    return {std::move(session), raw};
}

bool waitFor(const std::function<bool()> &predicate, int timeoutMs)
{
    const QDeadlineTimer deadline(timeoutMs);
    while (!predicate()) {
        if (deadline.hasExpired())
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QTest::qSleep(1);
    }
    return true;
}

Collector::Collector(Session *session)
{
    connect(session, &Session::dataReceived, this, [this](const QByteArray &bytes) { data += bytes; });
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

void writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(data);
}

} // namespace portside::test
