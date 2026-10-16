// Find in the scrollback (F8) and the hex view (F10).

#include "core/errors.h"
#include "core/find.h"
#include "core/hexview.h"

#include <QTest>
#include <stdexcept>

using namespace portside;

class TestFindHex : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void searchIsLiteralByDefault()
    {
        const QStringList lines = {QStringLiteral("interface Gi1/0/1"), QStringLiteral("ip 10.0.0.1"),
                                   QStringLiteral("ip 10a0b0c1")};
        const QList<Match> dots = findAll(lines, QStringLiteral("10.0.0.1"));
        QCOMPARE(dots.size(), 1);
        QCOMPARE(dots.first(), (Match{1, 3, 11}));
        QCOMPARE(findAll(lines, QStringLiteral("Gi1/0/1")).size(), 1);
    }

    void caseInsensitiveUnlessAsked()
    {
        const QStringList lines = {QStringLiteral("ERROR error Error")};
        QCOMPARE(findAll(lines, QStringLiteral("error")).size(), 3);
        SearchOptions options;
        options.caseSensitive = true;
        QCOMPARE(findAll(lines, QStringLiteral("error"), options).size(), 1);
    }

    void regexAndWholeWordAreOptIn()
    {
        const QStringList lines = {QStringLiteral("Gi1/0/1 Gi1/0/12 Te1/1/1")};
        SearchOptions regex;
        regex.regex = true;
        QCOMPARE(findAll(lines, QStringLiteral("(Gi|Te)1/\\d/1\\b"), regex).size(), 2);
        SearchOptions word;
        word.wholeWord = true;
        QCOMPARE(findAll({QStringLiteral("cat concat cat.")}, QStringLiteral("cat"), word).size(), 2);
    }

    void aBadPatternIsATypedError()
    {
        SearchOptions regex;
        regex.regex = true;
        try {
            findAll({QStringLiteral("x")}, QStringLiteral("(["), regex);
            QFAIL("no error");
        } catch (const PortsideError &error) {
            QCOMPARE(error.kind(), ErrorKind::InvalidSettings);
        }
    }

    void anEmptyQueryFindsNothing()
    {
        QVERIFY(findAll({QStringLiteral("x")}, QString()).isEmpty());
    }

    void nextAndPreviousWrapAround()
    {
        const QList<Match> matches = {{0, 1, 2}, {2, 0, 1}, {5, 3, 4}};
        QCOMPARE(*findNext(matches, std::nullopt), matches.at(0));
        QCOMPARE(*findNext(matches, matches.at(0)), matches.at(1));
        QCOMPARE(*findNext(matches, matches.at(2)), matches.at(0));
        QCOMPARE(*findPrevious(matches, std::nullopt), matches.at(2));
        QCOMPARE(*findPrevious(matches, matches.at(0)), matches.at(2));
        QVERIFY(!findNext({}, std::nullopt));
    }

    void hexRowsLookLikeHexdump()
    {
        const QList<HexRow> rows = hexRows(QByteArray("Hello, world!\r\n\x00\x7f", 17), 0x10);
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.at(0).render(),
                 QStringLiteral("00000010  48 65 6c 6c 6f 2c 20 77  6f 72 6c 64 21 0d 0a 00  |Hello, world!...|"));
        // 16 columns of "xx " minus the last space, plus the middle gap: 48.
        QCOMPARE(rows.at(1).render(),
                 QStringLiteral("00000020  7f") + QString(46, QLatin1Char(' ')) + QStringLiteral("  |.|"));
    }

    void strippingTheHighBitMatchesTheTextView()
    {
        QCOMPARE(formatHex(QByteArray("\xc8\xc9"), 0, 16, true),
                 QStringLiteral("00000000  48 49") + QString(43, QLatin1Char(' ')) + QStringLiteral("  |HI|"));
    }

    void widthMustBePositive()
    {
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, hexRows("x", 0, 0));
    }
};

QTEST_GUILESS_MAIN(TestFindHex)
#include "tst_find_hex.moc"
