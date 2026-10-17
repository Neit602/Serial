#pragma once

// Shared machinery for the protocol engines.  Not public API: front ends use
// transfer.h and job.h.

#include "core/transfer/transfer.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>

#include <memory>
#include <optional>
#include <vector>

namespace portside::transfer {

// A ByteChannel with a read buffer, byte-at-a-time reads with timeouts and
// cancellation.  Every wait polls the cancel flag at least every 50 ms.
class Link
{
public:
    Link(ByteChannel &channel, const std::atomic<bool> &cancel);

    // The next byte (0–255), or -1 when `timeoutMs` passes first.  Throws
    // PortsideError(Cancelled) when cancelled.
    int getByte(int timeoutMs);
    // Put a byte back to be read again next.
    void unget(quint8 byte);
    // True when a byte can be read without waiting.
    bool hasInput();

    void put(const QByteArray &data);
    void put(char byte);

    // Throw away buffered input, then anything more that arrives until the
    // line has been quiet for `quietMs` (bounded by `maxMs`).
    void purge(int quietMs = 0, int maxMs = 3000);

    void checkCancelled() const;
    bool cancelRequested() const { return m_cancel.load(); }

    qint64 bytesRead() const { return m_read; }
    qint64 bytesWritten() const { return m_written; }

private:
    bool fill(int timeoutMs);

    ByteChannel &m_channel;
    const std::atomic<bool> &m_cancel;
    QByteArray m_buffer;
    qsizetype m_offset = 0;
    qint64 m_read = 0;
    qint64 m_written = 0;
};

[[noreturn]] void fail(const QString &message, const QString &remedy = {});
[[noreturn]] void failCancelledByPeer();

// Progress reporting and the result under construction, shared by every
// engine.  Calls back at most every 100 ms, plus at each file boundary.
class Job
{
public:
    Job(ByteChannel &channel, const TransferOptions &options, const std::atomic<bool> &cancel,
        const ProgressCallback &callback, TransferDirection direction);

    Link link;
    const TransferOptions &options;
    TransferResult result;

    void beginFile(const QString &name, qint64 size, int index, int count);
    void advance(qint64 bytes);
    void setFileBytes(qint64 bytes);
    void retry();
    // Close the current file's entry in the result.
    void endFile(const QString &path, qint64 bytes, bool skipped = false,
                 const QString &note = {});
    void setTotalSize(qint64 size) { m_progress.totalSize = size; }
    TransferResult finish();

private:
    void report(bool force);

    ProgressCallback m_callback;
    TransferProgress m_progress;
    QElapsedTimer m_clock;
    qint64 m_lastReport = -1000;
};

// A file being sent.
class OutgoingFile
{
public:
    explicit OutgoingFile(const QString &path);

    QString path() const { return m_path; }
    // The bare file name, as it goes on the wire.
    QString name() const { return m_name; }
    qint64 size() const { return m_size; }
    QDateTime modified() const { return m_modified; }

    QByteArray read(qint64 maxBytes);
    void seek(qint64 position);
    qint64 position() const { return m_file.pos(); }
    bool atEnd() const { return m_file.atEnd(); }

private:
    QString m_path;
    QString m_name;
    qint64 m_size = 0;
    QDateTime m_modified;
    QFile m_file;
};

// Open every file before a word goes on the wire, so a typo fails at once
// instead of after the peer has been woken.
std::vector<std::unique_ptr<OutgoingFile>> openAll(const QStringList &paths);

// A file being received: written to "<name>.part" and renamed into place
// only when complete (DECISIONS D53), so a broken transfer never leaves a
// file that looks finished.
class IncomingFile
{
public:
    IncomingFile(QString finalPath, QString partPath, qint64 resumeFrom);
    ~IncomingFile();

    QString finalPath() const { return m_finalPath; }
    qint64 size() const { return m_written; }
    // Bytes that were already there from an interrupted transfer.
    qint64 resumedFrom() const { return m_resumedFrom; }

    void write(const QByteArray &data);
    // Drop everything past `size` (YMODEM's declared length).
    void truncate(qint64 size);
    // Rename into place (applying the overwrite policy decided at open) and
    // set the modification time when known.  Returns the final path.
    QString commit(const QDateTime &modified = {});
    // Delete the partial file — or keep it for a later resume.
    void abandon(bool keepPartial = false);
    // What the destructor does with a file that was never committed.
    void setKeepPartial(bool keep) { m_keepPartial = keep; }

private:
    QString m_finalPath;
    QString m_partPath;
    QFile m_file;
    qint64 m_written = 0;
    qint64 m_resumedFrom = 0;
    bool m_done = false;
    bool m_keepPartial = false;
};

// Where received files go, and the overwrite policy.
class ReceiveDirectory
{
public:
    explicit ReceiveDirectory(const TransferOptions &options);

    // Make a name from the peer safe: no directories, no "..", nothing a
    // filesystem would reject.  A device cannot write outside the directory.
    static QString safeName(const QString &peerName);

    // Start receiving `peerName`.  Empty when the policy refuses it (the name
    // is taken); `why` then says so.  Throws when the file cannot be created.
    std::unique_ptr<IncomingFile> open(const QString &peerName, QString *why = nullptr,
                                       bool resume = false);

    QString directory() const { return m_directory; }

private:
    QString m_directory;
    OverwritePolicy m_policy;
};

// -- the engines ---------------------------------------------------------

void sendXmodem(Job &job, const std::vector<std::unique_ptr<OutgoingFile>> &files);
void receiveXmodem(Job &job);
void sendYmodem(Job &job, const std::vector<std::unique_ptr<OutgoingFile>> &files);
void receiveYmodem(Job &job);
void sendZmodem(Job &job, const std::vector<std::unique_ptr<OutgoingFile>> &files);
void receiveZmodem(Job &job);
void sendKermit(Job &job, const std::vector<std::unique_ptr<OutgoingFile>> &files);
void receiveKermit(Job &job);
void sendRaw(Job &job, const std::vector<std::unique_ptr<OutgoingFile>> &files);

} // namespace portside::transfer
