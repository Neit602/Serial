#pragma once

// Incremental byte→text decoding (PRD F11, and the high-bit option of F10).
//
// Serial data arrives in arbitrary chunks, so a multi-byte character is
// regularly split across two reads.  A stateful decoder is therefore not a
// nicety: decoding each chunk independently would corrupt every UTF-8
// character that straddles a read boundary.
//
// Undecodable bytes become U+FFFD.  A terminal that rejects bad input is
// useless on a serial line, where line noise and wrong baud rates are routine.

#include "core/enums.h"

#include <QByteArray>
#include <QString>

#include <string>

namespace portside {

enum class CharacterSet { Utf8, Ascii, Latin1, Cp437, Cp850 };

template <>
struct EnumNames<CharacterSet>
{
    static constexpr std::array<std::pair<CharacterSet, std::string_view>, 5> table{{
        {CharacterSet::Utf8, "utf-8"},
        {CharacterSet::Ascii, "ascii"},
        {CharacterSet::Latin1, "latin-1"},
        {CharacterSet::Cp437, "cp437"},
        {CharacterSet::Cp850, "cp850"},
    }};
};

inline constexpr char32_t kReplacementChar = 0xFFFD;

class IncrementalTextDecoder
{
public:
    explicit IncrementalTextDecoder(CharacterSet charset = CharacterSet::Utf8,
                                    bool stripHighBit = false);

    CharacterSet charset() const { return m_charset; }
    // Switch character set, discarding any half-decoded character.
    void setCharset(CharacterSet charset);

    bool stripHighBit() const { return m_stripHighBit; }
    // Mask bit 7 off every byte before decoding (F10).  Some 7-bit devices set
    // it as parity or as a marker; stripping it turns their output back into
    // text.
    void setStripHighBit(bool strip) { m_stripHighBit = strip; }

    // Decode `data` to code points, keeping an incomplete trailing character
    // for the next call.  With `final` an incomplete sequence is flushed as
    // U+FFFD instead, which is what a session close wants.
    std::u32string decode(QByteArrayView data, bool final = false);
    QString decodeToString(QByteArrayView data, bool final = false);

    // Forget any partially decoded character (used on reconnect).
    void reset();

private:
    void decodeUtf8(unsigned char byte, std::u32string &out);

    CharacterSet m_charset;
    bool m_stripHighBit;
    // UTF-8 state: the code point so far, bytes still needed, and the valid
    // range for the next continuation byte (it narrows after E0/ED/F0/F4).
    char32_t m_codePoint = 0;
    int m_needed = 0;
    int m_seen = 0;
    unsigned char m_lower = 0x80;
    unsigned char m_upper = 0xBF;
};

// Convert code points to a QString (UTF-16).
QString toQString(const std::u32string &text);

} // namespace portside
