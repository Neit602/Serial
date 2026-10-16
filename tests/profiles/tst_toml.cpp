// The TOML reader and writer behind profiles.

#include "core/errors.h"
#include "core/toml.h"

#include <QTest>

#include <cmath>

using namespace portside;

namespace {

QString errorFor(const QString &text)
{
    try {
        parseToml(text);
    } catch (const PortsideError &error) {
        return error.message();
    }
    return {};
}

} // namespace

class TestToml : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void scalars()
    {
        const QVariantMap data = parseToml(QStringLiteral(
            "int = 42\nneg = -17\nhex = 0x1C200\noct = 0o17\nbin = 0b101\nunder = 115_200\n"
            "float = 0.25\nexp = 5e-2\ninf = inf\nyes = true\nno = false\n"));
        QCOMPARE(data.value(QStringLiteral("int")).toLongLong(), 42);
        QCOMPARE(data.value(QStringLiteral("neg")).toLongLong(), -17);
        QCOMPARE(data.value(QStringLiteral("hex")).toLongLong(), 115200);
        QCOMPARE(data.value(QStringLiteral("oct")).toLongLong(), 15);
        QCOMPARE(data.value(QStringLiteral("bin")).toLongLong(), 5);
        QCOMPARE(data.value(QStringLiteral("under")).toLongLong(), 115200);
        QCOMPARE(data.value(QStringLiteral("float")).toDouble(), 0.25);
        QCOMPARE(data.value(QStringLiteral("exp")).toDouble(), 0.05);
        QVERIFY(std::isinf(data.value(QStringLiteral("inf")).toDouble()));
        QCOMPARE(data.value(QStringLiteral("yes")).typeId(), QMetaType::Bool);
        QVERIFY(!data.value(QStringLiteral("no")).toBool());
        QCOMPARE(data.value(QStringLiteral("int")).typeId(), QMetaType::LongLong);
        QCOMPARE(data.value(QStringLiteral("float")).typeId(), QMetaType::Double);
    }

    void strings()
    {
        const QVariantMap data = parseToml(QStringLiteral(
            "basic = \"tab\\tquote\\\" \\u00e9\"\n"
            "literal = '[>#]\\s*$'\n"
            "prompt = \"[>#]\\\\s*$\"\n"
            "multi = \"\"\"\nline one\nline \\\n    two\"\"\"\n"
            "raw = '''\nC:\\path\n'''\n"));
        QCOMPARE(data.value(QStringLiteral("basic")).toString(), QStringLiteral("tab\tquote\" é"));
        QCOMPARE(data.value(QStringLiteral("literal")).toString(), QStringLiteral("[>#]\\s*$"));
        QCOMPARE(data.value(QStringLiteral("prompt")).toString(), QStringLiteral("[>#]\\s*$"));
        QCOMPARE(data.value(QStringLiteral("multi")).toString(), QStringLiteral("line one\nline two"));
        QCOMPARE(data.value(QStringLiteral("raw")).toString(), QStringLiteral("C:\\path\n"));
    }

    void tablesArraysAndComments()
    {
        const QVariantMap data = parseToml(QStringLiteral(
            "# a profile\n"
            "name = \"lab\"   # trailing comment\n"
            "keys = [\n  \"a\",  # first\n  \"b\",\n]\n"
            "empty = []\n"
            "[serial]\nbaudrate = 9600\n"
            "[a.b]\nc = 1\n"
            "[inline]\npoint = { x = 1, y = 2 }\n"
            "dotted.key = \"v\"\n"
            "[[ports]]\nname = \"one\"\n[[ports]]\nname = \"two\"\n"));
        QCOMPARE(data.value(QStringLiteral("name")).toString(), QStringLiteral("lab"));
        QCOMPARE(data.value(QStringLiteral("keys")).toStringList(), (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
        QVERIFY(data.value(QStringLiteral("empty")).toList().isEmpty());
        QCOMPARE(data.value(QStringLiteral("serial")).toMap().value(QStringLiteral("baudrate")).toInt(), 9600);
        QCOMPARE(data.value(QStringLiteral("a")).toMap().value(QStringLiteral("b")).toMap().value(QStringLiteral("c")).toInt(), 1);
        const QVariantMap inlineTable = data.value(QStringLiteral("inline")).toMap();
        QCOMPARE(inlineTable.value(QStringLiteral("point")).toMap().value(QStringLiteral("y")).toInt(), 2);
        QCOMPARE(inlineTable.value(QStringLiteral("dotted")).toMap().value(QStringLiteral("key")).toString(), QStringLiteral("v"));
        const QVariantList ports = data.value(QStringLiteral("ports")).toList();
        QCOMPARE(ports.size(), 2);
        QCOMPARE(ports.at(1).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("two"));
    }

    void crlfFilesParse()
    {
        const QVariantMap data = parseToml(QStringLiteral("a = 1\r\n[t]\r\nb = 2\r\n"));
        QCOMPARE(data.value(QStringLiteral("t")).toMap().value(QStringLiteral("b")).toInt(), 2);
    }

    void errorsNameTheLine_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("message");
        QTest::newRow("duplicate") << QStringLiteral("a = 1\na = 2\n") << QStringLiteral("line 2: duplicate key 'a'");
        QTest::newRow("unterminated") << QStringLiteral("\n\na = \"open\n") << QStringLiteral("line 3: unterminated string");
        QTest::newRow("garbage after value") << QStringLiteral("a = 1 2\n") << QStringLiteral("line 1: unexpected '2' after a value");
        QTest::newRow("no value") << QStringLiteral("a =\n") << QStringLiteral("line 1: expected a value");
        QTest::newRow("table twice") << QStringLiteral("[t]\n[t]\n") << QStringLiteral("line 2: table [t] is defined twice");
        QTest::newRow("date") << QStringLiteral("when = 1979-05-27\n") << QStringLiteral("line 1: dates and times are not supported in profiles");
        QTest::newRow("leading zero") << QStringLiteral("a = 0123\n") << QStringLiteral("line 1: leading zeros are not allowed in '0123'");
        QTest::newRow("bad escape") << QStringLiteral("a = \"\\q\"\n") << QStringLiteral("line 1: invalid escape '\\q'");
    }

    void errorsNameTheLine()
    {
        QFETCH(QString, text);
        QFETCH(QString, message);
        QCOMPARE(errorFor(text), message);
    }

    void theWriterRoundTrips()
    {
        TomlWriter writer;
        writer.value(QStringLiteral("name"), QStringLiteral("quote \" back\\slash\nnew"));
        writer.value(QStringLiteral("count"), 3);
        writer.value(QStringLiteral("delay"), 0.05);
        writer.value(QStringLiteral("whole"), 5.0);
        writer.value(QStringLiteral("flag"), true);
        writer.value(QStringLiteral("list"), QStringList{QStringLiteral("a"), QStringLiteral("b")});
        writer.value(QStringLiteral("odd key"), QStringLiteral("x"));
        writer.table(QStringLiteral("serial"));
        writer.value(QStringLiteral("parity"), QStringLiteral("N"));
        const QString text = writer.text();
        QVERIFY(text.contains(QStringLiteral("delay = 0.05\n")));
        QVERIFY(text.contains(QStringLiteral("whole = 5.0\n")));
        QVERIFY(text.contains(QStringLiteral("\"odd key\" = \"x\"\n")));
        QVERIFY(text.contains(QStringLiteral("\n[serial]\nparity = \"N\"\n")));
        const QVariantMap data = parseToml(text);
        QCOMPARE(data.value(QStringLiteral("name")).toString(), QStringLiteral("quote \" back\\slash\nnew"));
        QCOMPARE(data.value(QStringLiteral("count")).toInt(), 3);
        QCOMPARE(data.value(QStringLiteral("delay")).toDouble(), 0.05);
        QCOMPARE(data.value(QStringLiteral("whole")).typeId(), QMetaType::Double);
        QCOMPARE(data.value(QStringLiteral("list")).toStringList(), (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
    }
};

QTEST_GUILESS_MAIN(TestToml)
#include "tst_toml.moc"
