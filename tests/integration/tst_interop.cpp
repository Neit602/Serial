// Interop with other implementations — the M4 exit criterion (PLAN-M4 §8):
// lrzsz (sx/rx, sb/rb, sz/rz) and Kermit (G-Kermit or C-Kermit), in both
// directions, over a socat pair, for a matrix of sizes and contents.
//
// Each tool runs with its standard input and output on one end of the pair,
// exactly as it would on a device's console; Portside runs a TransferJob on
// the other end.  Tests skip, saying which binary is missing, where a tool is
// not installed.

#include "core/serial_transport.h"
#include "core/session.h"
#include "core/transfer/job.h"
#include "support/socat_pair.h"
#include "support/support.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace portside;
using namespace portside::test;

namespace {

// lrzsz installs as sz/rz on Debian and Homebrew, lsz/lrz on some systems.
QString findTool(const QStringList &names)
{
    for (const QString &name : names) {
        const QString path = QStandardPaths::findExecutable(name);
        if (!path.isEmpty())
            return path;
    }
    return {};
}

QString lrzszTool(const QString &tool)
{
    return findTool({tool, QStringLiteral("l") + tool});
}

struct Content
{
    const char *name;
    QByteArray bytes;
};

QList<Content> contents()
{
    QRandomGenerator generator(42);
    QByteArray random(1 << 20, Qt::Uninitialized);
    for (qsizetype i = 0; i < random.size(); ++i)
        random[i] = char(generator.bounded(256));
    QByteArray text;
    for (int i = 0; i < 400; ++i)
        text += "interface GigabitEthernet0/" + QByteArray::number(i) + "\r\n description lab\r\n";
    return {
        {"0 bytes", QByteArray()},
        {"1 byte", QByteArray("x")},
        {"1 KB", random.left(1024)},
        {"1 MB random", random},
        {"text", text},
        {"all SUB", QByteArray(3000, '\x1a')},
        {"all CAN", QByteArray(3000, '\x18')},
    };
}

// One external program on the far end of the pair.  With `onStdio` its
// standard input and output are the port, as for a program on a device's
// console; otherwise it opens the port itself and gets no terminal at all.
class Peer
{
public:
    Peer(const QString &port, const QString &workingDirectory, bool onStdio = true)
    {
        m_process.setWorkingDirectory(workingDirectory);
        if (onStdio) {
            m_process.setStandardInputFile(port);
            m_process.setStandardOutputFile(port, QIODevice::Append);
        } else {
            m_process.setStandardInputFile(QProcess::nullDevice());
        }
    }

    bool start(const QString &program, const QStringList &arguments)
    {
        m_process.start(program, arguments);
        return m_process.waitForStarted(5000);
    }

    bool finish(int timeoutMs = 60000)
    {
        if (!m_process.waitForFinished(timeoutMs)) {
            m_process.kill();
            m_process.waitForFinished(3000);
            return false;
        }
        return m_process.exitStatus() == QProcess::NormalExit && m_process.exitCode() == 0;
    }

    QString diagnostics() { return QString::fromLocal8Bit(m_process.readAllStandardError()); }

private:
    QProcess m_process;
};

struct Outcome
{
    std::optional<TransferResult> result;
    std::optional<PortsideError> error;

    bool done() const { return result || error; }
    QString describe() const
    {
        return error ? error->display() : result ? result->summary() : QStringLiteral("(running)");
    }
};

Outcome runJob(Session *session, TransferDirection direction, const QStringList &files,
               const TransferOptions &options, const std::function<void()> &afterStart)
{
    Outcome outcome;
    TransferJob job(session, direction, files, options);
    QObject::connect(&job, &TransferJob::finished, &job,
                     [&](const TransferResult &result) { outcome.result = result; });
    QObject::connect(&job, &TransferJob::failed, &job,
                     [&](const PortsideError &error) { outcome.error = error; });
    job.start();
    if (afterStart)
        afterStart();
    waitFor([&] { return outcome.done(); }, 180000);
    return outcome;
}

TransferOptions optionsFor(Protocol protocol, const QString &directory = {})
{
    TransferOptions options;
    options.protocol = protocol;
    options.directory = directory;
    options.outputName = QStringLiteral("payload.bin");
    options.startTimeout = 20.0;
    return options;
}

} // namespace

class TestInterop : public QObject
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

    // -- lrzsz ---------------------------------------------------------------

    void lrzsz_data()
    {
        QTest::addColumn<Protocol>("protocol");
        QTest::addColumn<bool>("portsideSends");
        QTest::addColumn<QByteArray>("content");
        for (const Protocol protocol : {Protocol::Xmodem, Protocol::Xmodem1k, Protocol::Ymodem,
                                        Protocol::Zmodem}) {
            for (const bool portsideSends : {true, false}) {
                for (const Content &content : contents()) {
                    QTest::addRow("%s %s %s", qPrintable(enumName(protocol)),
                                  portsideSends ? "to lrzsz" : "from lrzsz", content.name)
                        << protocol << portsideSends << content.bytes;
                }
            }
        }
    }

    void lrzsz()
    {
        QFETCH(Protocol, protocol);
        QFETCH(bool, portsideSends);
        QFETCH(QByteArray, content);
        const bool xmodem = !protocolIsBatch(protocol);
        if (xmodem && content.isEmpty())
            QSKIP("XMODEM cannot carry an empty file in a way every implementation agrees on");

        const QString sendTool = lrzszTool(xmodem ? QStringLiteral("sx")
                                              : protocol == Protocol::Ymodem ? QStringLiteral("sb")
                                                                             : QStringLiteral("sz"));
        const QString receiveTool = lrzszTool(xmodem ? QStringLiteral("rx")
                                                 : protocol == Protocol::Ymodem ? QStringLiteral("rb")
                                                                                : QStringLiteral("rz"));
        const QString tool = portsideSends ? receiveTool : sendTool;
        if (tool.isEmpty())
            QSKIP("lrzsz is not installed (apt install lrzsz, brew install lrzsz)");

        QTemporaryDir dir;
        const QString theirs = dir.filePath(QStringLiteral("theirs"));
        const QString ours = dir.filePath(QStringLiteral("ours"));
        QVERIFY(QDir().mkpath(theirs));
        QVERIFY(QDir().mkpath(ours));
        auto session = std::make_unique<Session>(std::make_unique<SerialTransport>(m_pair.left()));
        session->open();
        Peer peer(m_pair.right(), theirs);

        QByteArray received;
        if (portsideSends) {
            const QString path = dir.filePath(QStringLiteral("payload.bin"));
            writeFile(path, content);
            // -b binary, -y overwrite; rx needs the file name XMODEM does not carry.
            QStringList arguments{QStringLiteral("-b")};
            if (xmodem)
                arguments << QStringLiteral("-c") << QStringLiteral("payload.bin");
            else
                arguments << QStringLiteral("-y");
            QVERIFY(peer.start(tool, arguments));
            const Outcome outcome =
                runJob(session.get(), TransferDirection::Send, {path}, optionsFor(protocol), {});
            const bool peerOk = peer.finish();
            QVERIFY2(outcome.result, qPrintable(outcome.describe() + QLatin1Char('\n') + peer.diagnostics()));
            QVERIFY2(peerOk, qPrintable(peer.diagnostics()));
            received = readFile(QDir(theirs).filePath(QStringLiteral("payload.bin")));
        } else {
            const QString path = QDir(theirs).filePath(QStringLiteral("payload.bin"));
            writeFile(path, content);
            QStringList arguments{QStringLiteral("-b")};
            if (protocol == Protocol::Xmodem1k)
                arguments << QStringLiteral("-k");
            arguments << QStringLiteral("payload.bin");
            const Outcome outcome = runJob(session.get(), TransferDirection::Receive, {},
                                           optionsFor(protocol, ours),
                                           [&] { QVERIFY(peer.start(tool, arguments)); });
            const bool peerOk = peer.finish();
            QVERIFY2(outcome.result, qPrintable(outcome.describe() + QLatin1Char('\n') + peer.diagnostics()));
            QVERIFY2(peerOk, qPrintable(peer.diagnostics()));
            received = readFile(QDir(ours).filePath(QStringLiteral("payload.bin")));
        }
        if (xmodem) {
            // XMODEM pads to a whole block, with SUB (lrzsz) or SUB (us).
            QVERIFY(received.startsWith(content));
            QVERIFY(received.size() - content.size() < 1024);
        } else {
            QCOMPARE(received.size(), content.size());
            QCOMPARE(received, content);
        }
    }

    // -- Kermit --------------------------------------------------------------

    void kermit_data()
    {
        QTest::addColumn<bool>("portsideSends");
        QTest::addColumn<QByteArray>("content");
        for (const bool portsideSends : {true, false}) {
            for (const Content &content : contents()) {
                QTest::addRow("%s %s", portsideSends ? "to kermit" : "from kermit", content.name)
                    << portsideSends << content.bytes;
            }
        }
    }

    void kermit()
    {
        QFETCH(bool, portsideSends);
        QFETCH(QByteArray, content);
        const QString gkermit = findTool({QStringLiteral("gkermit")});
        const QString ckermit = findTool({QStringLiteral("kermit"), QStringLiteral("ckermit")});
        if (gkermit.isEmpty() && ckermit.isEmpty())
            QSKIP("Kermit is not installed: neither gkermit nor kermit was found (apt install gkermit ckermit)");

        QTemporaryDir dir;
        const QString theirs = dir.filePath(QStringLiteral("theirs"));
        const QString ours = dir.filePath(QStringLiteral("ours"));
        QVERIFY(QDir().mkpath(theirs));
        QVERIFY(QDir().mkpath(ours));
        auto session = std::make_unique<Session>(std::make_unique<SerialTransport>(m_pair.left()));
        session->open();
        // G-Kermit talks over its standard input and output.  C-Kermit in
        // remote mode would use its controlling terminal instead, so it opens
        // the port itself (-l), in binary mode (-i), ignoring the carrier a
        // PTY never raises.
        const bool useG = !gkermit.isEmpty();
        Peer peer(m_pair.right(), theirs, useG);
        const QString program = useG ? gkermit : ckermit;
        const QStringList base =
            useG ? QStringList{QStringLiteral("-q"), QStringLiteral("-i")}
                 : QStringList{QStringLiteral("-Y"), QStringLiteral("-q"),
                               QStringLiteral("-C"), QStringLiteral("set carrier-watch off, set flow none"),
                               QStringLiteral("-l"), m_pair.right(), QStringLiteral("-b"),
                               QStringLiteral("115200"), QStringLiteral("-i")};
        QByteArray received;
        if (portsideSends) {
            const QString path = dir.filePath(QStringLiteral("payload.bin"));
            writeFile(path, content);
            QVERIFY(peer.start(program, base + QStringList{QStringLiteral("-r")}));
            const Outcome outcome = runJob(session.get(), TransferDirection::Send, {path},
                                           optionsFor(Protocol::Kermit), {});
            const bool peerOk = peer.finish();
            QVERIFY2(outcome.result, qPrintable(outcome.describe() + QLatin1Char('\n') + peer.diagnostics()));
            QVERIFY2(peerOk, qPrintable(peer.diagnostics()));
            // G-Kermit converts names to lower case, C-Kermit may not: find
            // whatever arrived.
            const QStringList arrived = QDir(theirs).entryList(QDir::Files);
            QCOMPARE(arrived.size(), 1);
            received = readFile(QDir(theirs).filePath(arrived.first()));
        } else {
            writeFile(QDir(theirs).filePath(QStringLiteral("payload.bin")), content);
            const Outcome outcome =
                runJob(session.get(), TransferDirection::Receive, {},
                       optionsFor(Protocol::Kermit, ours), [&] {
                           QVERIFY(peer.start(program, base + QStringList{QStringLiteral("-s"),
                                                                          QStringLiteral("payload.bin")}));
                       });
            const bool peerOk = peer.finish();
            QVERIFY2(outcome.result, qPrintable(outcome.describe() + QLatin1Char('\n') + peer.diagnostics()));
            QVERIFY2(peerOk, qPrintable(peer.diagnostics()));
            const QStringList arrived = QDir(ours).entryList(QDir::Files);
            QCOMPARE(arrived.size(), 1);
            received = readFile(QDir(ours).filePath(arrived.first()));
        }
        QCOMPARE(received.size(), content.size());
        QCOMPARE(received, content);
    }

private:
    SocatPair m_pair;
};

QTEST_GUILESS_MAIN(TestInterop)
#include "tst_interop.moc"
