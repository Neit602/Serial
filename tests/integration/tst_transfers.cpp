// File transfers end to end over a real (socat) serial pair (PLAN-M4 §8.5):
// two Portside sessions talking to each other, the CLI's send and receive,
// and the failure paths — the cable pulled mid-transfer, a cancel.

#include "core/serial_transport.h"
#include "core/session.h"
#include "core/transfer/job.h"
#include "support/socat_pair.h"
#include "support/support.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace portside;
using namespace portside::test;

namespace {

std::unique_ptr<Session> sessionFor(const QString &port)
{
    auto session = std::make_unique<Session>(std::make_unique<SerialTransport>(port));
    session->open();
    return session;
}

// Big enough that even an optimised build streaming over a PTY at tens of
// MB/s is still mid-file when the test pulls the cable or cancels.
QByteArray bigFile()
{
    QByteArray block(1 << 20, Qt::Uninitialized);
    for (qsizetype i = 0; i < block.size(); ++i)
        block[i] = char(QRandomGenerator::global()->bounded(256));
    return block.repeated(64);
}

QByteArray randomBytes(qsizetype size)
{
    QByteArray data(size, Qt::Uninitialized);
    for (qsizetype i = 0; i < size; ++i)
        data[i] = char(QRandomGenerator::global()->bounded(256));
    return data;
}

TransferOptions options(Protocol protocol, const QString &directory = {})
{
    TransferOptions result;
    result.protocol = protocol;
    result.directory = directory;
    result.outputName = QStringLiteral("payload.bin");
    result.startTimeout = 10.0;
    return result;
}

struct Outcome
{
    std::optional<TransferResult> result;
    std::optional<PortsideError> error;
};

void watch(TransferJob &job, Outcome &outcome)
{
    QObject::connect(&job, &TransferJob::finished, &job,
                     [&outcome](const TransferResult &result) { outcome.result = result; });
    QObject::connect(&job, &TransferJob::failed, &job,
                     [&outcome](const PortsideError &error) { outcome.error = error; });
}

bool done(const Outcome &outcome)
{
    return outcome.result || outcome.error;
}

QString describe(const Outcome &outcome)
{
    if (outcome.error)
        return outcome.error->display();
    return outcome.result ? outcome.result->summary() : QStringLiteral("(still running)");
}

} // namespace

class TestTransfers : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        qRegisterMetaType<PortsideError>();
        qRegisterMetaType<TransferResult>();
        qRegisterMetaType<TransferProgress>();
    }

    void init()
    {
        if (!SocatPair::available())
            QSKIP("socat is not installed (or this is Windows)");
        QVERIFY(m_pair.start());
    }

    void cleanup() { m_pair.stop(); }

    void sessionToSession_data()
    {
        QTest::addColumn<Protocol>("protocol");
        for (const Protocol protocol : {Protocol::Xmodem, Protocol::Xmodem1k, Protocol::Ymodem,
                                        Protocol::YmodemG, Protocol::Zmodem, Protocol::Kermit})
            QTest::newRow(qPrintable(enumName(protocol))) << protocol;
    }

    void sessionToSession()
    {
        QFETCH(Protocol, protocol);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("payload.bin"));
        const QByteArray content = randomBytes(300000);
        writeFile(path, content);
        const QString inbox = dir.filePath(QStringLiteral("inbox"));

        auto left = sessionFor(m_pair.left());
        auto right = sessionFor(m_pair.right());
        TransferJob receiver(right.get(), TransferDirection::Receive, {}, options(protocol, inbox));
        TransferJob sender(left.get(), TransferDirection::Send, {path}, options(protocol));
        Outcome received, sent;
        watch(receiver, received);
        watch(sender, sent);
        receiver.start();
        sender.start();
        QVERIFY(waitFor([&] { return done(received) && done(sent); }, 120000));
        QVERIFY2(sent.result, qPrintable(describe(sent)));
        QVERIFY2(received.result, qPrintable(describe(received)));
        const QByteArray copy = readFile(dir.filePath(QStringLiteral("inbox/payload.bin")));
        QVERIFY(copy.startsWith(content));
        if (protocolIsBatch(protocol))
            QCOMPARE(copy.size(), content.size());
        QVERIFY(left->bytesOut() >= content.size());
        QVERIFY(right->bytesIn() >= content.size());
    }

    void theCliSendsAndReceives_data()
    {
        QTest::addColumn<QString>("protocol");
        QTest::newRow("zmodem") << "zmodem";
        QTest::newRow("ymodem") << "ymodem";
        QTest::newRow("kermit") << "kermit";
        QTest::newRow("xmodem-1k") << "xmodem-1k";
    }

    void theCliSendsAndReceives()
    {
        QFETCH(QString, protocol);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("config.txt"));
        const QByteArray content = randomBytes(50000);
        writeFile(path, content);
        const QString inbox = dir.filePath(QStringLiteral("inbox"));
        const QString cli = qEnvironmentVariable("PORTSIDE_CLI");

        QProcess receiver;
        receiver.start(cli, {QStringLiteral("receive"), m_pair.right(), QStringLiteral("-P"), protocol,
                             QStringLiteral("--into"), inbox, QStringLiteral("-o"),
                             QStringLiteral("config.txt"), QStringLiteral("--timeout"),
                             QStringLiteral("20")});
        QVERIFY(receiver.waitForStarted());
        QProcess sender;
        sender.start(cli, {QStringLiteral("send"), m_pair.left(), path, QStringLiteral("-P"), protocol,
                           QStringLiteral("--timeout"), QStringLiteral("20")});
        QVERIFY(sender.waitForStarted());
        QVERIFY(sender.waitForFinished(60000));
        QVERIFY(receiver.waitForFinished(60000));
        const QByteArray senderErr = sender.readAllStandardError();
        const QByteArray receiverErr = receiver.readAllStandardError();
        QVERIFY2(sender.exitCode() == 0, senderErr.constData());
        QVERIFY2(receiver.exitCode() == 0, receiverErr.constData());
        QVERIFY(readFile(dir.filePath(QStringLiteral("inbox/config.txt"))).startsWith(content));
        // Status goes to stderr: a log of a CI job reads well.
        QVERIFY(senderErr.contains("[portside] connected to"));
        QVERIFY(receiverErr.contains("[portside] saved"));
        QVERIFY(sender.readAllStandardOutput().isEmpty());
    }

    void theCliRefusesToOverwriteAndExitsNonZero()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("a.bin"));
        writeFile(path, "new");
        const QString inbox = dir.filePath(QStringLiteral("inbox"));
        QDir().mkpath(inbox);
        writeFile(dir.filePath(QStringLiteral("inbox/a.bin")), "old");
        const QString cli = qEnvironmentVariable("PORTSIDE_CLI");
        QProcess receiver;
        receiver.start(cli, {QStringLiteral("receive"), m_pair.right(), QStringLiteral("--into"), inbox,
                             QStringLiteral("--timeout"), QStringLiteral("20")});
        QVERIFY(receiver.waitForStarted());
        QProcess sender;
        sender.start(cli, {QStringLiteral("send"), m_pair.left(), path});
        QVERIFY(sender.waitForFinished(60000));
        QVERIFY(receiver.waitForFinished(60000));
        QCOMPARE(receiver.exitCode(), 1);
        QVERIFY(receiver.readAllStandardError().contains("skipped a.bin"));
        QCOMPARE(sender.exitCode(), 1);
        QCOMPARE(readFile(dir.filePath(QStringLiteral("inbox/a.bin"))), QByteArray("old"));
    }

    void pullingTheCableMidTransferFailsCleanly_data()
    {
        QTest::addColumn<Protocol>("protocol");
        QTest::newRow("zmodem") << Protocol::Zmodem;
        QTest::newRow("ymodem") << Protocol::Ymodem;
        QTest::newRow("kermit") << Protocol::Kermit;
    }

    void pullingTheCableMidTransferFailsCleanly()
    {
        QFETCH(Protocol, protocol);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("payload.bin"));
        writeFile(path, bigFile());
        auto left = sessionFor(m_pair.left());
        auto right = sessionFor(m_pair.right());
        TransferJob receiver(right.get(), TransferDirection::Receive, {},
                             options(protocol, dir.filePath(QStringLiteral("inbox"))));
        TransferJob sender(left.get(), TransferDirection::Send, {path}, options(protocol));
        Outcome received, sent;
        watch(receiver, received);
        watch(sender, sent);
        receiver.start();
        sender.start();
        QVERIFY(waitFor([&] { return receiver.progress().totalBytes > 0; }, 30000));
        m_pair.stop();
        QVERIFY(waitFor([&] { return done(received) && done(sent); }, 30000));
        QVERIFY2(received.error, qPrintable(describe(received)));
        QVERIFY2(sent.error, qPrintable(describe(sent)));
        // No half-file that looks finished.
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("inbox/payload.bin"))));
        // And the sessions say what happened instead of hanging.
        QVERIFY(waitFor([&] { return !left->isConnected() && !right->isConnected(); }, 5000));
    }

    void cancellingFromOneEndStopsTheOther()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("payload.bin"));
        writeFile(path, bigFile());
        auto left = sessionFor(m_pair.left());
        auto right = sessionFor(m_pair.right());
        TransferJob receiver(right.get(), TransferDirection::Receive, {},
                             options(Protocol::Zmodem, dir.filePath(QStringLiteral("inbox"))));
        TransferJob sender(left.get(), TransferDirection::Send, {path}, options(Protocol::Zmodem));
        Outcome received, sent;
        watch(receiver, received);
        watch(sender, sent);
        receiver.start();
        sender.start();
        QVERIFY(waitFor([&] { return receiver.progress().totalBytes > 0; }, 30000));
        sender.cancel();
        QVERIFY(waitFor([&] { return done(received) && done(sent); }, 15000));
        QVERIFY2(sent.error, qPrintable(describe(sent)));
        QCOMPARE(sent.error->kind(), ErrorKind::Cancelled);
        QVERIFY2(received.error, qPrintable(describe(received)));
        // Both ports are usable straight away.
        Collector rx(right.get());
        QVERIFY(waitFor([&] { return !left->isStreamClaimed() && !right->isStreamClaimed(); }));
        rx.data.clear();
        settle(left->send("hello\r"));
        QVERIFY(waitFor([&] { return rx.data.endsWith("hello\r"); }, 5000));
    }

private:
    SocatPair m_pair;
};

QTEST_GUILESS_MAIN(TestTransfers)
#include "tst_transfers.moc"
