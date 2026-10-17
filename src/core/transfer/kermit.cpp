// Kermit send and receive (PRD F19), written against the protocol book and
// G-Kermit's behaviour.  Binary mode only: bytes arrive exactly as sent.

#include "core/transfer/kermit.h"

#include "core/transfer/crc.h"
#include "core/transfer/engine.h"

#include <QDeadlineTimer>

#include <algorithm>

namespace portside::transfer {

namespace kermit {

namespace {

constexpr int kMaxPacket = 9000;

bool validPrefix(char c)
{
    return (c > 32 && c < 63) || (c > 95 && c < 127);
}

unsigned sum(QByteArrayView bytes)
{
    unsigned total = 0;
    for (const char byte : bytes)
        total += quint8(byte);
    return total;
}

char check1(QByteArrayView bytes)
{
    const unsigned s = sum(bytes);
    return tochar(int((((s & 192) >> 6) + s) & 63));
}

} // namespace

QByteArray encodeInit(const InitParams &params)
{
    QByteArray s;
    s += tochar(std::min(params.maxLength, 94));
    s += tochar(params.timeout);
    s += tochar(params.padCount);
    s += ctl(params.padChar);
    s += tochar(params.eol);
    s += params.ctlPrefix;
    s += params.binPrefix;
    s += char('0' + params.checkType);
    s += params.repeatPrefix;
    s += tochar((params.longPackets ? 2 : 0) | (params.attributes ? 8 : 0));
    s += tochar(1); // window size: no sliding windows
    const int longest = params.longPackets ? params.longLength : params.maxLength;
    s += tochar(longest / 95);
    s += tochar(longest % 95);
    s += '0';       // no checkpointing
    s += "+++";     // checkpoint interval, unused
    // WHATAMI: the field is valid, binary mode, file names taken literally.
    s += tochar(32 | 2 | 4);
    return s;
}

InitParams decodeInit(QByteArrayView data)
{
    InitParams p;
    const qsizetype n = data.size();
    const auto at = [&](qsizetype index) { return data[index]; };
    p.maxLength = n >= 1 ? unchar(at(0)) : 80;
    if (p.maxLength < 10)
        p.maxLength = 80;
    p.timeout = n >= 2 ? unchar(at(1)) : 5;
    p.padCount = n >= 3 ? std::max(0, unchar(at(2))) : 0;
    p.padChar = n >= 4 ? ctl(at(3)) : char(0);
    p.eol = n >= 5 ? char(unchar(at(4))) : '\r';
    if (p.eol < 2 || p.eol > 31)
        p.eol = '\r';
    p.ctlPrefix = n >= 6 && validPrefix(at(5)) ? at(5) : '#';
    p.binPrefix = n >= 7 ? at(6) : 'N';
    p.checkType = n >= 8 ? at(7) - '0' : 1;
    if (p.checkType < 1 || p.checkType > 3)
        p.checkType = 1;
    p.repeatPrefix = n >= 9 ? at(8) : ' ';
    p.longPackets = false;
    p.attributes = false;
    p.longLength = 0;
    if (n >= 10) {
        const int capas = unchar(at(9));
        p.longPackets = (capas & 2) != 0;
        p.attributes = (capas & 8) != 0;
        qsizetype last = 9;
        while (last < n - 1 && (unchar(at(last)) & 1))
            ++last;
        if (p.longPackets) {
            // Without MAXLX the protocol says 500.
            p.longLength = 500;
            if (n > last + 3)
                p.longLength = unchar(at(last + 2)) * 95 + unchar(at(last + 3));
        }
    }
    return p;
}

int Agreed::dataCapacity() const
{
    if (longPackets)
        return sendLength - checkType - 10;
    return sendLength - checkType - 2;
}

Agreed agree(const InitParams &ours, const InitParams &theirs)
{
    Agreed a;
    a.checkType = theirs.checkType == ours.checkType ? ours.checkType : 1;
    a.longPackets = ours.longPackets && theirs.longPackets && theirs.longLength > 94;
    a.sendLength = a.longPackets ? std::min(theirs.longLength, kMaxPacket)
                                 : std::clamp(theirs.maxLength, 10, 94);
    a.attributes = ours.attributes && theirs.attributes;
    a.ctlPrefixOut = ours.ctlPrefix;
    a.ctlPrefixIn = theirs.ctlPrefix;
    if (ours.binPrefix == 'Y' && validPrefix(theirs.binPrefix)) {
        a.binQuoting = true;
        a.binPrefix = theirs.binPrefix;
    } else if (validPrefix(ours.binPrefix)
               && (theirs.binPrefix == 'Y' || theirs.binPrefix == ours.binPrefix)) {
        a.binQuoting = true;
        a.binPrefix = ours.binPrefix;
    }
    a.repeating = validPrefix(theirs.repeatPrefix) && theirs.repeatPrefix == ours.repeatPrefix;
    a.repeatPrefix = ours.repeatPrefix;
    a.eol = theirs.eol;
    a.padCount = theirs.padCount;
    a.padChar = theirs.padChar;
    a.timeoutMs = std::clamp(theirs.timeout, 2, 60) * 1000;
    return a;
}

QByteArray blockCheck(QByteArrayView body, int type)
{
    QByteArray out;
    switch (type) {
    case 2: {
        const unsigned s = sum(body) & 07777;
        out += tochar(int((s >> 6) & 077));
        out += tochar(int(s & 077));
        break;
    }
    case 3: {
        const quint16 crc = crcKermit(body);
        out += tochar((crc >> 12) & 017);
        out += tochar((crc >> 6) & 077);
        out += tochar(crc & 077);
        break;
    }
    default:
        out += check1(body);
        break;
    }
    return out;
}

QByteArray encodePacket(char type, int sequence, QByteArrayView data, int checkType,
                        const Agreed &agreed)
{
    QByteArray out(agreed.padCount, agreed.padChar);
    out += '\x01';
    QByteArray body;
    const int total = int(data.size()) + checkType;
    if (total + 2 > 94) {
        body += tochar(0);
        body += tochar(sequence % 64);
        body += type;
        body += tochar(total / 95);
        body += tochar(total % 95);
        body += check1(body);
    } else {
        body += tochar(total + 2);
        body += tochar(sequence % 64);
        body += type;
    }
    body += data;
    out += body;
    out += blockCheck(body, checkType);
    out += agreed.eol;
    return out;
}

namespace {

QByteArray encodeByte(quint8 byte, const Agreed &agreed)
{
    QByteArray out;
    quint8 a = byte;
    if (agreed.binQuoting && (a & 0x80)) {
        out += agreed.binPrefix;
        a &= 0x7F;
    }
    const quint8 low = a & 0x7F;
    if (low < 32 || low == 127) {
        out += agreed.ctlPrefixOut;
        a ^= 64;
    } else if (low == quint8(agreed.ctlPrefixOut)
               || (agreed.binQuoting && low == quint8(agreed.binPrefix))
               || (agreed.repeating && low == quint8(agreed.repeatPrefix))) {
        out += agreed.ctlPrefixOut;
    }
    out += char(a);
    return out;
}

} // namespace

QByteArray encodeData(QByteArrayView input, int capacity, const Agreed &agreed,
                      qsizetype *consumed)
{
    QByteArray out;
    qsizetype i = 0;
    while (i < input.size()) {
        const quint8 byte = quint8(input[i]);
        qsizetype run = 1;
        if (agreed.repeating) {
            while (i + run < input.size() && quint8(input[i + run]) == byte && run < 94)
                ++run;
        }
        const QByteArray piece = encodeByte(byte, agreed);
        QByteArray chunk;
        qsizetype take = 1;
        if (agreed.repeating && run > 2) {
            chunk += agreed.repeatPrefix;
            chunk += tochar(int(run));
            chunk += piece;
            take = run;
        } else {
            chunk = piece;
        }
        if (out.size() + chunk.size() > capacity)
            break;
        out += chunk;
        i += take;
    }
    if (consumed != nullptr)
        *consumed = i;
    return out;
}

std::optional<QByteArray> decodeData(QByteArrayView data, const Agreed &agreed)
{
    QByteArray out;
    qsizetype i = 0;
    const qsizetype n = data.size();
    while (i < n) {
        char a = data[i++];
        int repeat = 1;
        if (agreed.repeating && a == agreed.repeatPrefix) {
            if (i + 1 >= n)
                return std::nullopt;
            repeat = unchar(data[i++]);
            a = data[i++];
        }
        quint8 high = 0;
        if (agreed.binQuoting && a == agreed.binPrefix) {
            if (i >= n)
                return std::nullopt;
            high = 0x80;
            a = data[i++];
        }
        if (a == agreed.ctlPrefixIn) {
            if (i >= n)
                return std::nullopt;
            a = data[i++];
            const int low = quint8(a) & 0x7F;
            if (low > 62 && low < 96)
                a = ctl(a);
        }
        out.append(std::max(1, repeat), char(quint8(a) | high));
    }
    return out;
}

} // namespace kermit

namespace {

using namespace kermit;

constexpr char MARK = 0x01;
constexpr int kByteTimeoutMs = 5000;
// How often a sender repeats its Send-Init, and a receiver its NAK, while
// waiting for the other side to start.
constexpr int kStartIntervalMs = 5000;
// File data read per packet loop.
constexpr qsizetype kReadAhead = 16384;

struct Packet
{
    char type = 0;
    int sequence = 0;
    QByteArray data;
};

enum class ReadStatus { Ok, Timeout, Bad };

class Engine
{
public:
    explicit Engine(Job &job)
        : m_job(job)
    {
        m_ours.timeout = 10;
    }

    void sendFiles(const std::vector<std::unique_ptr<OutgoingFile>> &files);
    void receiveFiles();
    // Best effort: tell the other Kermit why we are stopping.
    void sendError(const QString &message);

private:
    ReadStatus readPacket(int timeoutMs, Packet &packet);
    void send(char type, int sequence, QByteArrayView data, int checkType);
    void send(char type, QByteArrayView data = {}) { send(type, m_sequence, data, m_check); }
    Packet exchange(char type, QByteArrayView data, int attempts = 0, int intervalMs = 0);
    [[noreturn]] void failOnError(const Packet &packet);
    QByteArray encodeText(const QString &text) const;
    QByteArray attributes(const OutgoingFile &file) const;
    void parseAttributes(const QByteArray &data, qint64 *size, QDateTime *modified) const;
    void sendFile(OutgoingFile &file, int index, int count);

    Job &m_job;
    InitParams m_ours;
    Agreed m_agreed;
    int m_check = 1;
    int m_sequence = 0;
};

ReadStatus Engine::readPacket(int timeoutMs, Packet &packet)
{
    Link &link = m_job.link;
    QDeadlineTimer deadline(timeoutMs);
    int c = -1;
    do {
        c = link.getByte(int(qMax<qint64>(0, deadline.remainingTime())));
        if (c < 0)
            return ReadStatus::Timeout;
    } while (c != MARK);

    for (;;) {
        bool restart = false;
        QByteArray body;
        const auto next = [&](int &out) {
            out = link.getByte(kByteTimeoutMs);
            if (out == MARK)
                restart = true;
            return out >= 0 && !restart;
        };
        int len = 0, seq = 0, type = 0;
        if (!next(len) || !next(seq) || !next(type)) {
            if (restart)
                continue;
            return ReadStatus::Bad;
        }
        body += char(len);
        body += char(seq);
        body += char(type);
        int total = 0;
        if (unchar(char(len)) == 0) {
            int x1 = 0, x2 = 0, hcheck = 0;
            if (!next(x1) || !next(x2) || !next(hcheck)) {
                if (restart)
                    continue;
                return ReadStatus::Bad;
            }
            body += char(x1);
            body += char(x2);
            if (char(hcheck) != check1(body))
                return ReadStatus::Bad;
            body += char(hcheck);
            total = unchar(char(x1)) * 95 + unchar(char(x2));
        } else {
            total = unchar(char(len)) - 2;
        }
        if (total < 1 || total > kMaxPacket + 3)
            return ReadStatus::Bad;
        QByteArray rest;
        rest.reserve(total);
        for (int i = 0; i < total; ++i) {
            int byte = 0;
            if (!next(byte))
                break;
            rest += char(byte);
        }
        if (restart)
            continue;
        if (rest.size() != total)
            return ReadStatus::Bad;

        // The packet's check type is the agreed one — except for the packets
        // that may cross the switch to it (a repeated S, an error).
        const int types[] = {m_check, 1, 2, 3};
        const bool mayDiffer = type == 'S' || type == 'I' || type == 'E';
        for (const int checkType : types) {
            if (checkType > total || (checkType != m_check && !mayDiffer))
                continue;
            const QByteArray data = rest.left(total - checkType);
            if (blockCheck(body + data, checkType) == rest.mid(total - checkType)) {
                packet.type = char(type);
                packet.sequence = unchar(char(seq));
                packet.data = data;
                return ReadStatus::Ok;
            }
        }
        return ReadStatus::Bad;
    }
}

void Engine::send(char type, int sequence, QByteArrayView data, int checkType)
{
    m_job.link.put(encodePacket(type, sequence, data, checkType, m_agreed));
}

void Engine::failOnError(const Packet &packet)
{
    const QString message = QString::fromLatin1(decodeData(packet.data, m_agreed).value_or(packet.data));
    fail(QStringLiteral("the other Kermit stopped the transfer: %1").arg(message.trimmed()));
}

void Engine::sendError(const QString &message)
{
    try {
        send('E', encodeText(message));
    } catch (const PortsideError &) {
    }
}

// Send a packet and wait for its ACK, resending on NAK, silence or damage.
Packet Engine::exchange(char type, QByteArrayView data, int attempts, int intervalMs)
{
    if (attempts <= 0)
        attempts = m_job.options.maxRetries + 1;
    if (intervalMs <= 0)
        intervalMs = m_agreed.timeoutMs;
    const int sequence = m_sequence;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (attempt > 0)
            m_job.retry();
        send(type, sequence, data, type == 'S' ? 1 : m_check);
        QDeadlineTimer deadline(intervalMs);
        for (;;) {
            Packet reply;
            const ReadStatus status =
                readPacket(int(qMax<qint64>(1, deadline.remainingTime())), reply);
            if (status != ReadStatus::Ok)
                break;
            if (reply.type == 'E')
                failOnError(reply);
            if (reply.type == 'Y' && reply.sequence == sequence)
                return reply;
            // A NAK for the next packet acknowledges this one.
            if (reply.type == 'N' && reply.sequence == (sequence + 1) % 64)
                return Packet{'Y', sequence, {}};
            if (reply.type == 'N')
                break;
            // An old ACK: keep waiting for ours.
        }
    }
    fail(QStringLiteral("no acknowledgement for packet %1 after %2 tries").arg(sequence).arg(attempts),
         QStringLiteral("Check that Kermit on the device is receiving (for example kermit -r or "
                        "gkermit -r)."));
}

QByteArray Engine::encodeText(const QString &text) const
{
    const QByteArray raw = text.toUtf8();
    qsizetype consumed = 0;
    return encodeData(raw, std::max(20, m_agreed.dataCapacity()), m_agreed, &consumed);
}

QByteArray Engine::attributes(const OutgoingFile &file) const
{
    QByteArray a;
    // File type: binary, 8-bit.
    a += '"';
    a += tochar(2);
    a += "B8";
    const QByteArray size = QByteArray::number(file.size());
    a += '1';
    a += tochar(int(size.size()));
    a += size;
    if (file.modified().isValid()) {
        const QByteArray date =
            file.modified().toString(QStringLiteral("yyyyMMdd HH:mm:ss")).toLatin1();
        a += '#';
        a += tochar(int(date.size()));
        a += date;
    }
    return a;
}

void Engine::parseAttributes(const QByteArray &data, qint64 *size, QDateTime *modified) const
{
    qsizetype i = 0;
    while (i + 1 < data.size()) {
        const char tag = data[i];
        const int length = unchar(data[i + 1]);
        i += 2;
        if (length < 0 || i + length > data.size())
            return;
        const QByteArray value = data.mid(i, length);
        i += length;
        bool ok = false;
        if (tag == '1') {
            const qint64 bytes = value.toLongLong(&ok);
            if (ok)
                *size = bytes;
        } else if (tag == '!' && *size < 0) {
            const qint64 kilobytes = value.toLongLong(&ok);
            if (ok)
                *size = kilobytes * 1024;
        } else if (tag == '#') {
            QDateTime when = QDateTime::fromString(QString::fromLatin1(value),
                                                   QStringLiteral("yyyyMMdd HH:mm:ss"));
            if (!when.isValid())
                when = QDateTime::fromString(QString::fromLatin1(value), QStringLiteral("yyyyMMdd"));
            if (when.isValid())
                *modified = when;
        }
    }
}

// -- sending ------------------------------------------------------------------

void Engine::sendFiles(const std::vector<std::unique_ptr<OutgoingFile>> &files)
{
    m_sequence = 0;
    const int startMs = int(m_job.options.startTimeout * 1000.0);
    const Packet ack = exchange('S', encodeInit(m_ours),
                                std::max(1, startMs / kStartIntervalMs), kStartIntervalMs);
    m_agreed = agree(m_ours, decodeInit(ack.data));
    m_check = m_agreed.checkType;
    m_job.result.check = QStringLiteral("type %1 block check").arg(m_check);

    const int count = int(files.size());
    for (int index = 0; index < count; ++index)
        sendFile(*files[std::size_t(index)], index + 1, count);
    m_sequence = (m_sequence + 1) % 64;
    exchange('B', {});
}

void Engine::sendFile(OutgoingFile &file, int index, int count)
{
    m_job.beginFile(file.name(), file.size(), index, count);
    m_sequence = (m_sequence + 1) % 64;
    exchange('F', encodeText(file.name()));
    if (m_agreed.attributes) {
        m_sequence = (m_sequence + 1) % 64;
        const Packet reply = exchange('A', attributes(file));
        if (reply.data.startsWith('N')) {
            m_sequence = (m_sequence + 1) % 64;
            exchange('Z', "D");
            m_job.endFile(file.path(), 0, true, QStringLiteral("the receiver refused it"));
            return;
        }
    }
    const int capacity = m_agreed.dataCapacity();
    QByteArray pending;
    qint64 sent = 0;
    bool interrupted = false;
    while (!interrupted) {
        if (pending.size() < kReadAhead && !file.atEnd())
            pending += file.read(kReadAhead);
        if (pending.isEmpty())
            break;
        qsizetype consumed = 0;
        const QByteArray encoded = encodeData(pending, capacity, m_agreed, &consumed);
        pending.remove(0, consumed);
        m_sequence = (m_sequence + 1) % 64;
        const Packet reply = exchange('D', encoded);
        sent += consumed;
        m_job.advance(consumed);
        // The receiver can stop a file ("X") or the whole batch ("Z").
        if (reply.data.startsWith('X') || reply.data.startsWith('Z'))
            interrupted = true;
    }
    m_sequence = (m_sequence + 1) % 64;
    exchange('Z', interrupted ? QByteArrayView("D") : QByteArrayView());
    if (interrupted)
        m_job.endFile(file.path(), sent, true, QStringLiteral("the receiver stopped it"));
    else
        m_job.endFile(file.path(), file.size());
}

// -- receiving ------------------------------------------------------------------

void Engine::receiveFiles()
{
    ReceiveDirectory directory(m_job.options);
    const int startMs = int(m_job.options.startTimeout * 1000.0);
    QDeadlineTimer startDeadline(startMs);

    // Wait for Send-Init, nudging the sender with a NAK now and then.
    Packet packet;
    for (;;) {
        const ReadStatus status = readPacket(
            int(qMin<qint64>(kStartIntervalMs, qMax<qint64>(1, startDeadline.remainingTime()))),
            packet);
        if (status == ReadStatus::Ok && packet.type == 'S')
            break;
        if (status == ReadStatus::Ok && packet.type == 'E')
            failOnError(packet);
        if (startDeadline.hasExpired())
            fail(QStringLiteral("no Kermit sender started within %1 s").arg(startMs / 1000),
                 QStringLiteral("Start the send on the device (for example kermit -s file or "
                                "gkermit -s file), then receive."));
        // Nudge on silence only: answering a packet (our own NAK, echoed by
        // the line) would loop.
        if (status != ReadStatus::Ok)
            send('N', 0, {}, 1);
    }

    const InitParams theirs = decodeInit(packet.data);
    // Agree to the sender's block check and repeat prefix when we can.
    m_ours.checkType = theirs.checkType;
    if (validPrefix(theirs.repeatPrefix))
        m_ours.repeatPrefix = theirs.repeatPrefix;
    m_agreed = agree(m_ours, theirs);
    m_sequence = packet.sequence;
    send('Y', m_sequence, encodeInit(m_ours), 1);
    QByteArray lastReply = encodePacket('Y', m_sequence, encodeInit(m_ours), 1, m_agreed);
    m_check = m_agreed.checkType;
    m_job.result.check = QStringLiteral("type %1 block check").arg(m_check);

    std::unique_ptr<IncomingFile> file;
    QString fileName;
    QString skipReason;
    bool skipping = false;
    qint64 size = -1;
    QDateTime modified;
    int index = 0;
    int errors = 0;

    const auto reply = [&](QByteArrayView data = {}) {
        lastReply = encodePacket('Y', m_sequence, data, m_check, m_agreed);
        m_job.link.put(lastReply);
    };

    for (;;) {
        const int expected = (m_sequence + 1) % 64;
        const ReadStatus status = readPacket(m_agreed.timeoutMs, packet);
        if (status != ReadStatus::Ok) {
            m_job.retry();
            if (++errors > m_job.options.maxRetries)
                fail(status == ReadStatus::Timeout
                         ? QStringLiteral("the Kermit sender went quiet")
                         : QStringLiteral("too many damaged packets in a row"),
                     QStringLiteral("Lower the baud rate or enable flow control, then try again."));
            send('N', expected, {}, m_check);
            continue;
        }
        if (packet.type == 'E')
            failOnError(packet);
        if (packet.sequence == m_sequence) {
            // Our reply was lost; say it again.
            m_job.link.put(lastReply);
            continue;
        }
        if (packet.sequence != expected) {
            send('N', expected, {}, m_check);
            continue;
        }
        errors = 0;
        m_sequence = expected;
        switch (packet.type) {
        case 'F': {
            fileName = QString::fromUtf8(decodeData(packet.data, m_agreed).value_or(packet.data));
            size = -1;
            modified = QDateTime();
            skipReason.clear();
            file = directory.open(fileName, &skipReason);
            skipping = !file;
            m_job.beginFile(ReceiveDirectory::safeName(fileName), -1, ++index, 0);
            reply();
            break;
        }
        case 'A':
            parseAttributes(packet.data, &size, &modified);
            if (size >= 0)
                m_job.beginFile(ReceiveDirectory::safeName(fileName), size, index, 0);
            reply(skipping ? QByteArrayView("N") : QByteArrayView());
            break;
        case 'D': {
            if (skipping) {
                reply("X");
                break;
            }
            const std::optional<QByteArray> data = decodeData(packet.data, m_agreed);
            if (!data) {
                // Undecodable but intact by its check: a sender bug.
                fail(QStringLiteral("a data packet could not be decoded"));
            }
            if (file)
                file->write(*data);
            m_job.advance(data->size());
            reply();
            break;
        }
        case 'Z': {
            const bool discard = decodeData(packet.data, m_agreed).value_or(QByteArray()).startsWith('D');
            if (skipping) {
                m_job.endFile(QString(), 0, true, skipReason);
            } else if (file && discard) {
                const qint64 bytes = file->size();
                file->abandon();
                m_job.endFile(QString(), bytes, true, QStringLiteral("the sender stopped it"));
            } else if (file) {
                const qint64 bytes = file->size();
                const QString path = file->commit(modified);
                m_job.endFile(path, bytes);
            }
            file.reset();
            skipping = false;
            reply();
            break;
        }
        case 'B':
            reply();
            return;
        case 'S':
            // A new Send-Init in the middle: answer it again.
            send('Y', m_sequence, encodeInit(m_ours), 1);
            break;
        default:
            sendError(QStringLiteral("Portside cannot handle a '%1' packet").arg(QChar::fromLatin1(packet.type)));
            fail(QStringLiteral("the sender used a Kermit feature Portside does not support "
                                "(packet type '%1')")
                     .arg(QChar::fromLatin1(packet.type)));
        }
    }
}

template <typename Body>
void guarded(Engine &engine, Body &&body)
{
    try {
        body();
    } catch (const PortsideError &error) {
        if (!error.isTransportError())
            engine.sendError(error.kind() == ErrorKind::Cancelled
                                 ? QStringLiteral("cancelled by the user")
                                 : error.message());
        throw;
    }
}

} // namespace

void sendKermit(Job &job, const std::vector<std::unique_ptr<OutgoingFile>> &files)
{
    Engine engine(job);
    guarded(engine, [&] { engine.sendFiles(files); });
}

void receiveKermit(Job &job)
{
    Engine engine(job);
    guarded(engine, [&] { engine.receiveFiles(); });
}

} // namespace portside::transfer
