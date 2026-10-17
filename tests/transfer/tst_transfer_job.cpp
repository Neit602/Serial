// Transfers over a real Session: the stream claim (PLAN-M4 §1.1) and
// TransferJob (§1.5), with a protocol engine playing the device on the far
// side of a loopback transport.

#include "core/session_log.h"
#include "core/stream_claim.h"
#include "core/transfer/job.h"
#include "support/loopback_peer.h"
#include "support/support.h"

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <future>

using namespace portside;
using namespace portside::test;

namespace {

LoopbackSession quietLoopback()
{
    LoopbackOptions options;
    options.echo = false;
    LoopbackSession loop = makeLoopbackSession(options);
    loop.session->open();
    return loop;
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
    result.startTimeout = 5.0;
    return result;
}

} // namespace

class TestTransferJob : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void claimTakesTheStreamFromTheTerminal()
    {
        LoopbackSession loop = quietLoopback();
        Collector terminal(loop.session.get());
        QSignalSpy claimed(loop.session.get(), &Session::claimedDataReceived);
        QSignalSpy changed(loop.session.get(), &Session::streamClaimChanged);

        std::shared_ptr<StreamClaim> claim = loop.session->claimStream();
        QVERIFY(loop.session->isStreamClaimed());
        QCOMPARE(changed.count(), 1);

        loop.transport->feed("ACK?");
        QCOMPARE(claim->read(2000), QByteArray("ACK?"));
        QVERIFY(waitFor([&] { return claimed.count() == 1; }));
        QVERIFY(terminal.data.isEmpty());
        QCOMPARE(loop.session->bytesIn(), qint64(4));

        // The terminal cannot type into a transfer.
        QFuture<qint64> typed = settle(loop.session->send("x"));
        QVERIFY_THROWS_EXCEPTION(PortsideError, typed.result());
        QFuture<BreakResult> broke = settle(loop.session->sendBreak());
        QVERIFY_THROWS_EXCEPTION(PortsideError, broke.result());
        // But the transfer can.
        claim->write("block");
        QCOMPARE(loop.transport->written(), QByteArray("block"));
        QVERIFY(waitFor([&] { return loop.session->bytesOut() == 5; }));

        // Only one claim at a time.
        QVERIFY_THROWS_EXCEPTION(PortsideError, loop.session->claimStream());

        loop.session->releaseStream();
        QVERIFY(!loop.session->isStreamClaimed());
        QCOMPARE(changed.count(), 2);
        QVERIFY_THROWS_EXCEPTION(PortsideError, claim->read(10));
        loop.transport->feed("prompt>");
        QVERIFY(waitFor([&] { return terminal.data == "prompt>"; }));
    }

    void bytesWaitingAtClaimTimeGoToTheTransfer()
    {
        LoopbackSession loop = quietLoopback();
        // Feed without letting the event loop drain it to the terminal.
        loop.transport->feed("C");
        QThread::msleep(50);
        std::shared_ptr<StreamClaim> claim = loop.session->claimStream();
        QByteArray got = claim->read(1000);
        QCOMPARE(got, QByteArray("C"));
        loop.session->releaseStream();
    }

    void claimNeedsAConnectedSession()
    {
        LoopbackSession loop = makeLoopbackSession();
        QVERIFY_THROWS_EXCEPTION(PortsideError, loop.session->claimStream());
    }

    void closingTheSessionEndsTheClaim()
    {
        LoopbackSession loop = quietLoopback();
        std::shared_ptr<StreamClaim> claim = loop.session->claimStream();
        auto reader = std::async(std::launch::async, [&] {
            try {
                claim->read(10000);
                claim->read(10000);
            } catch (const PortsideError &error) {
                return error.message();
            }
            return QString();
        });
        loop.session->close();
        QVERIFY(reader.get().contains(QStringLiteral("closed")));
        QVERIFY_THROWS_EXCEPTION(PortsideError, claim->write("late"));
    }

    void losingTheDeviceEndsTheClaim()
    {
        LoopbackSession loop = quietLoopback();
        std::shared_ptr<StreamClaim> claim = loop.session->claimStream();
        loop.transport->setVanishAfter(2);
        loop.transport->feed("abcdef");
        bool failed = false;
        QElapsedTimer clock;
        clock.start();
        while (!failed && clock.elapsed() < 3000) {
            try {
                claim->read(50);
            } catch (const PortsideError &error) {
                failed = error.kind() == ErrorKind::DeviceVanished;
            }
            QCoreApplication::processEvents();
        }
        QVERIFY(failed);
    }

    void sendsAndReceivesThroughASession_data()
    {
        QTest::addColumn<Protocol>("protocol");
        QTest::addColumn<bool>("sending");
        for (const Protocol protocol :
             {Protocol::Xmodem1k, Protocol::Ymodem, Protocol::Zmodem, Protocol::Kermit}) {
            QTest::addRow("%s send", qPrintable(enumName(protocol))) << protocol << true;
            QTest::addRow("%s receive", qPrintable(enumName(protocol))) << protocol << false;
        }
    }

    void sendsAndReceivesThroughASession()
    {
        QFETCH(Protocol, protocol);
        QFETCH(bool, sending);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("firmware.bin"));
        const QByteArray content = randomBytes(40000);
        writeFile(path, content);
        const QString inbox = dir.filePath(QStringLiteral("inbox"));

        LoopbackSession loop = quietLoopback();
        Collector terminal(loop.session.get());
        LoopbackPeer peer(loop.transport);
        std::atomic<bool> cancel{false};
        TransferOptions local = options(protocol, inbox);
        local.outputName = QStringLiteral("firmware.bin");
        TransferOptions remote = options(protocol, inbox);
        remote.outputName = QStringLiteral("firmware.bin");

        TransferJob job(loop.session.get(),
                        sending ? TransferDirection::Send : TransferDirection::Receive,
                        sending ? QStringList{path} : QStringList{}, local);
        QSignalSpy finished(&job, &TransferJob::finished);
        QSignalSpy failed(&job, &TransferJob::failed);
        QSignalSpy progressed(&job, &TransferJob::progressed);
        job.start();
        QVERIFY(job.isRunning());

        auto device = std::async(std::launch::async, [&] {
            try {
                if (sending)
                    receiveFiles(peer, remote, cancel);
                else
                    sendFiles(peer, {path}, remote, cancel);
            } catch (const PortsideError &error) {
                return error.display();
            }
            return QString();
        });
        QVERIFY(waitFor([&] { return finished.count() + failed.count() > 0; }, 30000));
        QCOMPARE(device.get(), QString());
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().value<PortsideError>().display()));
        const auto result = finished.first().first().value<TransferResult>();
        QCOMPARE(result.files.size(), 1);
        QVERIFY(readFile(dir.filePath(QStringLiteral("inbox/firmware.bin"))).startsWith(content));
        QVERIFY(!progressed.isEmpty());
        const auto last = progressed.last().first().value<TransferProgress>();
        QVERIFY(last.totalBytes >= content.size());
        // The terminal saw none of it, and has the port back.
        QVERIFY(!job.isRunning());
        QVERIFY(!loop.session->isStreamClaimed());
        QVERIFY(terminal.data.isEmpty());
        loop.transport->feed("$ ");
        QVERIFY(waitFor([&] { return terminal.data == "$ "; }));
    }

    void cancelStopsAndReleases()
    {
        LoopbackSession loop = quietLoopback();
        QTemporaryDir dir;
        TransferJob job(loop.session.get(), TransferDirection::Receive, {},
                        options(Protocol::Zmodem, dir.path()));
        QSignalSpy failed(&job, &TransferJob::failed);
        job.start();
        QThread::msleep(100);
        job.cancel();
        QVERIFY(waitFor([&] { return !failed.isEmpty(); }, 3000));
        QCOMPARE(failed.first().first().value<PortsideError>().kind(), ErrorKind::Cancelled);
        QVERIFY(!loop.session->isStreamClaimed());
        // The cancel went out on the wire.
        QVERIFY(loop.transport->written().contains(QByteArray(5, '\x18')));
    }

    void destroyingARunningJobCancelsIt()
    {
        LoopbackSession loop = quietLoopback();
        QTemporaryDir dir;
        {
            TransferJob job(loop.session.get(), TransferDirection::Receive, {},
                            options(Protocol::Kermit, dir.path()));
            job.start();
            QThread::msleep(50);
        }
        QVERIFY(!loop.session->isStreamClaimed());
        QFuture<qint64> typed = settle(loop.session->send("ok"));
        QCOMPARE(typed.result(), qint64(2));
    }

    void closingTheSessionFailsTheJob()
    {
        LoopbackSession loop = quietLoopback();
        QTemporaryDir dir;
        TransferJob job(loop.session.get(), TransferDirection::Receive, {},
                        options(Protocol::Ymodem, dir.path()));
        QSignalSpy failed(&job, &TransferJob::failed);
        job.start();
        QThread::msleep(50);
        loop.session->close();
        QVERIFY(waitFor([&] { return !failed.isEmpty(); }, 3000));
        QVERIFY(!job.isRunning());
    }

    void rawLogsKeepTransferBytesTextLogsDoNot()
    {
        QTemporaryDir dir;
        LoopbackSession loop = quietLoopback();
        LogSettings raw;
        raw.format = LogFormat::Raw;
        SessionLogger rawLog(raw, loop.session->port());
        rawLog.attach(loop.session.get());
        rawLog.start(dir.filePath(QStringLiteral("raw.log")));
        SessionLogger textLog({}, loop.session->port());
        textLog.attach(loop.session.get());
        textLog.start(dir.filePath(QStringLiteral("text.log")));

        loop.transport->feed("before ");
        QVERIFY(waitFor([&] { return loop.session->bytesIn() == 7; }));
        std::shared_ptr<StreamClaim> claim = loop.session->claimStream();
        loop.transport->feed(QByteArray("\x01\x02\xff", 3));
        QCOMPARE(claim->read(1000).size(), 3);
        QVERIFY(waitFor([&] { return loop.session->bytesIn() == 10; }));
        loop.session->releaseStream();
        loop.transport->feed("after");
        QVERIFY(waitFor([&] { return loop.session->bytesIn() == 15; }));
        rawLog.stop();
        textLog.stop();
        QCOMPARE(readFile(dir.filePath(QStringLiteral("raw.log"))),
                 QByteArray("before \x01\x02\xff" "after"));
        QCOMPARE(readFile(dir.filePath(QStringLiteral("text.log"))), QByteArray("before after\n"));
    }
};

QTEST_GUILESS_MAIN(TestTransferJob)
#include "tst_transfer_job.moc"
