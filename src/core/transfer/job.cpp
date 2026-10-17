#include "core/transfer/job.h"

#include "core/session.h"
#include "core/stream_claim.h"

#include <QMetaObject>

namespace portside {

TransferJob::TransferJob(Session *session, TransferDirection direction, QStringList files,
                         TransferOptions options, QObject *parent)
    : QObject(parent)
    , m_session(session)
    , m_direction(direction)
    , m_files(std::move(files))
    , m_options(std::move(options))
{
    qRegisterMetaType<portside::TransferProgress>();
    qRegisterMetaType<portside::TransferResult>();
    qRegisterMetaType<portside::PortsideError>();
}

TransferJob::~TransferJob()
{
    stop();
}

TransferProgress TransferJob::progress() const
{
    std::lock_guard lock(m_mutex);
    return m_progress;
}

void TransferJob::start()
{
    if (isRunning())
        throw sessionError(QStringLiteral("this transfer is already running"));
    if (!m_session)
        throw sessionError(QStringLiteral("the session is gone"));
    m_claim = m_session->claimStream();
    m_cancel = false;
    m_thread = std::thread([this] { run(); });
}

void TransferJob::cancel()
{
    m_cancel = true;
}

void TransferJob::run()
{
    const ProgressCallback report = [this](const TransferProgress &progress) {
        {
            std::lock_guard lock(m_mutex);
            m_progress = progress;
        }
        // One queued delivery at a time; it always reads the latest.
        if (!m_progressPending.exchange(true))
            QMetaObject::invokeMethod(this, [this] { deliverProgress(); }, Qt::QueuedConnection);
    };
    std::optional<TransferResult> result;
    std::optional<PortsideError> error;
    try {
        if (m_direction == TransferDirection::Send)
            result = sendFiles(*m_claim, m_files, m_options, m_cancel, report);
        else
            result = receiveFiles(*m_claim, m_options, m_cancel, report);
    } catch (const PortsideError &failure) {
        error = failure;
    } catch (const std::exception &failure) {
        error = transferError(QString::fromUtf8(failure.what()));
    }
    {
        std::lock_guard lock(m_mutex);
        m_result = std::move(result);
        m_error = std::move(error);
    }
    QMetaObject::invokeMethod(this, [this] { complete(); }, Qt::QueuedConnection);
}

void TransferJob::deliverProgress()
{
    m_progressPending = false;
    Q_EMIT progressed(progress());
}

void TransferJob::stop()
{
    if (m_thread.joinable()) {
        m_cancel = true;
        m_thread.join();
    }
    if (m_claim) {
        m_claim.reset();
        if (m_session)
            m_session->releaseStream();
    }
}

void TransferJob::complete()
{
    if (!m_thread.joinable())
        return;
    stop();
    std::optional<TransferResult> result;
    std::optional<PortsideError> error;
    {
        std::lock_guard lock(m_mutex);
        result = m_result;
        error = m_error;
    }
    Q_EMIT progressed(progress());
    if (error)
        Q_EMIT failed(*error);
    else if (result)
        Q_EMIT finished(*result);
}

} // namespace portside
