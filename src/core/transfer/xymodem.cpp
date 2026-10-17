// XMODEM (checksum, CRC-16, 1K) and YMODEM (batch, -1K, -G), PRD F17.
//
// YMODEM is XMODEM's framing plus a block 0 that carries the file's name,
// size and date, so both share one implementation here.

#include "core/transfer/crc.h"
#include "core/transfer/engine.h"

#include <QDeadlineTimer>

#include <optional>

namespace portside::transfer {

namespace {

constexpr char SOH = 0x01;
constexpr char STX = 0x02;
constexpr char EOT = 0x04;
constexpr char ACK = 0x06;
constexpr char NAK = 0x15;
constexpr char CAN = 0x18;
constexpr char SUB = 0x1A;
constexpr char WANT_CRC = 'C';
constexpr char WANT_STREAM = 'G';

// Waiting for the other end's answer to a block, and between the bytes of one.
constexpr int kReplyTimeoutMs = 10000;
constexpr int kByteTimeoutMs = 1000;
// How often a receiver repeats its start character before the first block.
constexpr int kStartIntervalMs = 3000;

const QString kStartReceiverRemedy = QStringLiteral(
    "Start the receive on the device first (for example rx, rb, loadx or loady), then send.");
const QString kStartSenderRemedy =
    QStringLiteral("Start the send on the device first (for example sx or sb), then receive.");

QByteArray makeBlock(quint8 sequence, QByteArray payload, int size, bool crc, char pad)
{
    payload.append(size - payload.size(), pad);
    QByteArray block;
    block.reserve(size + 5);
    block += size == 1024 ? STX : SOH;
    block += char(sequence);
    block += char(~sequence);
    block += payload;
    if (crc) {
        const quint16 value = crc16(payload);
        block += char(value >> 8);
        block += char(value & 0xFF);
    } else {
        block += char(checksum8(payload));
    }
    return block;
}

// -- sending ------------------------------------------------------------------

struct StartRequest
{
    bool crc = true;
    bool streaming = false;
};

// Wait for the receiver to ask for data: NAK (checksum), 'C' (CRC) or, for
// YMODEM, 'G' (streaming).  Anything else — the echo of the command that
// started the receiver, say — is ignored.
StartRequest awaitStart(Job &job, bool allowStreaming, int timeoutMs)
{
    QDeadlineTimer deadline(timeoutMs);
    int cancels = 0;
    for (;;) {
        const int c = job.link.getByte(int(qMax<qint64>(1, deadline.remainingTime())));
        if (c < 0) {
            fail(QStringLiteral("the receiver did not ask for data within %1 s")
                     .arg(timeoutMs / 1000),
                 kStartReceiverRemedy);
        }
        if (c == CAN) {
            if (++cancels >= 2)
                failCancelledByPeer();
            continue;
        }
        cancels = 0;
        if (c == WANT_CRC)
            return {true, false};
        if (c == NAK)
            return {false, false};
        if (c == WANT_STREAM && allowStreaming)
            return {true, true};
    }
}

// A streaming receiver only ever talks to say it is giving up.
void checkStreamingCancel(Job &job)
{
    int cancels = 0;
    while (job.link.hasInput()) {
        if (job.link.getByte(0) == CAN) {
            if (++cancels >= 2)
                failCancelledByPeer();
        }
    }
}

// Send one block and wait for its ACK, resending on NAK or silence.  For
// the first block a repeated start character also means "send it again": the
// receiver never saw it.
void sendBlock(Job &job, const QByteArray &block, quint8 sequence, bool streaming,
               bool first = false)
{
    for (int attempt = 0;; ++attempt) {
        if (attempt > job.options.maxRetries) {
            fail(QStringLiteral("block %1 was not acknowledged after %2 tries")
                     .arg(sequence)
                     .arg(attempt));
        }
        if (attempt > 0)
            job.retry();
        job.link.put(block);
        if (streaming) {
            checkStreamingCancel(job);
            return;
        }
        int cancels = 0;
        for (;;) {
            const int c = job.link.getByte(kReplyTimeoutMs);
            if (c == ACK)
                return;
            if (c == NAK || c < 0 || (first && (c == WANT_CRC || c == WANT_STREAM)))
                break;
            if (c == CAN && ++cancels >= 2)
                failCancelledByPeer();
        }
    }
}

void sendEot(Job &job)
{
    for (int attempt = 0; attempt <= job.options.maxRetries; ++attempt) {
        job.link.put(EOT);
        int cancels = 0;
        for (;;) {
            const int c = job.link.getByte(kReplyTimeoutMs);
            if (c == ACK)
                return;
            // Many receivers NAK the first EOT to make sure it was not noise.
            if (c == NAK || c < 0)
                break;
            if (c == CAN && ++cancels >= 2)
                failCancelledByPeer();
        }
    }
    fail(QStringLiteral("the receiver did not acknowledge the end of the file"));
}

// Send the file's blocks, numbered from 1.
void sendData(Job &job, OutgoingFile &file, const StartRequest &start, bool allow1k)
{
    quint8 sequence = 1;
    while (!file.atEnd()) {
        const qint64 remaining = file.size() - file.position();
        // A 1K block for the bulk; 128-byte blocks for a short tail, so a
        // 1,030-byte file does not become 2,048 bytes on the wire.
        const int size = allow1k && start.crc && remaining > 896 ? 1024 : 128;
        const QByteArray payload = file.read(size);
        if (payload.isEmpty())
            break;
        sendBlock(job, makeBlock(sequence, payload, size, start.crc, SUB), sequence,
                  start.streaming, sequence == 1);
        job.advance(payload.size());
        ++sequence;
    }
    sendEot(job);
}

QString checkName(const StartRequest &start)
{
    return start.crc ? QStringLiteral("CRC-16") : QStringLiteral("checksum");
}

// -- receiving ------------------------------------------------------------------

struct Received
{
    enum Kind { Block, Eot, Timeout, Bad } kind = Timeout;
    quint8 sequence = 0;
    QByteArray data;

    static Received of(Kind kind, quint8 sequence = 0, QByteArray data = {})
    {
        Received received;
        received.kind = kind;
        received.sequence = sequence;
        received.data = std::move(data);
        return received;
    }
};

class BlockReader
{
public:
    BlockReader(Job &job, bool crc)
        : m_job(job)
        , m_crc(crc)
    {
    }

    bool crc() const { return m_crc; }
    void setCrc(bool crc) { m_crc = crc; }

    Received next(int timeoutMs)
    {
        Link &link = m_job.link;
        QDeadlineTimer deadline(timeoutMs);
        int c = -1;
        // Skip line noise until something that starts a block.
        for (;;) {
            c = link.getByte(int(qMax<qint64>(0, deadline.remainingTime())));
            if (c < 0)
                return Received::of(Received::Timeout);
            if (c == SOH || c == STX || c == EOT)
                break;
            if (c == CAN) {
                const int next = link.getByte(kByteTimeoutMs);
                if (next == CAN)
                    failCancelledByPeer();
                if (next >= 0)
                    link.unget(quint8(next));
            }
        }
        if (c == EOT)
            return Received::of(Received::Eot);
        const int size = c == STX ? 1024 : 128;
        const int sequence = link.getByte(kByteTimeoutMs);
        const int complement = link.getByte(kByteTimeoutMs);
        if (sequence < 0 || complement < 0 || (sequence ^ complement) != 0xFF)
            return Received::of(Received::Bad);
        QByteArray data;
        data.reserve(size);
        for (int i = 0; i < size; ++i) {
            const int byte = link.getByte(kByteTimeoutMs);
            if (byte < 0)
                return Received::of(Received::Bad);
            data += char(byte);
        }
        const int first = link.getByte(kByteTimeoutMs);
        if (first < 0)
            return Received::of(Received::Bad);
        if (!m_locked) {
            // The first block settles the mode: a sender may answer a late
            // 'C' after the receiver fell back to NAK, or the other way round.
            const int second = link.getByte(m_crc ? kByteTimeoutMs : 300);
            if (second >= 0 && crc16(data) == quint16((first << 8) | second)) {
                m_crc = true;
            } else if (checksum8(data) == quint8(first)) {
                m_crc = false;
            } else {
                return Received::of(Received::Bad);
            }
            m_locked = true;
        } else if (m_crc) {
            const int second = link.getByte(kByteTimeoutMs);
            if (second < 0 || crc16(data) != quint16((first << 8) | second))
                return Received::of(Received::Bad);
        } else if (checksum8(data) != quint8(first)) {
            return Received::of(Received::Bad);
        }
        return Received::of(Received::Block, quint8(sequence), data);
    }

private:
    Job &m_job;
    bool m_crc;
    bool m_locked = false;
};

void rejectBlock(Job &job, int &errors)
{
    job.retry();
    if (++errors > job.options.maxRetries)
        fail(QStringLiteral("too many bad blocks in a row; the line is too noisy"),
             QStringLiteral("Lower the baud rate or enable flow control, then try again."));
    // Let the rest of a garbled block go by before asking again.
    job.link.purge(kByteTimeoutMs);
    job.link.put(NAK);
}

[[noreturn]] void failOutOfStep(quint8 got, quint8 expected)
{
    fail(QStringLiteral("block %1 arrived when %2 was expected; the transfer is out of step")
             .arg(got)
             .arg(expected));
}

// Receive data blocks (numbered from 1) up to EOT.  `size` is the declared
// length (YMODEM), or -1.  `first` is a block already read while waiting for
// the sender to start.  The last block is held back until EOT, so XMODEM text
// mode can trim its padding.
void receiveData(Job &job, BlockReader &reader, IncomingFile &out, qint64 size, bool streaming,
                 char startChar, bool trimPadding, std::optional<Received> first = std::nullopt)
{
    quint8 expected = 1;
    int errors = 0;
    QByteArray held;
    const auto deliver = [&](QByteArray data) {
        if (size >= 0)
            data.truncate(qMax<qint64>(0, size - out.size()));
        out.write(data);
    };
    for (;;) {
        Received block;
        if (first) {
            block = *first;
            first.reset();
        } else {
            block = reader.next(kReplyTimeoutMs);
        }
        if (block.kind == Received::Eot) {
            job.link.put(ACK);
            break;
        }
        if (block.kind != Received::Block) {
            if (streaming)
                fail(QStringLiteral("a block was lost; YMODEM-G cannot resend, so the transfer "
                                    "stops"),
                     QStringLiteral("Use YMODEM (without -G) on a line that drops data."));
            rejectBlock(job, errors);
            continue;
        }
        errors = 0;
        if (block.sequence == expected) {
            if (!held.isEmpty())
                deliver(held);
            job.advance(block.data.size());
            held = block.data;
            ++expected;
            if (!streaming)
                job.link.put(ACK);
        } else if (block.sequence == quint8(expected - 1)) {
            // Our ACK was lost and the sender repeated itself.  Block 0
            // repeated means the sender missed the ACK of the file header.
            job.link.put(ACK);
            if (block.sequence == 0)
                job.link.put(startChar);
        } else {
            failOutOfStep(block.sequence, expected);
        }
    }
    if (trimPadding) {
        while (held.endsWith(SUB))
            held.chop(1);
    }
    deliver(held);
    if (size >= 0)
        out.truncate(size);
}

QString defaultXmodemName()
{
    return QStringLiteral("xmodem-%1.bin")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
}

// Wait for the first block (or a YMODEM header), repeating the start
// character, and falling back from CRC to checksum the way XMODEM receivers
// always have: three 'C's unanswered, then NAKs.
Received awaitFirstBlock(Job &job, BlockReader &reader, char &startChar, int timeoutMs,
                         bool allowFallback)
{
    QDeadlineTimer deadline(timeoutMs);
    int asked = 0;
    for (;;) {
        job.link.put(startChar);
        ++asked;
        const Received block =
            reader.next(int(qMin<qint64>(kStartIntervalMs, deadline.remainingTime())));
        if (block.kind == Received::Block || block.kind == Received::Eot)
            return block;
        if (deadline.hasExpired())
            fail(QStringLiteral("the sender did not start within %1 s").arg(timeoutMs / 1000),
                 kStartSenderRemedy);
        if (block.kind == Received::Bad) {
            // The sender has started; the block was damaged.  Ask for it
            // again in the mode we are in, without counting it as unanswered.
            job.retry();
            job.link.purge(kByteTimeoutMs);
            if (startChar != NAK)
                continue;
        }
        if (allowFallback && startChar == WANT_CRC && asked >= 3) {
            startChar = NAK;
            reader.setCrc(false);
        }
    }
}

struct YmodemHeader
{
    QString name;
    qint64 size = -1;
    QDateTime modified;
};

YmodemHeader parseHeader(const QByteArray &data)
{
    YmodemHeader header;
    const qsizetype nul = data.indexOf('\0');
    header.name = QString::fromUtf8(data.left(nul < 0 ? data.size() : nul));
    if (nul < 0)
        return header;
    QByteArray rest = data.mid(nul + 1);
    const qsizetype end = rest.indexOf('\0');
    if (end >= 0)
        rest.truncate(end);
    const QList<QByteArray> fields = rest.simplified().split(' ');
    bool ok = false;
    if (!fields.isEmpty()) {
        const qint64 size = fields.at(0).toLongLong(&ok);
        if (ok && size >= 0)
            header.size = size;
    }
    if (fields.size() > 1) {
        const qint64 seconds = fields.at(1).toLongLong(&ok, 8);
        if (ok && seconds > 0)
            header.modified = QDateTime::fromSecsSinceEpoch(seconds);
    }
    return header;
}

QByteArray headerPayload(const OutgoingFile &file)
{
    QByteArray payload = file.name().toUtf8();
    payload += '\0';
    payload += QByteArray::number(file.size());
    if (file.modified().isValid())
        payload += ' ' + QByteArray::number(file.modified().toSecsSinceEpoch(), 8);
    return payload;
}

} // namespace

// -- XMODEM -------------------------------------------------------------------

void sendXmodem(Job &job, const std::vector<std::unique_ptr<OutgoingFile>> &files)
{
    OutgoingFile &file = *files.front();
    job.beginFile(file.name(), file.size(), 1, 1);
    const StartRequest start =
        awaitStart(job, false, int(job.options.startTimeout * 1000.0));
    // Receivers repeat their start character; the extras must not be read
    // as the reply to block 1.
    job.link.purge();
    job.result.check = checkName(start);
    sendData(job, file, start, job.options.protocol == Protocol::Xmodem1k);
    job.endFile(file.path(), file.size());
}

void receiveXmodem(Job &job)
{
    ReceiveDirectory directory(job.options);
    const QString name =
        job.options.outputName.isEmpty() ? defaultXmodemName() : job.options.outputName;
    QString why;
    std::unique_ptr<IncomingFile> out = directory.open(name, &why);
    if (!out)
        fail(why, QStringLiteral("Choose another name, or set the overwrite policy to rename or "
                                 "overwrite."));
    job.beginFile(ReceiveDirectory::safeName(name), -1, 1, 1);

    char startChar = job.options.checksum ? NAK : WANT_CRC;
    BlockReader reader(job, !job.options.checksum);
    const Received first = awaitFirstBlock(job, reader, startChar,
                                           int(job.options.startTimeout * 1000.0), true);
    job.result.check = reader.crc() ? QStringLiteral("CRC-16") : QStringLiteral("checksum");
    receiveData(job, reader, *out, -1, false, startChar, job.options.textMode, first);
    const qint64 bytes = out->size();
    const QString path = out->commit();
    job.endFile(path, bytes);
}

// -- YMODEM -------------------------------------------------------------------

void sendYmodem(Job &job, const std::vector<std::unique_ptr<OutgoingFile>> &files)
{
    const bool allow1k = true;
    StartRequest start = awaitStart(job, true, int(job.options.startTimeout * 1000.0));
    job.link.purge();
    job.result.check = checkName(start);
    const int count = int(files.size());
    for (int index = 0; index < count; ++index) {
        OutgoingFile &file = *files[std::size_t(index)];
        job.beginFile(file.name(), file.size(), index + 1, count);
        if (index > 0) {
            start = awaitStart(job, true, kReplyTimeoutMs * 3);
            job.link.purge();
        }
        const QByteArray payload = headerPayload(file);
        const int size = payload.size() > 128 ? 1024 : 128;
        // Block 0 is always acknowledged, even when streaming.
        sendBlock(job, makeBlock(0, payload, size, start.crc, '\0'), 0, false);
        start = awaitStart(job, true, kReplyTimeoutMs * 3);
        job.link.purge();
        sendData(job, file, start, allow1k);
        job.endFile(file.path(), file.size());
    }
    // An empty block 0 ends the batch.
    start = awaitStart(job, true, kReplyTimeoutMs * 3);
    job.link.purge();
    sendBlock(job, makeBlock(0, {}, 128, start.crc, '\0'), 0, false);
}

void receiveYmodem(Job &job)
{
    ReceiveDirectory directory(job.options);
    const bool streaming = job.options.protocol == Protocol::YmodemG;
    char startChar = streaming ? WANT_STREAM : (job.options.checksum ? NAK : WANT_CRC);
    BlockReader reader(job, !job.options.checksum);
    int index = 0;
    for (;;) {
        // The first header may take as long as the user does to start the
        // sender; later ones follow within seconds.
        const int timeout = index == 0 ? int(job.options.startTimeout * 1000.0) : kReplyTimeoutMs * 3;
        const Received block = awaitFirstBlock(job, reader, startChar, timeout, index == 0 && !streaming);
        if (index == 0)
            job.result.check = reader.crc() ? QStringLiteral("CRC-16") : QStringLiteral("checksum");
        if (block.kind == Received::Eot) {
            // A sender repeating the end of the previous file.
            job.link.put(ACK);
            continue;
        }
        if (block.sequence != 0) {
            // The last data block of the previous file, repeated.
            job.link.put(ACK);
            continue;
        }
        const YmodemHeader header = parseHeader(block.data);
        if (header.name.isEmpty()) {
            job.link.put(ACK);
            break;
        }
        QString why;
        std::unique_ptr<IncomingFile> out = directory.open(header.name, &why);
        if (!out)
            fail(why, QStringLiteral("Move the existing file, or set the overwrite policy to "
                                     "rename or overwrite."));
        ++index;
        job.beginFile(ReceiveDirectory::safeName(header.name), header.size, index, 0);
        job.link.put(ACK);
        job.link.put(startChar);
        receiveData(job, reader, *out, header.size, streaming, startChar, false);
        const qint64 bytes = out->size();
        const QString path = out->commit(header.modified);
        job.endFile(path, bytes);
    }
}

} // namespace portside::transfer
