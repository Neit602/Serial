#include "core/transfer/transfer.h"

#include "core/transfer/engine.h"

#include <QDir>
#include <QStandardPaths>

namespace portside {

QString protocolTitle(Protocol protocol)
{
    switch (protocol) {
    case Protocol::Xmodem:
        return QStringLiteral("XMODEM");
    case Protocol::Xmodem1k:
        return QStringLiteral("XMODEM-1K");
    case Protocol::Ymodem:
        return QStringLiteral("YMODEM");
    case Protocol::YmodemG:
        return QStringLiteral("YMODEM-G");
    case Protocol::Zmodem:
        return QStringLiteral("ZMODEM");
    case Protocol::Kermit:
        return QStringLiteral("Kermit");
    case Protocol::Raw:
        break;
    }
    return QStringLiteral("raw");
}

bool protocolIsBatch(Protocol protocol)
{
    return protocol != Protocol::Xmodem && protocol != Protocol::Xmodem1k;
}

bool protocolCanReceive(Protocol protocol)
{
    return protocol != Protocol::Raw;
}

QString formatBytes(qint64 bytes)
{
    if (bytes < 1000)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1000 * 1000)
        return QStringLiteral("%1 kB").arg(double(bytes) / 1e3, 0, 'f', 1);
    if (bytes < qint64(1000) * 1000 * 1000)
        return QStringLiteral("%1 MB").arg(double(bytes) / 1e6, 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(double(bytes) / 1e9, 0, 'f', 2);
}

QString defaultDownloadDirectory()
{
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return downloads.isEmpty() ? QDir::homePath() : downloads;
}

int TransferResult::skippedCount() const
{
    int count = 0;
    for (const TransferredFile &file : files)
        count += file.skipped ? 1 : 0;
    return count;
}

QString TransferResult::summary() const
{
    const int moved = int(files.size()) - skippedCount();
    const QString verb = direction == TransferDirection::Send ? QStringLiteral("sent")
                                                              : QStringLiteral("received");
    QString text = QStringLiteral("%1: %2 %3 %4, %5 in %6 s")
                       .arg(protocolTitle(protocol), verb)
                       .arg(moved)
                       .arg(moved == 1 ? QStringLiteral("file") : QStringLiteral("files"))
                       .arg(formatBytes(bytes))
                       .arg(seconds, 0, 'f', 1);
    if (seconds > 0.05)
        text += QStringLiteral(" (%1/s)").arg(formatBytes(qint64(double(bytes) / seconds)));
    if (!check.isEmpty())
        text += QStringLiteral(", ") + check;
    if (retries > 0)
        text += QStringLiteral(", %1 %2").arg(retries).arg(retries == 1 ? QStringLiteral("retry")
                                                                         : QStringLiteral("retries"));
    if (const int skipped = skippedCount())
        text += QStringLiteral(", %1 skipped").arg(skipped);
    return text;
}

namespace {

using transfer::Job;
using transfer::Link;

// The X/Y/ZMODEM abort: a run of CANs (two are enough; lrzsz sends ten) and
// as many backspaces to erase them from a terminal that echoed them.
void sendCancelSequence(Protocol protocol, Link &link)
{
    if (protocol == Protocol::Raw || protocol == Protocol::Kermit)
        return;
    QByteArray sequence(10, '\x18');
    sequence += QByteArray(10, '\x08');
    try {
        link.put(sequence);
    } catch (const PortsideError &) {
        // The port is gone; nobody is listening.
    }
}

template <typename Run>
TransferResult run(Job &job, Run &&body)
{
    try {
        body();
    } catch (const PortsideError &error) {
        if (!error.isTransportError())
            sendCancelSequence(job.options.protocol, job.link);
        throw;
    }
    return job.finish();
}

} // namespace

TransferResult sendFiles(ByteChannel &channel, const QStringList &paths,
                         const TransferOptions &options, const std::atomic<bool> &cancel,
                         const ProgressCallback &progress)
{
    const auto files = transfer::openAll(paths);
    if (!protocolIsBatch(options.protocol) && files.size() > 1)
        throw transferError(QStringLiteral("%1 sends one file at a time")
                                .arg(protocolTitle(options.protocol)),
                            QStringLiteral("Use YMODEM, ZMODEM or Kermit to send several files."));
    Job job(channel, options, cancel, progress, TransferDirection::Send);
    qint64 total = 0;
    for (const auto &file : files)
        total += file->size();
    job.setTotalSize(total);
    return run(job, [&] {
        switch (options.protocol) {
        case Protocol::Xmodem:
        case Protocol::Xmodem1k:
            transfer::sendXmodem(job, files);
            break;
        case Protocol::Ymodem:
        case Protocol::YmodemG:
            transfer::sendYmodem(job, files);
            break;
        case Protocol::Zmodem:
            transfer::sendZmodem(job, files);
            break;
        case Protocol::Kermit:
            transfer::sendKermit(job, files);
            break;
        case Protocol::Raw:
            transfer::sendRaw(job, files);
            break;
        }
    });
}

TransferResult receiveFiles(ByteChannel &channel, const TransferOptions &options,
                            const std::atomic<bool> &cancel, const ProgressCallback &progress)
{
    if (!protocolCanReceive(options.protocol))
        throw transferError(QStringLiteral("%1 can only send").arg(protocolTitle(options.protocol)),
                            QStringLiteral("Use a raw session log to capture incoming bytes."));
    Job job(channel, options, cancel, progress, TransferDirection::Receive);
    return run(job, [&] {
        switch (options.protocol) {
        case Protocol::Xmodem:
        case Protocol::Xmodem1k:
            transfer::receiveXmodem(job);
            break;
        case Protocol::Ymodem:
        case Protocol::YmodemG:
            transfer::receiveYmodem(job);
            break;
        case Protocol::Zmodem:
            transfer::receiveZmodem(job);
            break;
        case Protocol::Kermit:
            transfer::receiveKermit(job);
            break;
        case Protocol::Raw:
            break;
        }
    });
}

} // namespace portside
