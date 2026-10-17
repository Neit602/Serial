#pragma once

// File transfers (PRD F17–F19): XMODEM, YMODEM, ZMODEM, Kermit and raw send.
//
// A protocol engine is plain blocking code: it reads and writes a
// ByteChannel and returns a TransferResult or throws PortsideError.  Engines
// run on a thread of their own (TransferJob, in job.h, does that for a
// Session), so a protocol waiting ten seconds for an ACK never blocks a UI or
// the session's I/O thread.  Keeping them free of Qt's event loop is also what
// lets the tests drive two engines against each other over an in-memory pipe.

#include "core/enums.h"
#include "core/errors.h"

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

namespace portside {

enum class Protocol { Xmodem, Xmodem1k, Ymodem, YmodemG, Zmodem, Kermit, Raw };

template <>
struct EnumNames<Protocol>
{
    static constexpr std::array<std::pair<Protocol, std::string_view>, 7> table{{
        {Protocol::Xmodem, "xmodem"},
        {Protocol::Xmodem1k, "xmodem-1k"},
        {Protocol::Ymodem, "ymodem"},
        {Protocol::YmodemG, "ymodem-g"},
        {Protocol::Zmodem, "zmodem"},
        {Protocol::Kermit, "kermit"},
        {Protocol::Raw, "raw"},
    }};
};

// "XMODEM-1K", "ZMODEM", "raw" — for titles and messages.
QString protocolTitle(Protocol protocol);
// Whether a protocol can carry several files in one transfer.
bool protocolIsBatch(Protocol protocol);
// Whether a protocol can receive (raw is send-only).
bool protocolCanReceive(Protocol protocol);

enum class TransferDirection { Send, Receive };

template <>
struct EnumNames<TransferDirection>
{
    static constexpr std::array<std::pair<TransferDirection, std::string_view>, 2> table{{
        {TransferDirection::Send, "send"},
        {TransferDirection::Receive, "receive"},
    }};
};

// What to do when a received file's name is already taken (DECISIONS D52).
enum class OverwritePolicy { Refuse, Rename, Overwrite };

template <>
struct EnumNames<OverwritePolicy>
{
    static constexpr std::array<std::pair<OverwritePolicy, std::string_view>, 3> table{{
        {OverwritePolicy::Refuse, "refuse"},
        {OverwritePolicy::Rename, "rename"},
        {OverwritePolicy::Overwrite, "overwrite"},
    }};
};

struct TransferOptions
{
    Protocol protocol = Protocol::Zmodem;

    // -- receiving ---------------------------------------------------------
    // Where received files go; empty means the current directory.
    QString directory;
    // XMODEM carries no file name, so the receiver has to supply one.  Empty
    // means "xmodem-<date>-<time>.bin".
    QString outputName;
    OverwritePolicy overwrite = OverwritePolicy::Refuse;
    // XMODEM pads the last block with SUB (0x1A).  A text transfer strips that
    // padding; a binary one never does, because the byte may be real data.
    bool textMode = false;
    // X/YMODEM receive: ask for the 8-bit checksum instead of CRC-16, for very
    // old senders.
    bool checksum = false;
    // ZMODEM receive: continue a partial download left by an interrupted
    // transfer (the "<name>.part" file) instead of starting over.
    bool resume = false;

    // -- raw send ----------------------------------------------------------
    int rawChunkSize = 256;
    // Seconds between chunks, for a receiver with no flow control.
    double rawChunkDelay = 0.0;

    // -- timing ------------------------------------------------------------
    // How long to wait for the other end to start, in seconds.
    double startTimeout = 60.0;
    // Consecutive errors on one block or packet before giving up.
    int maxRetries = 10;

    bool operator==(const TransferOptions &) const = default;
};

struct TransferProgress
{
    QString file;
    // 1-based index of the current file; fileCount is 0 when the protocol
    // does not say how many are coming.
    int fileIndex = 0;
    int fileCount = 0;
    qint64 fileBytes = 0;
    // -1 when the protocol carries no size (XMODEM).
    qint64 fileSize = -1;
    qint64 totalBytes = 0;
    qint64 totalSize = -1;
    int retries = 0;
    double seconds = 0.0;

    double bytesPerSecond() const { return seconds > 0.0 ? double(totalBytes) / seconds : 0.0; }
};

struct TransferredFile
{
    // The name on the wire, and where it was read from or written to.
    QString name;
    QString path;
    qint64 bytes = 0;
    // Set when the receiver declined the file (it already existed, ...).
    bool skipped = false;
    QString note;
};

struct TransferResult
{
    Protocol protocol = Protocol::Zmodem;
    TransferDirection direction = TransferDirection::Send;
    QList<TransferredFile> files;
    qint64 bytes = 0;
    double seconds = 0.0;
    int retries = 0;
    // The integrity check the peers agreed on: "CRC-16", "CRC-32", "checksum".
    QString check;

    int skippedCount() const;
    // "ZMODEM: received 2 files, 1.2 MB in 4.1 s (300 kB/s), CRC-32".
    QString summary() const;
};

// "1.2 MB", "512 B".
QString formatBytes(qint64 bytes);

// Where received files go when nothing says otherwise: the user's Downloads
// folder on every platform.
QString defaultDownloadDirectory();

// The byte stream a protocol engine talks over.  Both calls block; both may
// throw PortsideError (the device vanished, the session closed).
class ByteChannel
{
public:
    virtual ~ByteChannel() = default;
    // Whatever has arrived, waiting up to `timeoutMs` for the first byte; an
    // empty result means the wait timed out.  0 polls.
    virtual QByteArray read(int timeoutMs) = 0;
    // Hand every byte to the transport, returning once it has taken them —
    // the backpressure that keeps a fast sender from queueing a whole file.
    virtual void write(const QByteArray &data) = 0;
};

using ProgressCallback = std::function<void(const TransferProgress &)>;

// Run a whole transfer on the calling thread.  `cancel` is polled; setting it
// makes the engine send its protocol's cancel sequence and throw
// PortsideError(Cancelled).  `progress` is called from the calling thread.
TransferResult sendFiles(ByteChannel &channel, const QStringList &paths,
                         const TransferOptions &options, const std::atomic<bool> &cancel,
                         const ProgressCallback &progress = {});
TransferResult receiveFiles(ByteChannel &channel, const TransferOptions &options,
                            const std::atomic<bool> &cancel,
                            const ProgressCallback &progress = {});

} // namespace portside

Q_DECLARE_METATYPE(portside::Protocol)
Q_DECLARE_METATYPE(portside::TransferDirection)
Q_DECLARE_METATYPE(portside::TransferProgress)
Q_DECLARE_METATYPE(portside::TransferResult)
