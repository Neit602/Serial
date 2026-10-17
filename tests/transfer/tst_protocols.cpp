// The protocol engines against each other over an in-memory pipe (PLAN-M4
// §2–§6), plus the framing pieces on their own.  Interop with lrzsz and
// Kermit is in tst_interop.

#include "core/transfer/crc.h"
#include "core/transfer/engine.h"
#include "core/transfer/kermit.h"
#include "core/transfer/transfer.h"
#include "core/transfer/zmodem.h"
#include "support/pipe_channel.h"
#include "support/support.h"

#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtTest>

#include <future>

using namespace portside;
using namespace portside::test;

namespace {

struct Outcome
{
    std::optional<TransferResult> result;
    std::optional<PortsideError> error;
};

struct Transfer
{
    Outcome sender;
    Outcome receiver;
};

TransferOptions quick(Protocol protocol)
{
    TransferOptions options;
    options.protocol = protocol;
    options.startTimeout = 5.0;
    options.maxRetries = 6;
    return options;
}

template <typename Body>
Outcome capture(Body &&body)
{
    Outcome outcome;
    try {
        outcome.result = body();
    } catch (const PortsideError &error) {
        outcome.error = error;
    }
    return outcome;
}

struct Faults
{
    PipeFilter sender;
    PipeFilter receiver;
    // Cancel the sender (or receiver) once this many bytes have left the
    // sender: counted on the wire, because an in-memory pipe is so fast that
    // a whole file moves between two progress reports.
    qint64 cancelSenderAt = -1;
    qint64 cancelReceiverAt = -1;
};

Transfer run(const QStringList &files, const TransferOptions &sendOptions,
             const TransferOptions &receiveOptions, Faults faults = {})
{
    PipePair pipe;
    std::atomic<bool> cancelSend{false};
    std::atomic<bool> cancelReceive{false};
    qint64 sent = 0;
    pipe.left->setFilter([&](const QByteArray &data) {
        sent += data.size();
        if (faults.cancelSenderAt >= 0 && sent >= faults.cancelSenderAt)
            cancelSend = true;
        if (faults.cancelReceiverAt >= 0 && sent >= faults.cancelReceiverAt)
            cancelReceive = true;
        return faults.sender ? faults.sender(data) : data;
    });
    if (faults.receiver)
        pipe.right->setFilter(faults.receiver);
    auto receiver = std::async(std::launch::async, [&] {
        return capture([&] { return receiveFiles(*pipe.right, receiveOptions, cancelReceive); });
    });
    auto sender = std::async(std::launch::async, [&] {
        return capture([&] { return sendFiles(*pipe.left, files, sendOptions, cancelSend); });
    });
    Transfer transfer;
    transfer.sender = sender.get();
    transfer.receiver = receiver.get();
    return transfer;
}

QByteArray randomBytes(qsizetype size, quint32 seed = 7)
{
    QRandomGenerator generator(seed);
    QByteArray data(size, Qt::Uninitialized);
    for (qsizetype i = 0; i < size; ++i)
        data[i] = char(generator.bounded(256));
    return data;
}

QString describe(const Outcome &outcome)
{
    if (outcome.error)
        return outcome.error->display();
    return outcome.result ? outcome.result->summary() : QStringLiteral("(nothing)");
}

const QList<Protocol> kFramed = {Protocol::Xmodem, Protocol::Xmodem1k, Protocol::Ymodem,
                                 Protocol::YmodemG, Protocol::Zmodem, Protocol::Kermit};

} // namespace

class TestProtocols : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // -- checks and framing ------------------------------------------------

    void crcCheckValues()
    {
        const QByteArray check = "123456789";
        QCOMPARE(transfer::crc16(check), quint16(0x31C3));
        QCOMPARE(transfer::crc32(check), quint32(0xCBF43926));
        QCOMPARE(transfer::crcKermit(check), quint16(0x2189));
        // Incremental CRCs equal one pass.
        QCOMPARE(transfer::crc32(check.mid(4), transfer::crc32(check.left(4))),
                 quint32(0xCBF43926));
        QCOMPARE(transfer::crc16(check.mid(4), transfer::crc16(check.left(4))), quint16(0x31C3));
        QCOMPARE(transfer::checksum8(QByteArray("\xff\x02", 2)), quint8(0x01));
    }

    void zmodemHexHeaderMatchesTheSpecification()
    {
        using namespace transfer::zmodem;
        // ZRQINIT as every sz sends it.
        QCOMPARE(encodeHexHeader(Header::at(ZRQINIT, 0)),
                 QByteArray("**\x18" "B00000000000000\r\x8a\x11"));
        // ZRINIT with CANFDX|CANOVIO|CANFC32 in ZF0, as lrzsz's rz sends it.
        const QByteArray zrinit = encodeHexHeader(Header::flags(ZRINIT, 0x23));
        QVERIFY(zrinit.startsWith("**\x18" "B0100000023"));
        // No XON after ZACK and ZFIN.
        QVERIFY(encodeHexHeader(Header::at(ZFIN, 0)).endsWith("\r\x8a"));
        QCOMPARE(Header::at(ZRPOS, 0x12345678).position(), quint32(0x12345678));
        QCOMPARE(Header::flags(ZRINIT, 0x23).flag(0), quint8(0x23));
    }

    void zmodemEscapingHidesFlowControlAndZdle()
    {
        using namespace transfer::zmodem;
        QByteArray all;
        for (int i = 0; i < 256; ++i)
            all += char(i);
        all += "@\r@\x8d";
        Escaper escaper;
        const QByteArray out = encodeSubpacket(all, ZCRCG, true, escaper);
        // The only raw ZDLEs are escape prefixes, and nothing a modem or a
        // terminal driver would act on survives unescaped.
        for (qsizetype i = 0; i < out.size(); ++i) {
            const quint8 c = quint8(out[i]);
            if (c == 0x18) {
                ++i;
                continue;
            }
            QVERIFY2(c != 0x11 && c != 0x13 && c != 0x91 && c != 0x93 && c != 0x10 && c != 0x90,
                     qPrintable(QStringLiteral("raw 0x%1 at %2").arg(c, 2, 16).arg(i)));
        }
        QVERIFY(!out.contains("@\r"));
    }

    void zmodemStartDetectorSeesSplitSequences()
    {
        using namespace transfer::zmodem;
        StartDetector detector;
        QCOMPARE(detector.feed("hello world\r\n"), StartDetector::Event::None);
        QCOMPARE(detector.feed("rz\r**\x18"), StartDetector::Event::None);
        QCOMPARE(detector.feed("B00000000000000\r\x8a"), StartDetector::Event::SenderReady);
        QCOMPARE(detector.feed("***\x18" "B0100000023"), StartDetector::Event::ReceiverReady);
        QCOMPARE(detector.feed("**\x18" "B02"), StartDetector::Event::None);
        QCOMPARE(detector.feed("a * b ** c"), StartDetector::Event::None);
    }

    void kermitPrefixingRoundTrips_data()
    {
        QTest::addColumn<bool>("binQuoting");
        QTest::addColumn<bool>("repeating");
        QTest::newRow("plain") << false << false;
        QTest::newRow("8th-bit") << true << false;
        QTest::newRow("repeat") << false << true;
        QTest::newRow("both") << true << true;
    }

    void kermitPrefixingRoundTrips()
    {
        QFETCH(bool, binQuoting);
        QFETCH(bool, repeating);
        using namespace transfer::kermit;
        Agreed agreed;
        agreed.binQuoting = binQuoting;
        agreed.binPrefix = '&';
        agreed.repeating = repeating;
        QByteArray input = randomBytes(5000, 3);
        input += QByteArray(300, 'a') + QByteArray(5, '#') + QByteArray(100, '\0') + "&~#&~";
        for (int capacity : {10, 37, 90, 1000}) {
            QByteArray decoded;
            qsizetype offset = 0;
            while (offset < input.size()) {
                qsizetype consumed = 0;
                const QByteArray encoded =
                    encodeData(QByteArrayView(input).mid(offset), capacity, agreed, &consumed);
                QVERIFY(consumed > 0);
                QVERIFY(encoded.size() <= capacity);
                for (const char c : encoded)
                    QVERIFY2(quint8(c) >= 32 && quint8(c) != 127 && (binQuoting ? quint8(c) < 128 : true),
                             "encoded data must be printable");
                const auto back = decodeData(encoded, agreed);
                QVERIFY(back.has_value());
                decoded += *back;
                offset += consumed;
            }
            QCOMPARE(decoded, input);
        }
    }

    void kermitInitNegotiation()
    {
        using namespace transfer::kermit;
        InitParams ours;
        const InitParams parsed = decodeInit(encodeInit(ours));
        QCOMPARE(parsed.maxLength, 94);
        QCOMPARE(parsed.checkType, 3);
        QCOMPARE(parsed.repeatPrefix, '~');
        QVERIFY(parsed.longPackets);
        QVERIFY(parsed.attributes);
        QCOMPARE(parsed.longLength, 4096);

        const Agreed agreed = agree(ours, parsed);
        QCOMPARE(agreed.checkType, 3);
        QVERIFY(agreed.longPackets);
        QVERIFY(agreed.repeating);
        QVERIFY(!agreed.binQuoting);

        // A minimal peer: "~* @-#Y1" and nothing more.
        const InitParams old = decodeInit(QByteArrayView("~* @-#N1"));
        const Agreed fallback = agree(ours, old);
        QCOMPARE(fallback.checkType, 1);
        QVERIFY(!fallback.longPackets);
        QVERIFY(!fallback.attributes);
        QCOMPARE(fallback.sendLength, 94);
        QVERIFY(!fallback.repeating);
    }

    void kermitBlockChecks()
    {
        using namespace transfer::kermit;
        // A NAK for packet 0 as every Kermit sends it: "#  N3" + check.
        const QByteArray body = "# N";
        QCOMPARE(blockCheck(body, 1), QByteArray("3"));
        QCOMPARE(blockCheck(body, 2).size(), 2);
        QCOMPARE(blockCheck(body, 3).size(), 3);
        Agreed agreed;
        QCOMPARE(encodePacket('N', 0, {}, 1, agreed), QByteArray("\x01# N3\r"));
    }

    void receivedNamesStayInTheDownloadFolder_data()
    {
        QTest::addColumn<QString>("peer");
        QTest::addColumn<QString>("expected");
        QTest::newRow("plain") << "firmware.bin" << "firmware.bin";
        QTest::newRow("unix traversal") << "../../etc/passwd" << "passwd";
        QTest::newRow("windows path") << "C:\\Windows\\boot.ini" << "boot.ini";
        QTest::newRow("hidden") << "../.bashrc" << "bashrc";
        QTest::newRow("dots") << ".." << "unnamed";
        QTest::newRow("empty") << "" << "unnamed";
        QTest::newRow("device name") << "con.txt" << "_con.txt";
        QTest::newRow("control chars") << "a\x01" "b" << "a_b";
    }

    void receivedNamesStayInTheDownloadFolder()
    {
        QFETCH(QString, peer);
        QFETCH(QString, expected);
        QCOMPARE(transfer::ReceiveDirectory::safeName(peer), expected);
    }

    // -- round trips ---------------------------------------------------------

    void roundTrip_data()
    {
        QTest::addColumn<Protocol>("protocol");
        QTest::addColumn<QByteArray>("content");
        const QList<std::pair<const char *, QByteArray>> contents = {
            {"empty", QByteArray()},
            {"1 byte", QByteArray("x")},
            {"127 bytes", randomBytes(127)},
            {"128 bytes", randomBytes(128)},
            {"129 bytes", randomBytes(129)},
            {"1024 bytes", randomBytes(1024)},
            {"1030 bytes", randomBytes(1030)},
            {"70 kB random", randomBytes(70000)},
            {"text", QByteArray("line one\r\nline two\nline three\r\n").repeated(40)},
            {"all SUB", QByteArray(300, '\x1a')},
            {"all CAN", QByteArray(2000, '\x18')},
            {"all 0xFF", QByteArray(1500, '\xff')},
            {"ends in SUB", randomBytes(200) + '\x1a'},
        };
        for (const Protocol protocol : kFramed) {
            for (const auto &[name, content] : contents) {
                QTest::addRow("%s %s", qPrintable(enumName(protocol)), name) << protocol << content;
            }
        }
    }

    void roundTrip()
    {
        QFETCH(Protocol, protocol);
        QFETCH(QByteArray, content);
        QTemporaryDir source;
        QTemporaryDir target;
        const QString path = source.filePath(QStringLiteral("payload.bin"));
        writeFile(path, content);

        TransferOptions receiving = quick(protocol);
        receiving.directory = target.path();
        receiving.outputName = QStringLiteral("payload.bin");
        const Transfer transfer = run({path}, quick(protocol), receiving);
        QVERIFY2(transfer.sender.result, qPrintable(describe(transfer.sender)));
        QVERIFY2(transfer.receiver.result, qPrintable(describe(transfer.receiver)));

        const QByteArray received = readFile(target.filePath(QStringLiteral("payload.bin")));
        if (protocolIsBatch(protocol)) {
            QCOMPARE(received.size(), content.size());
            QCOMPARE(received, content);
        } else {
            // XMODEM has no length field: the last block arrives padded, and
            // in binary mode the padding stays (a real trailing SUB is data).
            QVERIFY(received.startsWith(content));
            QCOMPARE(received.size() % 128, 0);
            QVERIFY(received.size() - content.size() < 1024);
            for (qsizetype i = content.size(); i < received.size(); ++i)
                QCOMPARE(received.at(i), '\x1a');
        }
        QCOMPARE(transfer.receiver.result->files.size(), 1);
        QCOMPARE(transfer.sender.result->bytes, qint64(content.size()));
        QVERIFY(!QFileInfo::exists(target.filePath(QStringLiteral("payload.bin.part"))));
    }

    void batchKeepsNamesSizesAndDates_data()
    {
        QTest::addColumn<Protocol>("protocol");
        QTest::addColumn<bool>("keepsDate");
        QTest::newRow("ymodem") << Protocol::Ymodem << true;
        QTest::newRow("ymodem-g") << Protocol::YmodemG << true;
        QTest::newRow("zmodem") << Protocol::Zmodem << true;
        QTest::newRow("kermit") << Protocol::Kermit << true;
    }

    void batchKeepsNamesSizesAndDates()
    {
        QFETCH(Protocol, protocol);
        QFETCH(bool, keepsDate);
        QTemporaryDir source;
        QTemporaryDir target;
        QStringList paths;
        const QDateTime when = QDateTime::fromSecsSinceEpoch(1600000000);
        for (const auto &[name, size] :
             QList<std::pair<QString, int>>{{QStringLiteral("one.txt"), 10},
                                            {QStringLiteral("two.bin"), 5000},
                                            {QStringLiteral("three"), 0}}) {
            const QString path = source.filePath(name);
            writeFile(path, randomBytes(size, quint32(size)));
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(when, QFileDevice::FileModificationTime));
            paths << path;
        }
        TransferOptions receiving = quick(protocol);
        receiving.directory = target.path();
        const Transfer transfer = run(paths, quick(protocol), receiving);
        QVERIFY2(transfer.receiver.result, qPrintable(describe(transfer.receiver)));
        QVERIFY2(transfer.sender.result, qPrintable(describe(transfer.sender)));
        QCOMPARE(transfer.receiver.result->files.size(), 3);
        for (const QString &path : paths) {
            const QString copy = target.filePath(QFileInfo(path).fileName());
            QCOMPARE(readFile(copy), readFile(path));
            if (keepsDate)
                QCOMPARE(QFileInfo(copy).lastModified().toSecsSinceEpoch(), when.toSecsSinceEpoch());
        }
    }

    void xmodemModes_data()
    {
        QTest::addColumn<bool>("checksum");
        QTest::addColumn<QString>("check");
        QTest::newRow("crc") << false << "CRC-16";
        QTest::newRow("checksum") << true << "checksum";
    }

    void xmodemModes()
    {
        QFETCH(bool, checksum);
        QFETCH(QString, check);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("in.bin"));
        writeFile(path, randomBytes(3000));
        TransferOptions receiving = quick(Protocol::Xmodem);
        receiving.directory = dir.filePath(QStringLiteral("out"));
        receiving.outputName = QStringLiteral("out.bin");
        receiving.checksum = checksum;
        const Transfer transfer = run({path}, quick(Protocol::Xmodem1k), receiving);
        QVERIFY2(transfer.receiver.result, qPrintable(describe(transfer.receiver)));
        QCOMPARE(transfer.receiver.result->check, check);
        QCOMPARE(transfer.sender.result->check, check);
        QVERIFY(readFile(dir.filePath(QStringLiteral("out/out.bin"))).startsWith(readFile(path)));
    }

    void xmodemTextModeTrimsPadding()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("notes.txt"));
        const QByteArray text = "show running-config\r\nexit\r\n";
        writeFile(path, text);
        TransferOptions receiving = quick(Protocol::Xmodem);
        receiving.directory = dir.filePath(QStringLiteral("out"));
        receiving.outputName = QStringLiteral("notes.txt");
        receiving.textMode = true;
        const Transfer transfer = run({path}, quick(Protocol::Xmodem), receiving);
        QVERIFY2(transfer.receiver.result, qPrintable(describe(transfer.receiver)));
        QCOMPARE(readFile(dir.filePath(QStringLiteral("out/notes.txt"))), text);
    }

    // -- damage and recovery -------------------------------------------------

    void survivesACorruptedByte_data()
    {
        QTest::addColumn<Protocol>("protocol");
        QTest::addColumn<qint64>("offset");
        for (const Protocol protocol : {Protocol::Xmodem, Protocol::Xmodem1k, Protocol::Ymodem,
                                        Protocol::Zmodem, Protocol::Kermit}) {
            for (const qint64 offset : {qint64(300), qint64(5000), qint64(20000)})
                QTest::addRow("%s at %lld", qPrintable(enumName(protocol)), offset)
                    << protocol << offset;
        }
    }

    void survivesACorruptedByte()
    {
        QFETCH(Protocol, protocol);
        QFETCH(qint64, offset);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("data.bin"));
        const QByteArray content = randomBytes(30000, 11);
        writeFile(path, content);
        TransferOptions receiving = quick(protocol);
        receiving.directory = dir.filePath(QStringLiteral("out"));
        receiving.outputName = QStringLiteral("data.bin");
        Faults faults;
        faults.sender = corruptAt(offset);
        const Transfer transfer = run({path}, quick(protocol), receiving, faults);
        QVERIFY2(transfer.receiver.result, qPrintable(describe(transfer.receiver)));
        QVERIFY2(transfer.sender.result, qPrintable(describe(transfer.sender)));
        QVERIFY(transfer.receiver.result->retries + transfer.sender.result->retries > 0);
        const QByteArray received = readFile(dir.filePath(QStringLiteral("out/data.bin")));
        QVERIFY(received.startsWith(content));
        if (protocolIsBatch(protocol))
            QCOMPARE(received.size(), content.size());
    }

    void survivesLostBytes_data()
    {
        QTest::addColumn<Protocol>("protocol");
        for (const Protocol protocol :
             {Protocol::Xmodem, Protocol::Ymodem, Protocol::Zmodem, Protocol::Kermit})
            QTest::newRow(qPrintable(enumName(protocol))) << protocol;
    }

    void survivesLostBytes()
    {
        QFETCH(Protocol, protocol);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("data.bin"));
        const QByteArray content = randomBytes(20000, 5);
        writeFile(path, content);
        TransferOptions receiving = quick(protocol);
        receiving.directory = dir.filePath(QStringLiteral("out"));
        receiving.outputName = QStringLiteral("data.bin");
        Faults faults;
        faults.sender = dropAt(4000, 40);
        const Transfer transfer = run({path}, quick(protocol), receiving, faults);
        QVERIFY2(transfer.receiver.result, qPrintable(describe(transfer.receiver)));
        QVERIFY(readFile(dir.filePath(QStringLiteral("out/data.bin"))).startsWith(content));
    }

    void ymodemGStopsOnTheFirstError()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("data.bin"));
        writeFile(path, randomBytes(20000));
        TransferOptions receiving = quick(Protocol::YmodemG);
        receiving.directory = dir.filePath(QStringLiteral("out"));
        Faults faults;
        faults.sender = corruptAt(6000);
        const Transfer transfer = run({path}, quick(Protocol::YmodemG), receiving, faults);
        QVERIFY(transfer.receiver.error);
        QVERIFY2(transfer.receiver.error->message().contains(QStringLiteral("YMODEM-G")),
                 qPrintable(transfer.receiver.error->message()));
        QVERIFY(transfer.sender.error);
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("out/data.bin"))));
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("out/data.bin.part"))));
    }

    // -- cancel --------------------------------------------------------------

    void cancelIsProtocolCorrect_data()
    {
        QTest::addColumn<Protocol>("protocol");
        QTest::addColumn<bool>("fromSender");
        for (const Protocol protocol : kFramed) {
            QTest::addRow("%s by sender", qPrintable(enumName(protocol))) << protocol << true;
            QTest::addRow("%s by receiver", qPrintable(enumName(protocol))) << protocol << false;
        }
    }

    void cancelIsProtocolCorrect()
    {
        QFETCH(Protocol, protocol);
        QFETCH(bool, fromSender);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("big.bin"));
        writeFile(path, randomBytes(400000));
        TransferOptions receiving = quick(protocol);
        receiving.directory = dir.filePath(QStringLiteral("out"));
        receiving.outputName = QStringLiteral("big.bin");
        Faults faults;
        (fromSender ? faults.cancelSenderAt : faults.cancelReceiverAt) = 20000;
        QElapsedTimer clock;
        clock.start();
        const Transfer transfer = run({path}, quick(protocol), receiving, faults);
        const Outcome &canceller = fromSender ? transfer.sender : transfer.receiver;
        const Outcome &peer = fromSender ? transfer.receiver : transfer.sender;
        QVERIFY(canceller.error);
        QCOMPARE(canceller.error->kind(), ErrorKind::Cancelled);
        // The other end hears about it from the protocol, not by timing out.
        QVERIFY2(peer.error, qPrintable(describe(peer)));
        QCOMPARE(peer.error->kind(), ErrorKind::Transfer);
        QVERIFY2(clock.elapsed() < 8000, qPrintable(QString::number(clock.elapsed())));
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("out/big.bin"))));
    }

    // -- the receiving directory ----------------------------------------------

    void overwritePolicies()
    {
        QTemporaryDir source;
        QTemporaryDir target;
        const QString path = source.filePath(QStringLiteral("cfg.txt"));
        writeFile(path, "new contents");
        const QString existing = target.filePath(QStringLiteral("cfg.txt"));
        writeFile(existing, "old contents");

        // Refuse: ZMODEM skips the file and says why.
        TransferOptions receiving = quick(Protocol::Zmodem);
        receiving.directory = target.path();
        Transfer transfer = run({path}, quick(Protocol::Zmodem), receiving);
        QVERIFY2(transfer.receiver.result, qPrintable(describe(transfer.receiver)));
        QCOMPARE(transfer.receiver.result->skippedCount(), 1);
        QVERIFY(transfer.receiver.result->files.first().note.contains(QStringLiteral("exists")));
        QCOMPARE(transfer.sender.result->skippedCount(), 1);
        QCOMPARE(readFile(existing), QByteArray("old contents"));

        // Refuse: Kermit declines it too.
        receiving.protocol = Protocol::Kermit;
        transfer = run({path}, quick(Protocol::Kermit), receiving);
        QVERIFY2(transfer.receiver.result, qPrintable(describe(transfer.receiver)));
        QCOMPARE(transfer.receiver.result->skippedCount(), 1);
        QCOMPARE(readFile(existing), QByteArray("old contents"));

        // Refuse: YMODEM cannot skip, so it stops with the reason.
        receiving.protocol = Protocol::Ymodem;
        transfer = run({path}, quick(Protocol::Ymodem), receiving);
        QVERIFY(transfer.receiver.error);
        QVERIFY(transfer.receiver.error->message().contains(QStringLiteral("already exists")));
        QCOMPARE(readFile(existing), QByteArray("old contents"));

        // Rename.
        receiving.protocol = Protocol::Zmodem;
        receiving.overwrite = OverwritePolicy::Rename;
        transfer = run({path}, quick(Protocol::Zmodem), receiving);
        QVERIFY2(transfer.receiver.result, qPrintable(describe(transfer.receiver)));
        QCOMPARE(readFile(target.filePath(QStringLiteral("cfg.1.txt"))), QByteArray("new contents"));
        QCOMPARE(readFile(existing), QByteArray("old contents"));

        // Overwrite.
        receiving.overwrite = OverwritePolicy::Overwrite;
        transfer = run({path}, quick(Protocol::Zmodem), receiving);
        QVERIFY2(transfer.receiver.result, qPrintable(describe(transfer.receiver)));
        QCOMPARE(readFile(existing), QByteArray("new contents"));
    }

    void zmodemResumesAPartialDownload()
    {
        QTemporaryDir source;
        QTemporaryDir target;
        const QByteArray content = randomBytes(50000, 9);
        const QString path = source.filePath(QStringLiteral("image.bin"));
        writeFile(path, content);
        writeFile(target.filePath(QStringLiteral("image.bin.part")), content.left(30000));
        TransferOptions receiving = quick(Protocol::Zmodem);
        receiving.directory = target.path();
        receiving.resume = true;
        const Transfer transfer = run({path}, quick(Protocol::Zmodem), receiving);
        QVERIFY2(transfer.receiver.result, qPrintable(describe(transfer.receiver)));
        QCOMPARE(readFile(target.filePath(QStringLiteral("image.bin"))), content);
        QVERIFY(!QFileInfo::exists(target.filePath(QStringLiteral("image.bin.part"))));
    }

    void zmodemRefusesRemoteCommands()
    {
        using namespace transfer::zmodem;
        PipePair pipe;
        std::atomic<bool> cancel{false};
        QTemporaryDir dir;
        TransferOptions receiving = quick(Protocol::Zmodem);
        receiving.directory = dir.path();
        auto receiver = std::async(std::launch::async, [&] {
            return capture([&] { return receiveFiles(*pipe.right, receiving, cancel); });
        });
        // A hostile "sender": straight to ZCOMMAND.
        Escaper escaper;
        pipe.left->write(encodeHexHeader(Header::at(ZRQINIT, 0)));
        pipe.left->write(encodeBinaryHeader(Header::flags(ZCOMMAND, 0), false, escaper));
        pipe.left->write(encodeSubpacket(QByteArrayView("touch pwned\0", 12), ZCRCW, false, escaper));
        const Outcome outcome = receiver.get();
        QVERIFY(outcome.error);
        QVERIFY(outcome.error->message().contains(QStringLiteral("ZCOMMAND")));
        QVERIFY(!QFileInfo::exists(QStringLiteral("pwned")));
        // And it said so on the wire with a cancel.
        QVERIFY(pipe.left->read(500).contains(QByteArray(5, '\x18')));
    }

    // -- timeouts --------------------------------------------------------------

    void silentPeersTimeOutWithAHint_data()
    {
        QTest::addColumn<Protocol>("protocol");
        QTest::addColumn<bool>("sending");
        for (const Protocol protocol : kFramed) {
            QTest::addRow("%s send", qPrintable(enumName(protocol))) << protocol << true;
            QTest::addRow("%s receive", qPrintable(enumName(protocol))) << protocol << false;
        }
    }

    void silentPeersTimeOutWithAHint()
    {
        QFETCH(Protocol, protocol);
        QFETCH(bool, sending);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("f.bin"));
        writeFile(path, "data");
        PipePair pipe;
        std::atomic<bool> cancel{false};
        TransferOptions options = quick(protocol);
        options.startTimeout = 1.0;
        options.directory = dir.path();
        QElapsedTimer clock;
        clock.start();
        const Outcome outcome = capture([&] {
            return sending ? sendFiles(*pipe.left, {path}, options, cancel)
                           : receiveFiles(*pipe.left, options, cancel);
        });
        QVERIFY(outcome.error);
        QCOMPARE(outcome.error->kind(), ErrorKind::Transfer);
        QVERIFY2(!outcome.error->remedy().isEmpty(), qPrintable(outcome.error->message()));
        QVERIFY2(clock.elapsed() < 7000, qPrintable(QString::number(clock.elapsed())));
    }

    // -- raw -------------------------------------------------------------------

    void rawSendIsByteExact()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("blob"));
        const QByteArray content = randomBytes(3000);
        writeFile(path, content);
        PipePair pipe;
        std::atomic<bool> cancel{false};
        TransferOptions options = quick(Protocol::Raw);
        options.rawChunkSize = 100;
        const TransferResult result = sendFiles(*pipe.left, {path}, options, cancel);
        QCOMPARE(result.bytes, qint64(3000));
        QCOMPARE(pipe.right->read(100), content);
        // Raw cannot receive.
        QVERIFY_THROWS_EXCEPTION(PortsideError, receiveFiles(*pipe.right, options, cancel));
    }

    void rawSendCancelsMidFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("blob"));
        writeFile(path, randomBytes(10000));
        PipePair pipe;
        std::atomic<bool> cancel{false};
        TransferOptions options = quick(Protocol::Raw);
        options.rawChunkSize = 100;
        options.rawChunkDelay = 0.01;
        const Outcome outcome = capture([&] {
            return sendFiles(*pipe.left, {path}, options, cancel,
                             [&](const TransferProgress &progress) {
                                 if (progress.totalBytes >= 1000)
                                     cancel = true;
                             });
        });
        QVERIFY(outcome.error);
        QCOMPARE(outcome.error->kind(), ErrorKind::Cancelled);
        QVERIFY(pipe.right->read(100).size() < 10000);
    }

    void xmodemSendsOneFileAtATime()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("a")), "a");
        writeFile(dir.filePath(QStringLiteral("b")), "b");
        PipePair pipe;
        std::atomic<bool> cancel{false};
        QVERIFY_THROWS_EXCEPTION(PortsideError,
                                 sendFiles(*pipe.left,
                                           {dir.filePath(QStringLiteral("a")),
                                            dir.filePath(QStringLiteral("b"))},
                                           quick(Protocol::Xmodem), cancel));
        QVERIFY_THROWS_EXCEPTION(PortsideError,
                                 sendFiles(*pipe.left, {dir.filePath(QStringLiteral("missing"))},
                                           quick(Protocol::Zmodem), cancel));
    }

    void summaryReadsWell()
    {
        TransferResult result;
        result.protocol = Protocol::Zmodem;
        result.direction = TransferDirection::Receive;
        result.files = {TransferredFile{QStringLiteral("a"), QStringLiteral("/tmp/a"), 1200000,
                                        false, {}}};
        result.bytes = 1200000;
        result.seconds = 4.0;
        result.check = QStringLiteral("CRC-32");
        QCOMPARE(result.summary(),
                 QStringLiteral("ZMODEM: received 1 file, 1.2 MB in 4.0 s (300.0 kB/s), CRC-32"));
    }
};

QTEST_GUILESS_MAIN(TestProtocols)
#include "tst_protocols.moc"
