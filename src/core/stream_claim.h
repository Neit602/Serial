#pragma once

// A session's byte stream, taken over by a file transfer (PLAN-M4 §1.1).
//
// While a claim is active the session hands every received byte to it rather
// than to the terminal, and refuses the terminal's writes, so an XMODEM ACK
// can never be eaten by the screen and a keystroke can never land in the
// middle of a block.  The claim is a ByteChannel: a protocol engine running on
// its own thread reads and writes it directly.  Writes still go through the
// session's I/O thread, after anything queued before them.

#include "core/errors.h"
#include "core/transfer/transfer.h"

#include <QByteArray>

#include <condition_variable>
#include <mutex>
#include <optional>

namespace portside {

class SessionWorker;

class StreamClaim : public ByteChannel
{
public:
    explicit StreamClaim(SessionWorker *worker);
    ~StreamClaim() override;
    StreamClaim(const StreamClaim &) = delete;
    StreamClaim &operator=(const StreamClaim &) = delete;

    // -- ByteChannel (the transfer's thread) ---------------------------------
    QByteArray read(int timeoutMs) override;
    void write(const QByteArray &data) override;

    // -- the session's side ----------------------------------------------------
    // I/O thread: bytes from the device.
    void push(const QByteArray &data);
    // Any thread: end the claim with an error that every pending and future
    // read or write throws (device lost, session closed, claim released).
    void fail(const PortsideError &error);
    bool hasFailed() const;

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_ready;
    QByteArray m_buffer;
    std::optional<PortsideError> m_failure;
    SessionWorker *m_worker = nullptr;
};

} // namespace portside
