// Incremental decoding (F10, F11).

#include "core/decoder.h"

#include <QTest>

using namespace portside;

class TestDecoder : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aCharacterSplitAcrossReadsDecodesOnce()
    {
        IncrementalTextDecoder decoder;
        QCOMPARE(decoder.decodeToString("caf\xc3"), QStringLiteral("caf"));
        QCOMPARE(decoder.decodeToString("\xa9"), QStringLiteral("é"));
        QCOMPARE(decoder.decodeToString("\xf0\x9f"), QString());
        QCOMPARE(decoder.decodeToString("\x98\x80"), QString::fromUtf8("\xf0\x9f\x98\x80"));
    }

    void invalidBytesBecomeReplacementCharacters_data()
    {
        QTest::addColumn<QByteArray>("input");
        QTest::addColumn<QString>("expected");
        const QString r(QChar(0xFFFD));
        QTest::newRow("lone continuation") << QByteArray("a\x80z") << QStringLiteral("a") + r + QStringLiteral("z");
        QTest::newRow("overlong") << QByteArray("\xc0\xaf") << r + r;
        QTest::newRow("surrogate") << QByteArray("\xed\xa0\x80") << r + r + r;
        QTest::newRow("truncated then ascii") << QByteArray("\xe2\x82x") << r + QStringLiteral("x");
        QTest::newRow("beyond U+10FFFF") << QByteArray("\xf4\x90\x80\x80") << r + r + r + r;
        QTest::newRow("line noise") << QByteArray("\xff\xfe") << r + r;
    }

    void invalidBytesBecomeReplacementCharacters()
    {
        QFETCH(QByteArray, input);
        QFETCH(QString, expected);
        IncrementalTextDecoder decoder;
        QCOMPARE(decoder.decodeToString(input), expected);
    }

    void aFinalDecodeFlushesAnIncompleteCharacter()
    {
        IncrementalTextDecoder decoder;
        QCOMPARE(decoder.decodeToString("ab\xc3", true), QStringLiteral("ab") + QChar(0xFFFD));
        QCOMPARE(decoder.decodeToString("c"), QStringLiteral("c"));
    }

    void singleByteCharacterSets()
    {
        IncrementalTextDecoder ascii(CharacterSet::Ascii);
        QCOMPARE(ascii.decodeToString("A\xe9"), QStringLiteral("A") + QChar(0xFFFD));
        IncrementalTextDecoder latin1(CharacterSet::Latin1);
        QCOMPARE(latin1.decodeToString("caf\xe9"), QStringLiteral("café"));
        IncrementalTextDecoder cp437(CharacterSet::Cp437);
        QCOMPARE(cp437.decodeToString("\xc9\xcd\xbb\xdb"), QStringLiteral("╔═╗█"));
        IncrementalTextDecoder cp850(CharacterSet::Cp850);
        QCOMPARE(cp850.decodeToString("\x9b\xd5"), QStringLiteral("øı"));
    }

    void controlCharactersSurvive()
    {
        IncrementalTextDecoder decoder(CharacterSet::Cp437);
        QCOMPARE(decoder.decodeToString("\x1b[0m\r\n\x07"), QStringLiteral("\x1b[0m\r\n\x07"));
    }

    void strippingTheHighBitTurnsSevenBitOutputBackIntoText()
    {
        IncrementalTextDecoder decoder(CharacterSet::Utf8, true);
        QCOMPARE(decoder.decodeToString(QByteArray("\xc8\xc9")), QStringLiteral("HI"));
        decoder.setStripHighBit(false);
        QVERIFY(!decoder.stripHighBit());
    }

    void switchingCharsetDropsAHalfCharacter()
    {
        IncrementalTextDecoder decoder;
        decoder.decodeToString("\xc3");
        decoder.setCharset(CharacterSet::Latin1);
        QCOMPARE(decoder.decodeToString("\xa9"), QStringLiteral("©"));
        QCOMPARE(decoder.charset(), CharacterSet::Latin1);
    }

    void enumNamesMatchTheProfileSpelling()
    {
        QCOMPARE(enumName(CharacterSet::Latin1), QStringLiteral("latin-1"));
        QCOMPARE(enumFromName<CharacterSet>(QStringLiteral("cp850")), std::optional(CharacterSet::Cp850));
        QVERIFY(!enumFromName<CharacterSet>(QStringLiteral("ebcdic")));
    }
};

QTEST_GUILESS_MAIN(TestDecoder)
#include "tst_decoder.moc"
