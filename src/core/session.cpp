#include "core/session.h"

#include "core/session_worker.h"
#include "core/stream_claim.h"

#include <QPromise>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace portside {

double ReconnectPolicy::delayFor(int attempt) const
{
    const double delay = initialDelay * std::pow(factor, std::max(0, attempt - 1));
    return std::min(delay, maxDelay);
}

Session::Session(std::unique_ptr<Transport> transport, SessionOptions options, QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
{
    qRegisterMetaType<portside::PortsideError>();
    qRegisterMetaType<portside::SessionState>();
    qRegisterMetaType<portside::BreakResult>();
    qRegisterMetaType<portside::ModemLines>();

    m_settings = m_options.settings.value_or(transport->settings());
    m_portName = transport->name();
    m_transport = transport.get();

    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("portside-io-%1").arg(m_portName));
    m_worker = new SessionWorker(m_options.rxLimit);
    // Moved before the thread starts; the transport becomes the worker's child
    // and so moves with it.
    m_worker->adopt(transport.release());
    m_worker->moveToThread(m_thread);

    connect(m_worker, &SessionWorker::received, this, &Session::drainReceived,
            Qt::QueuedConnection);
    connect(m_worker, &SessionWorker::transportFailed, this, &Session::handleFailure,
            Qt::QueuedConnection);
    connect(
        m_worker, &SessionWorker::claimedReceived, this,
        [this](const QByteArray &data) {
            m_bytesIn += data.size();
            Q_EMIT claimedDataReceived(data);
        },
        Qt::QueuedConnection);
    connect(
        m_worker, &SessionWorker::claimedWritten, this,
        [this](qint64 count) {
            m_bytesOut += count;
            Q_EMIT bytesSent(count);
        },
        Qt::QueuedConnection);

    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &Session::attemptReconnect);
}

Session::~Session()
{
    close();
    // The I/O thread has finished, so the worker and its transports can be
    // destroyed from here.
    delete m_worker;
    m_worker = nullptr;
}

QString Session::statusLine() const
{
    return QStringLiteral("%1 %2 · in %3 B · out %4 B · %5")
        .arg(m_portName, m_settings.describe())
        .arg(m_bytesIn)
        .arg(m_bytesOut)
        .arg(enumName(m_state));
}

// -- lifecycle -------------------------------------------------------------

void Session::open()
{
    if (m_state == SessionState::Connected || m_state == SessionState::Connecting
        || m_state == SessionState::Reconnecting) {
        throw sessionError(QStringLiteral("session for %1 is already open").arg(m_portName),
                           m_portName);
    }
    if (m_state == SessionState::Closed)
        throw sessionError(QStringLiteral("session is closed; create a new one"), m_portName);

    if (!m_thread->isRunning())
        m_thread->start();
    setState(SessionState::Connecting);

    std::optional<PortsideError> failure;
    quint64 generation = 0;
    const SerialSettings settings = m_settings;
    QMetaObject::invokeMethod(
        m_worker,
        [&] {
            try {
                m_worker->openCurrent(settings);
            } catch (const PortsideError &error) {
                failure = error;
            } catch (const std::exception &error) {
                failure = PortsideError(ErrorKind::Transport, QString::fromUtf8(error.what()),
                                        m_portName);
            }
            generation = m_worker->generation();
        },
        Qt::BlockingQueuedConnection);

    if (failure) {
        setState(SessionState::Disconnected, failure->text());
        throw *failure;
    }
    m_generation = generation;
    setState(SessionState::Connected);
}

void Session::close() noexcept
{
    if (m_state == SessionState::Closed)
        return;
    m_closing = true;
    m_reconnectTimer->stop();
    if (m_claim)
        m_claim->fail(sessionError(QStringLiteral("the session closed"), m_portName));
    if (m_thread->isRunning()) {
        // Queued behind every write already submitted, so they all complete.
        QMetaObject::invokeMethod(
            m_worker, [this] { m_worker->shutdown(); }, Qt::BlockingQueuedConnection);
        m_thread->quit();
        m_thread->wait();
    }
    // Whatever arrived before the port closed still reaches the subscribers.
    drainReceived();
    if (m_claim) {
        m_claim.reset();
        Q_EMIT streamClaimChanged(false);
    }
    setState(SessionState::Closed);
}

std::shared_ptr<StreamClaim> Session::claimStream()
{
    if (!isConnected())
        throw sessionError(
            QStringLiteral("session for %1 is %2, not connected").arg(m_portName, enumName(m_state)),
            m_portName);
    if (m_claim)
        throw sessionError(QStringLiteral("a file transfer is already using %1").arg(m_portName),
                           m_portName);
    auto claim = std::make_shared<StreamClaim>(m_worker);
    QMetaObject::invokeMethod(
        m_worker, [this, claim] { m_worker->setClaim(claim); }, Qt::BlockingQueuedConnection);
    m_claim = claim;
    Q_EMIT streamClaimChanged(true);
    return claim;
}

void Session::releaseStream()
{
    if (!m_claim)
        return;
    m_claim->fail(sessionError(QStringLiteral("the transfer no longer owns the port"), m_portName));
    if (m_thread->isRunning()) {
        QMetaObject::invokeMethod(
            m_worker, [this] { m_worker->setClaim(nullptr); }, Qt::BlockingQueuedConnection);
    }
    m_claim.reset();
    Q_EMIT streamClaimChanged(false);
}

// -- actions ---------------------------------------------------------------

template <typename R, typename F>
QFuture<R> Session::runIo(F &&work)
{
    auto promise = std::make_shared<QPromise<R>>();
    QFuture<R> future = promise->future();
    promise->start();
    QMetaObject::invokeMethod(
        m_worker,
        [promise, work = std::forward<F>(work)]() mutable {
            try {
                if constexpr (std::is_void_v<R>)
                    work();
                else
                    promise->addResult(work());
            } catch (...) {
                promise->setException(std::current_exception());
            }
            promise->finish();
        },
        Qt::QueuedConnection);
    return future;
}

template <typename R>
QFuture<R> Session::streamBusy() const
{
    return QtFuture::makeExceptionalFuture<R>(std::make_exception_ptr(sessionError(
        QStringLiteral("a file transfer is using %1").arg(m_portName), m_portName)));
}

template <typename R>
QFuture<R> Session::notConnected() const
{
    return QtFuture::makeExceptionalFuture<R>(std::make_exception_ptr(sessionError(
        QStringLiteral("session for %1 is %2, not connected").arg(m_portName, enumName(m_state)),
        m_portName)));
}

QFuture<qint64> Session::send(const QByteArray &data)
{
    if (data.isEmpty())
        return QtFuture::makeReadyValueFuture<qint64>(0);
    if (!isConnected())
        return notConnected<qint64>();
    if (m_claim)
        return streamBusy<qint64>();
    return runIo<qint64>([this, data]() -> qint64 {
        qint64 written = 0;
        try {
            written = m_worker->current()->write(data);
        } catch (const PortsideError &error) {
            if (error.isTransportError())
                m_worker->reportFailure(error);
            throw;
        }
        QMetaObject::invokeMethod(
            this,
            [this, written] {
                m_bytesOut += written;
                Q_EMIT bytesSent(written);
            },
            Qt::QueuedConnection);
        return written;
    });
}

QFuture<BreakResult> Session::sendBreak(double duration, std::optional<BreakMode> mode)
{
    if (!isConnected())
        return notConnected<BreakResult>();
    if (m_claim)
        return streamBusy<BreakResult>();
    const BreakMode chosen = mode.value_or(m_options.breakMode);
    return runIo<BreakResult>([this, duration, chosen] {
        try {
            return portside::sendBreak(*m_worker->current(), duration, chosen);
        } catch (const PortsideError &error) {
            if (error.kind() == ErrorKind::DeviceVanished)
                m_worker->reportFailure(error);
            throw;
        }
    });
}

QFuture<bool> Session::setDtr(bool value)
{
    if (!isConnected())
        return notConnected<bool>();
    return runIo<bool>([this, value] { return m_worker->current()->setDtr(value); });
}

QFuture<bool> Session::setRts(bool value)
{
    if (!isConnected())
        return notConnected<bool>();
    return runIo<bool>([this, value] { return m_worker->current()->setRts(value); });
}

QFuture<ModemLines> Session::modemLines()
{
    if (!isConnected())
        return notConnected<ModemLines>();
    return runIo<ModemLines>([this] { return m_worker->current()->modemLines(); });
}

QFuture<void> Session::applySettings(const SerialSettings &settings)
{
    settings.validate();
    m_settings = settings;
    if (!isConnected())
        return QtFuture::makeReadyVoidFuture();
    return runIo<void>([this, settings] { m_worker->current()->applySettings(settings); });
}

// -- receive path ------------------------------------------------------------

void Session::drainReceived()
{
    qint64 dropped = 0;
    const QByteArray data = m_worker->takeReceived(&dropped);
    m_rxDropped += dropped;
    if (data.isEmpty())
        return;
    m_bytesIn += data.size();
    Q_EMIT dataReceived(data);
}

// -- failure and reconnect ----------------------------------------------------

void Session::handleFailure(const PortsideError &error, quint64 generation)
{
    if (m_closing || m_state != SessionState::Connected || generation != m_generation)
        return;
    // Deliver what arrived before the failure ahead of the news of it.
    drainReceived();
    const bool reconnect = m_options.reconnect.enabled && m_options.reopen
                           && error.kind() == ErrorKind::DeviceVanished;
    Q_EMIT errorRaised(error, !reconnect);
    if (reconnect) {
        setState(SessionState::Reconnecting, error.text());
        m_attempt = 0;
        m_reconnectReason = error.text();
        scheduleReconnect();
        return;
    }
    setState(SessionState::Disconnected, error.text());
}

void Session::scheduleReconnect()
{
    if (m_closing)
        return;
    ++m_attempt;
    const auto &policy = m_options.reconnect;
    if (policy.maxAttempts && m_attempt > *policy.maxAttempts) {
        const PortsideError error(ErrorKind::DeviceVanished,
                                  QStringLiteral("gave up reconnecting to %1 after %2 attempts")
                                      .arg(m_portName)
                                      .arg(m_attempt - 1),
                                  m_portName);
        Q_EMIT errorRaised(error, true);
        setState(SessionState::Disconnected, error.text());
        return;
    }
    const double delay = policy.delayFor(m_attempt);
    Q_EMIT reconnectAttempt(m_attempt, delay, m_reconnectReason);
    m_reconnectTimer->start(int(std::lround(delay * 1000.0)));
}

void Session::attemptReconnect()
{
    if (m_closing)
        return;
    const Reopener reopen = m_options.reopen;
    const SerialSettings settings = m_settings;
    QMetaObject::invokeMethod(
        m_worker,
        [this, reopen, settings] {
            try {
                std::unique_ptr<Transport> fresh = reopen();
                fresh->applySettings(settings);
                fresh->open();
                Transport *raw = fresh.release();
                m_worker->adopt(raw);
                const quint64 generation = m_worker->generation();
                QMetaObject::invokeMethod(
                    this, [this, raw, generation] { onReconnected(raw, generation); },
                    Qt::QueuedConnection);
            } catch (const PortsideError &error) {
                // Still missing, or came back with the wrong permissions: retry.
                QMetaObject::invokeMethod(
                    this, [this, error] { onReconnectFailed(error); }, Qt::QueuedConnection);
            } catch (const std::exception &error) {
                const PortsideError wrapped(ErrorKind::Transport, QString::fromUtf8(error.what()));
                QMetaObject::invokeMethod(
                    this, [this, wrapped] { onReconnectFailed(wrapped); }, Qt::QueuedConnection);
            }
        },
        Qt::QueuedConnection);
}

void Session::onReconnected(Transport *transport, quint64 generation)
{
    if (m_closing || m_state != SessionState::Reconnecting)
        return;
    m_transport = transport;
    m_portName = transport->name();
    m_generation = generation;
    setState(SessionState::Connected, QStringLiteral("reconnected to %1").arg(m_portName));
}

void Session::onReconnectFailed(const PortsideError &error)
{
    if (m_closing || m_state != SessionState::Reconnecting)
        return;
    Q_EMIT errorRaised(error, false);
    m_reconnectReason = error.text();
    scheduleReconnect();
}

void Session::setState(SessionState state, const QString &detail)
{
    const SessionState old = m_state;
    m_state = state;
    if (old != state)
        Q_EMIT stateChanged(old, state, detail);
}

} // namespace portside
