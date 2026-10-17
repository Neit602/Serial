// ZMODEM send and receive (PRD F18), in-house per the PRD stack table and
// written against lrzsz's behaviour.
//
// What is deliberately missing: ZCOMMAND (a remote asking us to run a shell
// command — refused, DECISIONS D54), sender-side crash recovery and the
// compression/encryption options nobody implements.

#include "core/transfer/zmodem.h"

#include "core/transfer/crc.h"
#include "core/transfer/engine.h"

#include <QDeadlineTimer>
#include <QStorageInfo>

namespace portside::transfer {

namespace zmodem {

namespace {

constexpr char ZPAD = '*';
constexpr char ZDLE = 0x18;
constexpr char ZBIN = 'A';
constexpr char ZHEX = 'B';
constexpr char ZBIN32 = 'C';
constexpr char ZRUB0 = 'l';
constexpr char ZRUB1 = 'm';
constexpr char XON = 0x11;
constexpr char XOFF = 0x13;
constexpr char CAN = 0x18;

void putHex(QByteArray &out, quint8 byte)
{
    static constexpr char digits[] = "0123456789abcdef";
    out += digits[byte >> 4];
    out += digits[byte & 0x0F];
}

} // namespace

quint32 Header::position() const
{
    return quint32(data[0]) | quint32(data[1]) << 8 | quint32(data[2]) << 16
           | quint32(data[3]) << 24;
}

Header Header::at(quint8 type, quint32 position)
{
    Header header;
    header.type = type;
    header.data = {quint8(position), quint8(position >> 8), quint8(position >> 16),
                   quint8(position >> 24)};
    return header;
}

Header Header::flags(quint8 type, quint8 f0, quint8 f1, quint8 f2, quint8 f3)
{
    Header header;
    header.type = type;
    header.data = {f3, f2, f1, f0};
    return header;
}

void Escaper::put(QByteArray &out, quint8 byte)
{
    bool escape = false;
    if ((byte & 0x60) == 0) {
        switch (byte) {
        case ZDLE:
        case 0x10:
        case 0x90:
        case XON:
        case XON | 0x80:
        case XOFF:
        case XOFF | 0x80:
            escape = true;
            break;
        case 0x0D:
        case 0x8D:
            // "@<CR>" is Telenet's escape; break it up.
            escape = m_escapeControls || (m_last & 0x7F) == '@';
            break;
        default:
            escape = m_escapeControls;
            break;
        }
    }
    if (escape) {
        out += ZDLE;
        byte ^= 0x40;
    }
    out += char(byte);
    m_last = byte;
}

void Escaper::put(QByteArray &out, QByteArrayView bytes)
{
    for (const char byte : bytes)
        put(out, quint8(byte));
}

QByteArray encodeHexHeader(const Header &header)
{
    QByteArray body;
    body += char(header.type);
    for (const quint8 byte : header.data)
        body += char(byte);
    const quint16 crc = crc16(body);
    QByteArray out("**\x18" "B");
    for (const char byte : body)
        putHex(out, quint8(byte));
    putHex(out, quint8(crc >> 8));
    putHex(out, quint8(crc));
    out += "\r\x8a";
    // Un-stick a sender that a stray XOFF stopped.
    if (header.type != ZFIN && header.type != ZACK)
        out += XON;
    return out;
}

QByteArray encodeBinaryHeader(const Header &header, bool crc32Frame, Escaper &escaper)
{
    QByteArray body;
    body += char(header.type);
    for (const quint8 byte : header.data)
        body += char(byte);
    QByteArray out;
    out += ZPAD;
    out += ZDLE;
    out += crc32Frame ? ZBIN32 : ZBIN;
    escaper.put(out, body);
    if (crc32Frame) {
        const quint32 crc = crc32(body);
        for (int shift = 0; shift < 32; shift += 8)
            escaper.put(out, quint8(crc >> shift));
    } else {
        const quint16 crc = crc16(body);
        escaper.put(out, quint8(crc >> 8));
        escaper.put(out, quint8(crc));
    }
    return out;
}

QByteArray encodeSubpacket(QByteArrayView data, char end, bool crc32Frame, Escaper &escaper)
{
    QByteArray out;
    out.reserve(data.size() + data.size() / 16 + 12);
    escaper.put(out, data);
    out += ZDLE;
    out += end;
    if (crc32Frame) {
        const quint32 crc = crc32(QByteArrayView(&end, 1), crc32(data));
        for (int shift = 0; shift < 32; shift += 8)
            escaper.put(out, quint8(crc >> shift));
    } else {
        const quint16 crc = crc16(QByteArrayView(&end, 1), crc16(data));
        escaper.put(out, quint8(crc >> 8));
        escaper.put(out, quint8(crc));
    }
    if (end == ZCRCW)
        out += XON;
    return out;
}

StartDetector::Event StartDetector::feed(QByteArrayView data)
{
    static constexpr char prefix[] = "**\x18" "B0";
    constexpr int prefixLength = 5;
    Event event = Event::None;
    for (const char byte : data) {
        if (m_matched == prefixLength) {
            m_matched = 0;
            if (byte == '0') {
                event = Event::SenderReady;
                continue;
            }
            if (byte == '1') {
                event = Event::ReceiverReady;
                continue;
            }
        }
        if (byte == prefix[m_matched]) {
            ++m_matched;
        } else if (byte == ZPAD) {
            // "***" still ends in a valid start.
            m_matched = m_matched == 2 ? 2 : 1;
        } else {
            m_matched = 0;
        }
    }
    return event;
}

} // namespace zmodem

namespace {

using namespace zmodem;

constexpr int kHeaderTimeoutMs = 10000;
constexpr int kByteTimeoutMs = 10000;
constexpr int kSubpacketSize = 1024;
constexpr int kMaxSubpacket = 16384;

// Reader return codes besides frame types and data bytes.
constexpr int kTimeout = -1;
constexpr int kError = -2;
constexpr int kCancelled = -3;
// zdlRead() marks a frame end as kGotEnd | terminator.
constexpr int kGotEnd = 0x100;

class Reader
{
public:
    explicit Reader(Link &link)
        : m_link(link)
    {
    }

    bool lastFrameCrc32() const { return m_crc32; }
    void setEscapeControls(bool escape) { m_escapeControls = escape; }

    // A frame type (>= 0), or kTimeout, kError (bad CRC, garbage) or
    // kCancelled (five CANs).  Skips anything before the frame.
    int readHeader(Header &header, int timeoutMs)
    {
        QDeadlineTimer deadline(timeoutMs);
        int cancels = 0;
        for (;;) {
            const int c = m_link.getByte(remaining(deadline));
            if (c < 0)
                return kTimeout;
            if (c == CAN) {
                if (++cancels >= 5)
                    return kCancelled;
                continue;
            }
            if ((c & 0x7F) != ZPAD) {
                cancels = 0;
                continue;
            }
            int next = readStripped(deadline);
            while (next == ZPAD)
                next = readStripped(deadline);
            if (next < 0)
                return kTimeout;
            if (next != ZDLE)
                continue;
            next = readStripped(deadline);
            int type = kError;
            switch (next) {
            case ZBIN:
                m_crc32 = false;
                type = readBinary(header, false);
                break;
            case ZBIN32:
                m_crc32 = true;
                type = readBinary(header, true);
                break;
            case ZHEX:
                m_crc32 = false;
                type = readHex(header);
                break;
            case kTimeout:
                return kTimeout;
            default:
                continue;
            }
            return type;
        }
    }

    // Read one data subpacket into `out`.  Returns its terminator (ZCRCE..W)
    // or kTimeout, kError, kCancelled.
    int readSubpacket(QByteArray &out)
    {
        out.clear();
        for (;;) {
            const int c = zdlRead();
            if (c < 0)
                return c;
            if (c & kGotEnd) {
                const char end = char(c & 0xFF);
                const int checkLength = m_crc32 ? 4 : 2;
                QByteArray check;
                for (int i = 0; i < checkLength; ++i) {
                    const int byte = zdlRead();
                    if (byte < 0 || (byte & kGotEnd))
                        return byte < 0 ? byte : kError;
                    check += char(byte);
                }
                if (m_crc32) {
                    const quint32 got = quint32(quint8(check[0])) | quint32(quint8(check[1])) << 8
                                        | quint32(quint8(check[2])) << 16
                                        | quint32(quint8(check[3])) << 24;
                    if (crc32(QByteArrayView(&end, 1), crc32(out)) != got)
                        return kError;
                } else {
                    const quint16 got = quint16(quint8(check[0]) << 8 | quint8(check[1]));
                    if (crc16(QByteArrayView(&end, 1), crc16(out)) != got)
                        return kError;
                }
                return end;
            }
            if (out.size() >= kMaxSubpacket)
                return kError;
            out += char(c);
        }
    }

private:
    static int remaining(const QDeadlineTimer &deadline)
    {
        return int(qMax<qint64>(0, deadline.remainingTime()));
    }

    // A header byte: parity stripped, XON/XOFF ignored.
    int readStripped(const QDeadlineTimer &deadline)
    {
        for (;;) {
            const int c = m_link.getByte(qMin(remaining(deadline) + 1000, kByteTimeoutMs));
            if (c < 0)
                return kTimeout;
            const int stripped = c & 0x7F;
            if (stripped == XON || stripped == XOFF)
                continue;
            if (m_escapeControls && (stripped & 0x60) == 0 && stripped != ZDLE
                && stripped != '\r' && stripped != '\n')
                continue;
            return stripped;
        }
    }

    // A byte of a binary header or subpacket, with ZDLE escapes undone.
    int zdlRead()
    {
        for (;;) {
            int c = m_link.getByte(kByteTimeoutMs);
            if (c < 0)
                return kTimeout;
            if (c & 0x60)
                return c;
            if (c == XON || c == (XON | 0x80) || c == XOFF || c == (XOFF | 0x80))
                continue;
            if (c != ZDLE) {
                if (m_escapeControls)
                    continue;
                return c;
            }
            // After ZDLE.
            for (;;) {
                c = m_link.getByte(kByteTimeoutMs);
                if (c < 0)
                    return kTimeout;
                if (c == CAN) {
                    int cans = 2;
                    while (c == CAN && cans < 5) {
                        c = m_link.getByte(kByteTimeoutMs);
                        if (c < 0)
                            return kTimeout;
                        if (c == CAN)
                            ++cans;
                    }
                    if (cans >= 5)
                        return kCancelled;
                }
                switch (c) {
                case ZCRCE:
                case ZCRCG:
                case ZCRCQ:
                case ZCRCW:
                    return kGotEnd | c;
                case ZRUB0:
                    return 0x7F;
                case ZRUB1:
                    return 0xFF;
                case XON:
                case XON | 0x80:
                case XOFF:
                case XOFF | 0x80:
                    continue;
                default:
                    break;
                }
                if (m_escapeControls && (c & 0x60) == 0)
                    continue;
                if ((c & 0x60) == 0x40)
                    return c ^ 0x40;
                return kError;
            }
        }
    }

    int readBinary(Header &header, bool crc32Frame)
    {
        QByteArray body;
        const int length = 5 + (crc32Frame ? 4 : 2);
        for (int i = 0; i < length; ++i) {
            const int c = zdlRead();
            if (c < 0)
                return c;
            if (c & kGotEnd)
                return kError;
            body += char(c);
        }
        const QByteArrayView frame(body.constData(), 5);
        if (crc32Frame) {
            const quint32 got = quint32(quint8(body[5])) | quint32(quint8(body[6])) << 8
                                | quint32(quint8(body[7])) << 16 | quint32(quint8(body[8])) << 24;
            if (crc32(frame) != got)
                return kError;
        } else if (crc16(frame) != quint16(quint8(body[5]) << 8 | quint8(body[6]))) {
            return kError;
        }
        return unpack(header, body);
    }

    int readHex(Header &header)
    {
        QByteArray body;
        const QDeadlineTimer deadline(kByteTimeoutMs);
        for (int i = 0; i < 7; ++i) {
            int value = 0;
            for (int digit = 0; digit < 2; ++digit) {
                const int c = readStripped(deadline);
                if (c < 0)
                    return kTimeout;
                int nibble = -1;
                if (c >= '0' && c <= '9')
                    nibble = c - '0';
                else if (c >= 'a' && c <= 'f')
                    nibble = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F')
                    nibble = c - 'A' + 10;
                if (nibble < 0)
                    return kError;
                value = value << 4 | nibble;
            }
            body += char(value);
        }
        if (crc16(QByteArrayView(body.constData(), 5))
            != quint16(quint8(body[5]) << 8 | quint8(body[6])))
            return kError;
        // Swallow the CR LF after a hex header, if it is there.
        for (int i = 0; i < 2; ++i) {
            const int c = m_link.getByte(500);
            if (c < 0)
                break;
            if ((c & 0x7F) != '\r' && (c & 0x7F) != '\n') {
                m_link.unget(quint8(c));
                break;
            }
        }
        return unpack(header, body);
    }

    static int unpack(Header &header, const QByteArray &body)
    {
        header.type = quint8(body[0]);
        for (int i = 0; i < 4; ++i)
            header.data[std::size_t(i)] = quint8(body[i + 1]);
        return header.type;
    }

    Link &m_link;
    bool m_crc32 = false;
    bool m_escapeControls = false;
};

class Engine
{
public:
    explicit Engine(Job &job)
        : m_job(job)
        , m_reader(job.link)
    {
    }

    void sendFiles(const std::vector<std::unique_ptr<OutgoingFile>> &files);
    void receiveFiles();

private:
    void putHex(const Header &header) { m_job.link.put(encodeHexHeader(header)); }
    void putBinary(const Header &header)
    {
        m_job.link.put(encodeBinaryHeader(header, m_crc32, m_escaper));
    }
    void putSubpacket(QByteArrayView data, char end)
    {
        m_job.link.put(encodeSubpacket(data, end, m_crc32, m_escaper));
    }

    void countError(const QString &what)
    {
        m_job.retry();
        if (++m_errors > m_job.options.maxRetries)
            fail(QStringLiteral("%1 (%2 errors in a row)").arg(what).arg(m_errors),
                 QStringLiteral("Lower the baud rate or enable flow control, then try again."));
    }

    // Sending.
    bool headerFollows();
    void awaitReceiver();
    bool sendOne(OutgoingFile &file, int index, int count, qint64 bytesLeft);
    enum class DataOutcome { Done, Skipped };
    DataOutcome sendData(OutgoingFile &file, qint64 start);
    bool reposition(OutgoingFile &file, qint64 &position, quint32 requested);
    void finishSession();

    // Receiving.
    void sendReceiverInit();
    // False when the file was declined (ZSKIP).
    bool receiveOne(const Header &header, const QByteArray &info, ReceiveDirectory &directory,
                    int index);
    void receiveData(IncomingFile &file, qint64 size);
    [[noreturn]] void refuseCommand();

    Job &m_job;
    Reader m_reader;
    Escaper m_escaper;
    bool m_crc32 = false;
    quint32 m_receiverBuffer = 0;
    int m_errors = 0;
    QByteArray m_lastSkipped;
};

[[noreturn]] void failOnPeer(int type)
{
    if (type == ZFERR)
        fail(QStringLiteral("the receiver could not write the file"),
             QStringLiteral("Check the free space and permissions on the receiving side."));
    failCancelledByPeer();
}

bool isPeerAbort(int type)
{
    return type == kCancelled || type == ZCAN || type == ZABORT || type == ZFERR;
}

// -- sending ------------------------------------------------------------------

bool Engine::headerFollows()
{
    QDeadlineTimer quiet(2000);
    while (!quiet.hasExpired()) {
        const int c = m_job.link.getByte(int(qMax<qint64>(1, quiet.remainingTime())));
        if (c < 0)
            return false;
        if ((c & 0x7F) == ZPAD || c == CAN) {
            m_job.link.unget(quint8(c));
            return true;
        }
    }
    return false;
}

void Engine::awaitReceiver()
{
    // "rz\r" starts the receiver on a device sitting at a shell prompt.
    m_job.link.put(QByteArrayLiteral("rz\r"));
    putHex(Header::at(ZRQINIT, 0));
    const int startMs = int(m_job.options.startTimeout * 1000.0);
    QDeadlineTimer deadline(startMs);
    Header header;
    for (;;) {
        const int type = m_reader.readHeader(
            header, int(qMin<qint64>(kHeaderTimeoutMs, qMax<qint64>(1, deadline.remainingTime()))));
        if (type == ZRINIT)
            break;
        if (isPeerAbort(type))
            failOnPeer(type);
        if (type == ZCHALLENGE) {
            Header reply = header;
            reply.type = ZACK;
            putHex(reply);
            continue;
        }
        if (deadline.hasExpired())
            fail(QStringLiteral("no ZMODEM receiver answered within %1 s").arg(startMs / 1000),
                 QStringLiteral("Start rz on the device (or check that it supports ZMODEM), then "
                                "send."));
        if (type == kTimeout || type == kError || type == ZNAK)
            putHex(Header::at(ZRQINIT, 0));
        // Anything else, our own ZRQINIT echoed back included, is ignored.
    }
    const quint8 capabilities = header.flag(0);
    m_crc32 = (capabilities & CANFC32) != 0;
    m_escaper.setEscapeControls((capabilities & ESCCTL) != 0);
    m_receiverBuffer = quint32(header.data[0]) | quint32(header.data[1]) << 8;
    m_job.result.check = m_crc32 ? QStringLiteral("CRC-32") : QStringLiteral("CRC-16");
}

void Engine::sendFiles(const std::vector<std::unique_ptr<OutgoingFile>> &files)
{
    awaitReceiver();
    qint64 bytesLeft = 0;
    for (const auto &file : files)
        bytesLeft += file->size();
    const int count = int(files.size());
    for (int index = 0; index < count; ++index) {
        OutgoingFile &file = *files[std::size_t(index)];
        sendOne(file, index + 1, count, bytesLeft);
        bytesLeft -= file.size();
    }
    finishSession();
}

bool Engine::sendOne(OutgoingFile &file, int index, int count, qint64 bytesLeft)
{
    m_job.beginFile(file.name(), file.size(), index, count);
    QByteArray info = file.name().toUtf8();
    info += '\0';
    info += QByteArray::number(file.size());
    info += ' ';
    info += QByteArray::number(file.modified().isValid() ? file.modified().toSecsSinceEpoch() : 0, 8);
    info += " 100644 0 ";
    info += QByteArray::number(count - index + 1);
    info += ' ';
    info += QByteArray::number(bytesLeft);
    info += '\0';

    m_errors = 0;
    Header header;
    for (;;) {
        // ZF0 = 1: binary, no conversion.
        putBinary(Header::flags(ZFILE, 1));
        putSubpacket(info, ZCRCW);
        int type = kTimeout;
        for (;;) {
            type = m_reader.readHeader(header, kHeaderTimeoutMs);
            if (type == ZCRC) {
                // The receiver wants the file's CRC to decide whether it has it.
                file.seek(0);
                quint32 crc = 0;
                for (QByteArray chunk = file.read(65536); !chunk.isEmpty();
                     chunk = file.read(65536))
                    crc = crc32(chunk, crc);
                putHex(Header::at(ZCRC, crc));
                continue;
            }
            // A ZRINIT here is usually the receiver's answer to our ZRQINIT,
            // overtaken by the ZFILE.  Resend only if nothing else follows.
            if (type == ZRINIT && headerFollows())
                continue;
            break;
        }
        if (type == ZRPOS) {
            const qint64 start = header.position();
            if (sendData(file, start) == DataOutcome::Skipped) {
                m_job.endFile(file.path(), 0, true, QStringLiteral("the receiver skipped it"));
                return false;
            }
            m_job.endFile(file.path(), file.size());
            return true;
        }
        if (type == ZSKIP) {
            m_job.endFile(file.path(), 0, true, QStringLiteral("the receiver already has it"));
            return false;
        }
        if (isPeerAbort(type) || type == ZFIN)
            failOnPeer(type);
        // ZRINIT (it missed the ZFILE), ZNAK, noise or silence: send it again.
        countError(QStringLiteral("the receiver did not accept the file header"));
    }
}

bool Engine::reposition(OutgoingFile &file, qint64 &position, quint32 requested)
{
    if (qint64(requested) > file.size())
        fail(QStringLiteral("the receiver asked for offset %1 of a %2-byte file")
                 .arg(requested)
                 .arg(file.size()));
    countError(QStringLiteral("the receiver kept asking for data again"));
    position = requested;
    file.seek(position);
    m_job.setFileBytes(position);
    return true;
}

Engine::DataOutcome Engine::sendData(OutgoingFile &file, qint64 start)
{
    qint64 position = start;
    file.seek(position);
    m_job.setFileBytes(position);
    bool needHeader = true;
    qint64 sinceAck = 0;
    m_errors = 0;
    Header header;
    for (;;) {
        if (needHeader) {
            putBinary(Header::at(ZDATA, quint32(position)));
            needHeader = false;
            sinceAck = 0;
        }
        const QByteArray chunk = file.read(kSubpacketSize);
        const bool atEnd = file.atEnd();
        char end = atEnd ? ZCRCE : ZCRCG;
        if (!atEnd && m_receiverBuffer > 0 && sinceAck + chunk.size() >= m_receiverBuffer)
            end = ZCRCW;
        putSubpacket(chunk, end);
        position += chunk.size();
        sinceAck += chunk.size();
        m_job.setFileBytes(position);

        if (end == ZCRCW) {
            // A receiver with a small buffer: wait until it has caught up.
            for (;;) {
                const int type = m_reader.readHeader(header, kHeaderTimeoutMs);
                if (type == ZACK)
                    break;
                if (type == ZRPOS) {
                    reposition(file, position, header.position());
                    break;
                }
                if (type == ZSKIP)
                    return DataOutcome::Skipped;
                if (isPeerAbort(type))
                    failOnPeer(type);
                if (type == kTimeout) {
                    countError(QStringLiteral("the receiver stopped acknowledging data"));
                    break;
                }
            }
            needHeader = true;
            continue;
        }

        // Full duplex: look for a ZRPOS (the receiver saw an error) without
        // stopping the stream.
        bool repositioned = false;
        while (m_job.link.hasInput()) {
            const int c = m_job.link.getByte(0);
            if (c != ZPAD && c != CAN && c != (ZPAD | 0x80))
                continue;
            m_job.link.unget(quint8(c));
            const int type = m_reader.readHeader(header, 2000);
            if (type == ZRPOS) {
                reposition(file, position, header.position());
                repositioned = true;
                break;
            }
            if (type == ZSKIP)
                return DataOutcome::Skipped;
            if (isPeerAbort(type))
                failOnPeer(type);
        }
        if (repositioned) {
            needHeader = true;
            continue;
        }
        if (!atEnd)
            continue;

        // End of file: ZEOF, and wait for the receiver to confirm with ZRINIT.
        for (;;) {
            putBinary(Header::at(ZEOF, quint32(position)));
            int type = kTimeout;
            do {
                type = m_reader.readHeader(header, kHeaderTimeoutMs);
            } while (type == ZACK);
            if (type == ZRINIT || type == ZSKIP)
                return DataOutcome::Done;
            if (type == ZRPOS) {
                reposition(file, position, header.position());
                needHeader = true;
                break;
            }
            if (isPeerAbort(type))
                failOnPeer(type);
            countError(QStringLiteral("the receiver did not confirm the end of the file"));
        }
    }
}

void Engine::finishSession()
{
    Header header;
    for (int attempt = 0; attempt < 3; ++attempt) {
        putHex(Header::at(ZFIN, 0));
        int type = kTimeout;
        do {
            type = m_reader.readHeader(header, kHeaderTimeoutMs);
        } while (type == ZRINIT || type == ZACK);
        if (type == ZFIN) {
            m_job.link.put(QByteArrayLiteral("OO"));
            return;
        }
        if (type == kTimeout || type == kCancelled)
            break;
    }
    // Every file arrived; a receiver that leaves without the closing
    // handshake has not lost anything.
}

// -- receiving ----------------------------------------------------------------

void Engine::sendReceiverInit()
{
    putHex(Header::flags(ZRINIT, CANFDX | CANOVIO | CANFC32));
}

void Engine::refuseCommand()
{
    fail(QStringLiteral("the device asked Portside to run a command (ZCOMMAND); Portside never "
                        "does that"),
         QStringLiteral("Send the file with plain sz on the device, without -c or -i."));
}

void Engine::receiveFiles()
{
    ReceiveDirectory directory(m_job.options);
    const int startMs = int(m_job.options.startTimeout * 1000.0);
    QDeadlineTimer startDeadline(startMs);
    bool started = false;
    int index = 0;
    m_errors = 0;
    Header header;
    QByteArray data;
    sendReceiverInit();
    for (;;) {
        const int wait = started ? kHeaderTimeoutMs
                                 : int(qMin<qint64>(kHeaderTimeoutMs,
                                                    qMax<qint64>(1, startDeadline.remainingTime())));
        const int type = m_reader.readHeader(header, wait);
        switch (type) {
        case ZRQINIT:
            started = true;
            sendReceiverInit();
            break;
        case ZRINIT:
            // Our own ZRINIT, echoed by the line: answering it would loop.
            break;
        case ZSINIT: {
            started = true;
            if (m_reader.readSubpacket(data) < 0) {
                putHex(Header::at(ZNAK, 0));
                break;
            }
            if (header.flag(0) & ESCCTL) {
                m_escaper.setEscapeControls(true);
                m_reader.setEscapeControls(true);
            }
            putHex(Header::at(ZACK, 1));
            break;
        }
        case ZFREECNT: {
            const qint64 free = QStorageInfo(directory.directory()).bytesAvailable();
            putHex(Header::at(ZACK, quint32(qBound<qint64>(0, free, 0xFFFFFFFFLL))));
            break;
        }
        case ZCOMMAND:
            refuseCommand();
        case ZFILE:
            started = true;
            if (m_reader.readSubpacket(data) < 0) {
                countError(QStringLiteral("the file header kept arriving damaged"));
                putHex(Header::at(ZNAK, 0));
                break;
            }
            m_job.result.check = m_reader.lastFrameCrc32() ? QStringLiteral("CRC-32")
                                                           : QStringLiteral("CRC-16");
            if (data == m_lastSkipped) {
                // The sender repeated the header of a file we declined.
                putHex(Header::at(ZSKIP, 0));
                break;
            }
            m_lastSkipped.clear();
            if (!receiveOne(header, data, directory, ++index))
                m_lastSkipped = data;
            m_errors = 0;
            sendReceiverInit();
            break;
        case ZFIN:
            putHex(Header::at(ZFIN, 0));
            // The sender's "OO"; nothing depends on it arriving.
            for (int i = 0; i < 2; ++i) {
                if (m_job.link.getByte(1000) < 0)
                    break;
            }
            return;
        case kCancelled:
        case ZCAN:
        case ZABORT:
            failCancelledByPeer();
        case kTimeout:
            if (!started) {
                if (startDeadline.hasExpired())
                    fail(QStringLiteral("no ZMODEM sender started within %1 s").arg(startMs / 1000),
                         QStringLiteral("Start sz on the device, then receive."));
            } else {
                countError(QStringLiteral("the sender went quiet"));
            }
            sendReceiverInit();
            break;
        default:
            // A damaged header, or a ZDATA/ZEOF left over from a file we
            // already finished: say again that we are ready.
            if (started)
                countError(QStringLiteral("the sender's headers kept arriving damaged"));
            sendReceiverInit();
            break;
        }
    }
}

bool Engine::receiveOne(const Header &header, const QByteArray &info, ReceiveDirectory &directory,
                        int index)
{
    const qsizetype nul = info.indexOf('\0');
    const QString name = QString::fromUtf8(info.left(nul < 0 ? info.size() : nul));
    QList<QByteArray> fields;
    if (nul >= 0) {
        QByteArray rest = info.mid(nul + 1);
        const qsizetype end = rest.indexOf('\0');
        if (end >= 0)
            rest.truncate(end);
        fields = rest.simplified().split(' ');
    }
    bool ok = false;
    qint64 size = -1;
    QDateTime modified;
    int remaining = 0;
    if (!fields.isEmpty()) {
        size = fields.at(0).toLongLong(&ok);
        if (!ok)
            size = -1;
    }
    if (fields.size() > 1) {
        const qint64 seconds = fields.at(1).toLongLong(&ok, 8);
        if (ok && seconds > 0)
            modified = QDateTime::fromSecsSinceEpoch(seconds);
    }
    if (fields.size() > 4)
        remaining = fields.at(4).toInt(&ok);

    const bool resume = m_job.options.resume || header.flag(0) == ZCRESUM;
    QString why;
    std::unique_ptr<IncomingFile> file = directory.open(name, &why, resume);
    m_job.beginFile(ReceiveDirectory::safeName(name), size, index,
                    remaining > 0 ? index + remaining - 1 : 0);
    if (!file) {
        putHex(Header::at(ZSKIP, 0));
        m_job.endFile(QString(), 0, true, why);
        return false;
    }
    // An interrupted download keeps its .part file, so the next attempt can
    // pick up where this one stopped.
    file->setKeepPartial(true);
    if (size >= 0 && file->size() > size)
        file->truncate(0);
    receiveData(*file, size);
    const qint64 bytes = file->size();
    const QString path = file->commit(modified);
    m_job.endFile(path, bytes);
    return true;
}

void Engine::receiveData(IncomingFile &file, qint64 size)
{
    Q_UNUSED(size);
    qint64 position = file.size();
    m_job.setFileBytes(position);
    putHex(Header::at(ZRPOS, quint32(position)));
    m_errors = 0;
    Header header;
    QByteArray data;
    for (;;) {
        const int type = m_reader.readHeader(header, kHeaderTimeoutMs);
        if (type == ZDATA) {
            if (qint64(header.position()) != position) {
                // Data from before our last ZRPOS: keep asking for the
                // right place.
                countError(QStringLiteral("the sender kept sending from the wrong offset"));
                putHex(Header::at(ZRPOS, quint32(position)));
                continue;
            }
            for (;;) {
                const int end = m_reader.readSubpacket(data);
                if (end == kCancelled)
                    failCancelledByPeer();
                if (end < 0) {
                    countError(QStringLiteral("data kept arriving damaged"));
                    putHex(Header::at(ZRPOS, quint32(position)));
                    break;
                }
                file.write(data);
                position += data.size();
                m_job.setFileBytes(position);
                m_errors = 0;
                if (end == ZCRCW || end == ZCRCQ)
                    putHex(Header::at(ZACK, quint32(position)));
                if (end == ZCRCW || end == ZCRCE)
                    break;
            }
            continue;
        }
        if (type == ZEOF) {
            if (qint64(header.position()) == position)
                return;
            // A stale ZEOF; the right one follows the resent data.
            continue;
        }
        if (type == ZFILE) {
            // The sender missed our ZRPOS.
            m_reader.readSubpacket(data);
            putHex(Header::at(ZRPOS, quint32(position)));
            continue;
        }
        if (type == ZCOMMAND)
            refuseCommand();
        if (type == kCancelled || type == ZCAN || type == ZABORT)
            failCancelledByPeer();
        if (type == ZFIN)
            fail(QStringLiteral("the sender stopped in the middle of a file"));
        countError(type == kTimeout ? QStringLiteral("the sender went quiet")
                                    : QStringLiteral("headers kept arriving damaged"));
        putHex(Header::at(ZRPOS, quint32(position)));
    }
}

} // namespace

void sendZmodem(Job &job, const std::vector<std::unique_ptr<OutgoingFile>> &files)
{
    Engine(job).sendFiles(files);
}

void receiveZmodem(Job &job)
{
    Engine(job).receiveFiles();
}

} // namespace portside::transfer
