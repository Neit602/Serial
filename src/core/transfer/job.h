#pragma once

// One file transfer over a Session, for front ends (PLAN-M4 §1.5).
//
// start() claims the session's byte stream and runs the protocol engine on a
// thread of its own; progress, the result and errors come back as signals on
// the job's thread.  When the engine finishes, for any reason, the stream goes
// back to the terminal.  Destroying a running job cancels it and waits for
// the engine to stop (well under a second: it polls every 50 ms).

#include "core/transfer/transfer.h"

#include <QObject>
#include <QPointer>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace portside {

class Session;
class StreamClaim;

class TransferJob : public QObject
{
    Q_OBJECT

public:
    TransferJob(Session *session, TransferDirection direction, QStringList files,
                TransferOptions options, QObject *parent = nullptr);
    ~TransferJob() override;

    TransferDirection direction() const { return m_direction; }
    const TransferOptions &options() const { return m_options; }
    QStringList files() const { return m_files; }
    bool isRunning() const { return m_thread.joinable(); }
    // The most recent progress report.
    TransferProgress progress() const;

    // Claim the stream and begin.  Throws PortsideError when the session
    // cannot give up its stream (not connected, another transfer running).
    void start();
    // Ask the engine to stop; it sends its protocol's cancel sequence and
    // failed() follows with ErrorKind::Cancelled.
    void cancel();

Q_SIGNALS:
    void progressed(const portside::TransferProgress &progress);
    void finished(const portside::TransferResult &result);
    void failed(const portside::PortsideError &error);

private:
    void run();
    void deliverProgress();
    void complete();
    void stop();

    QPointer<Session> m_session;
    const TransferDirection m_direction;
    const QStringList m_files;
    const TransferOptions m_options;
    std::shared_ptr<StreamClaim> m_claim;
    std::thread m_thread;
    std::atomic<bool> m_cancel{false};

    mutable std::mutex m_mutex;
    TransferProgress m_progress;
    std::optional<TransferResult> m_result;
    std::optional<PortsideError> m_error;
    std::atomic<bool> m_progressPending{false};
};

} // namespace portside
