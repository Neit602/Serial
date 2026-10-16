#include "core/toml.h"

#include "core/errors.h"

#include <QLocale>
#include <QStringList>
#include <QVariantList>

#include <cmath>
#include <functional>
#include <limits>

namespace portside {

namespace {

class Parser
{
public:
    explicit Parser(const QString &text)
        : m_text(text)
    {
    }

    QVariantMap parse()
    {
        QStringList currentPath;
        bool currentIsArrayItem = false;
        while (!atEnd()) {
            skipWhitespaceAndNewlines();
            if (atEnd())
                break;
            m_statementLine = m_line;
            const QChar c = peek();
            if (c == QLatin1Char('[')) {
                const bool arrayOfTables = peek(1) == QLatin1Char('[');
                advance(arrayOfTables ? 2 : 1);
                skipInlineWhitespace();
                const QStringList path = parseKey();
                skipInlineWhitespace();
                expect(QLatin1Char(']'));
                if (arrayOfTables)
                    expect(QLatin1Char(']'));
                endOfLine();
                if (arrayOfTables)
                    appendTableArray(path);
                else
                    defineTable(path);
                currentPath = path;
                currentIsArrayItem = arrayOfTables;
                continue;
            }
            const QStringList key = parseKey();
            skipInlineWhitespace();
            expect(QLatin1Char('='));
            skipInlineWhitespace();
            const QVariant value = parseValue();
            endOfLine();
            QStringList full = currentPath;
            full += key;
            assign(full, value, currentIsArrayItem ? currentPath.size() : -1);
        }
        return m_root;
    }

private:
    [[noreturn]] void fail(const QString &message) const { failAt(m_line, message); }

    // Errors found after a statement was read (duplicates, redefinitions)
    // name the line the statement started on.
    [[noreturn]] void failAt(int line, const QString &message) const
    {
        throw PortsideError(ErrorKind::Profile, QStringLiteral("line %1: %2").arg(line).arg(message));
    }

    bool atEnd() const { return m_pos >= m_text.size(); }
    QChar peek(qsizetype ahead = 0) const
    {
        return m_pos + ahead < m_text.size() ? m_text.at(m_pos + ahead) : QChar();
    }
    void advance(qsizetype count = 1)
    {
        for (qsizetype i = 0; i < count && !atEnd(); ++i) {
            if (m_text.at(m_pos) == QLatin1Char('\n'))
                ++m_line;
            ++m_pos;
        }
    }
    void expect(QChar c)
    {
        if (peek() != c)
            fail(QStringLiteral("expected '%1'").arg(c));
        advance();
    }

    void skipInlineWhitespace()
    {
        while (peek() == QLatin1Char(' ') || peek() == QLatin1Char('\t'))
            advance();
    }

    void skipComment()
    {
        if (peek() != QLatin1Char('#'))
            return;
        while (!atEnd() && peek() != QLatin1Char('\n'))
            advance();
    }

    void skipWhitespaceAndNewlines()
    {
        for (;;) {
            skipInlineWhitespace();
            skipComment();
            if (peek() == QLatin1Char('\r') && peek(1) == QLatin1Char('\n')) {
                advance(2);
            } else if (peek() == QLatin1Char('\n')) {
                advance();
            } else {
                return;
            }
        }
    }

    void endOfLine()
    {
        skipInlineWhitespace();
        skipComment();
        if (atEnd())
            return;
        if (peek() == QLatin1Char('\r') && peek(1) == QLatin1Char('\n')) {
            advance(2);
            return;
        }
        if (peek() == QLatin1Char('\n')) {
            advance();
            return;
        }
        fail(QStringLiteral("unexpected '%1' after a value").arg(peek()));
    }

    // -- keys ----------------------------------------------------------------

    static bool isBareKeyChar(QChar c)
    {
        return (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
               || (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
               || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char('_')
               || c == QLatin1Char('-');
    }

    QStringList parseKey()
    {
        QStringList parts;
        for (;;) {
            skipInlineWhitespace();
            const QChar c = peek();
            if (c == QLatin1Char('"')) {
                parts << parseBasicString();
            } else if (c == QLatin1Char('\'')) {
                parts << parseLiteralString();
            } else if (isBareKeyChar(c)) {
                QString bare;
                while (isBareKeyChar(peek())) {
                    bare += peek();
                    advance();
                }
                parts << bare;
            } else {
                fail(QStringLiteral("expected a key"));
            }
            skipInlineWhitespace();
            if (peek() != QLatin1Char('.'))
                return parts;
            advance();
        }
    }

    // -- strings -------------------------------------------------------------

    QString parseEscape()
    {
        advance(); // the backslash
        const QChar c = peek();
        advance();
        switch (c.unicode()) {
        case 'b':
            return QStringLiteral("\b");
        case 't':
            return QStringLiteral("\t");
        case 'n':
            return QStringLiteral("\n");
        case 'f':
            return QStringLiteral("\f");
        case 'r':
            return QStringLiteral("\r");
        case 'e':
            return QStringLiteral("\x1b");
        case '"':
            return QStringLiteral("\"");
        case '\\':
            return QStringLiteral("\\");
        case 'u':
        case 'U': {
            const int digits = c == QLatin1Char('u') ? 4 : 8;
            const QString hex = m_text.mid(m_pos, digits);
            bool ok = false;
            const uint code = hex.toUInt(&ok, 16);
            if (!ok || hex.size() != digits || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
                fail(QStringLiteral("invalid unicode escape"));
            advance(digits);
            const char32_t point = code;
            return QString::fromUcs4(&point, 1);
        }
        default:
            fail(QStringLiteral("invalid escape '\\%1'").arg(c));
        }
    }

    QString parseBasicString()
    {
        if (m_text.mid(m_pos, 3) == QLatin1String("\"\"\""))
            return parseMultilineBasicString();
        advance(); // opening quote
        QString out;
        for (;;) {
            if (atEnd() || peek() == QLatin1Char('\n'))
                fail(QStringLiteral("unterminated string"));
            const QChar c = peek();
            if (c == QLatin1Char('"')) {
                advance();
                return out;
            }
            if (c == QLatin1Char('\\')) {
                out += parseEscape();
                continue;
            }
            out += c;
            advance();
        }
    }

    QString parseMultilineBasicString()
    {
        advance(3);
        if (peek() == QLatin1Char('\r') && peek(1) == QLatin1Char('\n'))
            advance(2);
        else if (peek() == QLatin1Char('\n'))
            advance();
        QString out;
        for (;;) {
            if (atEnd())
                fail(QStringLiteral("unterminated multi-line string"));
            if (m_text.mid(m_pos, 3) == QLatin1String("\"\"\"")) {
                advance(3);
                // Up to two quotes may end the content just before the close.
                while (peek() == QLatin1Char('"')) {
                    out += QLatin1Char('"');
                    advance();
                }
                return out;
            }
            const QChar c = peek();
            if (c == QLatin1Char('\\')) {
                // A line-ending backslash swallows the newline and the
                // whitespace that follows.
                qsizetype look = m_pos + 1;
                while (look < m_text.size() && (m_text.at(look) == QLatin1Char(' ') || m_text.at(look) == QLatin1Char('\t')))
                    ++look;
                if (look < m_text.size() && (m_text.at(look) == QLatin1Char('\n') || m_text.at(look) == QLatin1Char('\r'))) {
                    advance(look - m_pos);
                    while (!atEnd() && peek().isSpace())
                        advance();
                    continue;
                }
                out += parseEscape();
                continue;
            }
            out += c;
            advance();
        }
    }

    QString parseLiteralString()
    {
        if (m_text.mid(m_pos, 3) == QLatin1String("'''")) {
            advance(3);
            if (peek() == QLatin1Char('\r') && peek(1) == QLatin1Char('\n'))
                advance(2);
            else if (peek() == QLatin1Char('\n'))
                advance();
            QString out;
            for (;;) {
                if (atEnd())
                    fail(QStringLiteral("unterminated multi-line string"));
                if (m_text.mid(m_pos, 3) == QLatin1String("'''")) {
                    advance(3);
                    while (peek() == QLatin1Char('\'')) {
                        out += QLatin1Char('\'');
                        advance();
                    }
                    return out;
                }
                out += peek();
                advance();
            }
        }
        advance();
        QString out;
        for (;;) {
            if (atEnd() || peek() == QLatin1Char('\n'))
                fail(QStringLiteral("unterminated string"));
            if (peek() == QLatin1Char('\'')) {
                advance();
                return out;
            }
            out += peek();
            advance();
        }
    }

    // -- values --------------------------------------------------------------

    QVariant parseValue()
    {
        const QChar c = peek();
        if (c == QLatin1Char('"'))
            return parseBasicString();
        if (c == QLatin1Char('\''))
            return parseLiteralString();
        if (c == QLatin1Char('['))
            return parseArray();
        if (c == QLatin1Char('{'))
            return parseInlineTable();
        if (m_text.mid(m_pos, 4) == QLatin1String("true") && !isBareKeyChar(peek(4))) {
            advance(4);
            return true;
        }
        if (m_text.mid(m_pos, 5) == QLatin1String("false") && !isBareKeyChar(peek(5))) {
            advance(5);
            return false;
        }
        return parseNumber();
    }

    QVariant parseNumber()
    {
        QString token;
        while (!atEnd()) {
            const QChar c = peek();
            if (c.isLetterOrNumber() || c == QLatin1Char('+') || c == QLatin1Char('-')
                || c == QLatin1Char('.') || c == QLatin1Char('_') || c == QLatin1Char(':')) {
                token += c;
                advance();
            } else {
                break;
            }
        }
        if (token.isEmpty())
            fail(QStringLiteral("expected a value"));
        if (token.contains(QLatin1Char(':')) || (token.size() >= 10 && token.at(4) == QLatin1Char('-')
                                                 && token.at(7) == QLatin1Char('-'))) {
            fail(QStringLiteral("dates and times are not supported in profiles"));
        }

        QString sign;
        QString body = token;
        if (body.startsWith(QLatin1Char('+')) || body.startsWith(QLatin1Char('-'))) {
            sign = body.left(1);
            body = body.mid(1);
        }
        if (body == QLatin1String("inf"))
            return sign == QLatin1String("-") ? -std::numeric_limits<double>::infinity()
                                              : std::numeric_limits<double>::infinity();
        if (body == QLatin1String("nan"))
            return std::numeric_limits<double>::quiet_NaN();

        if (body.contains(QLatin1String("__")) || body.startsWith(QLatin1Char('_'))
            || body.endsWith(QLatin1Char('_'))) {
            fail(QStringLiteral("invalid number '%1'").arg(token));
        }
        const QString digits = QString(body).remove(QLatin1Char('_'));

        int base = 10;
        QString magnitude = digits;
        if (digits.startsWith(QLatin1String("0x")))
            base = 16;
        else if (digits.startsWith(QLatin1String("0o")))
            base = 8;
        else if (digits.startsWith(QLatin1String("0b")))
            base = 2;
        if (base != 10) {
            if (!sign.isEmpty())
                fail(QStringLiteral("a sign is not allowed on '%1'").arg(token));
            bool ok = false;
            const qint64 value = digits.mid(2).toLongLong(&ok, base);
            if (!ok)
                fail(QStringLiteral("invalid number '%1'").arg(token));
            return value;
        }

        const bool isFloat = digits.contains(QLatin1Char('.')) || digits.contains(QLatin1Char('e'))
                             || digits.contains(QLatin1Char('E'));
        if (isFloat) {
            bool ok = false;
            const double value = QLocale::c().toDouble(sign + digits, &ok);
            if (!ok || digits.startsWith(QLatin1Char('.')) || digits.endsWith(QLatin1Char('.')))
                fail(QStringLiteral("invalid number '%1'").arg(token));
            return value;
        }
        if (digits.size() > 1 && digits.startsWith(QLatin1Char('0')))
            fail(QStringLiteral("leading zeros are not allowed in '%1'").arg(token));
        bool ok = false;
        const qint64 value = (sign + digits).toLongLong(&ok, 10);
        if (!ok)
            fail(QStringLiteral("invalid value '%1'").arg(token));
        return value;
    }

    QVariant parseArray()
    {
        advance(); // [
        QVariantList items;
        for (;;) {
            skipWhitespaceAndNewlines();
            if (peek() == QLatin1Char(']')) {
                advance();
                return items;
            }
            items << parseValue();
            skipWhitespaceAndNewlines();
            if (peek() == QLatin1Char(',')) {
                advance();
                continue;
            }
            if (peek() == QLatin1Char(']')) {
                advance();
                return items;
            }
            fail(QStringLiteral("expected ',' or ']' in an array"));
        }
    }

    QVariant parseInlineTable()
    {
        advance(); // {
        QVariantMap table;
        skipInlineWhitespace();
        if (peek() == QLatin1Char('}')) {
            advance();
            return table;
        }
        for (;;) {
            const QStringList key = parseKey();
            skipInlineWhitespace();
            expect(QLatin1Char('='));
            skipInlineWhitespace();
            const QVariant value = parseValue();
            setPath(table, key, value);
            skipInlineWhitespace();
            if (peek() == QLatin1Char(',')) {
                advance();
                skipInlineWhitespace();
                continue;
            }
            expect(QLatin1Char('}'));
            return table;
        }
    }

    // -- tables --------------------------------------------------------------

    void setPath(QVariantMap &table, const QStringList &path, const QVariant &value)
    {
        if (path.size() == 1) {
            if (table.contains(path.first()))
                fail(QStringLiteral("duplicate key '%1'").arg(path.first()));
            table.insert(path.first(), value);
            return;
        }
        QVariant &child = table[path.first()];
        if (!child.isValid())
            child = QVariantMap();
        if (child.typeId() != QMetaType::QVariantMap)
            fail(QStringLiteral("'%1' is not a table").arg(path.first()));
        QVariantMap nested = child.toMap();
        setPath(nested, path.mid(1), value);
        child = nested;
    }

    // Walk to the table at `path` in `root`, creating tables on the way.  An
    // array of tables resolves to its last element.
    QVariantMap walk(QVariantMap table, const QStringList &path, qsizetype depth,
                     const std::function<QVariantMap(QVariantMap)> &leaf)
    {
        if (depth == path.size())
            return leaf(table);
        QVariant &child = table[path.at(depth)];
        if (!child.isValid())
            child = QVariantMap();
        if (child.typeId() == QMetaType::QVariantList) {
            QVariantList list = child.toList();
            if (list.isEmpty() || list.last().typeId() != QMetaType::QVariantMap)
                fail(QStringLiteral("'%1' is not a table").arg(path.at(depth)));
            list.last() = walk(list.last().toMap(), path, depth + 1, leaf);
            child = list;
        } else if (child.typeId() == QMetaType::QVariantMap) {
            child = walk(child.toMap(), path, depth + 1, leaf);
        } else {
            fail(QStringLiteral("'%1' is not a table").arg(path.at(depth)));
        }
        return table;
    }

    void defineTable(const QStringList &path)
    {
        const QString joined = path.join(QLatin1Char('.'));
        if (m_defined.contains(joined))
            failAt(m_statementLine, QStringLiteral("table [%1] is defined twice").arg(joined));
        m_defined << joined;
        m_root = walk(m_root, path, 0, [](QVariantMap table) { return table; });
    }

    void appendTableArray(const QStringList &path)
    {
        m_root = walk(m_root, path.mid(0, path.size() - 1), 0, [&](QVariantMap table) {
            QVariant &slot = table[path.last()];
            QVariantList list;
            if (slot.isValid()) {
                if (slot.typeId() != QMetaType::QVariantList)
                    fail(QStringLiteral("'%1' is not an array of tables").arg(path.last()));
                list = slot.toList();
            }
            list << QVariantMap();
            slot = list;
            return table;
        });
    }

    void assign(const QStringList &fullPath, const QVariant &value, qsizetype tableDepth)
    {
        Q_UNUSED(tableDepth);
        const QStringList tablePath = fullPath.mid(0, fullPath.size() - 1);
        const QString key = fullPath.last();
        m_root = walk(m_root, tablePath, 0, [&](QVariantMap table) {
            if (table.contains(key))
                failAt(m_statementLine, QStringLiteral("duplicate key '%1'").arg(key));
            table.insert(key, value);
            return table;
        });
    }

    const QString &m_text;
    qsizetype m_pos = 0;
    int m_line = 1;
    int m_statementLine = 1;
    QVariantMap m_root;
    QStringList m_defined;
};

} // namespace

QVariantMap parseToml(const QString &text)
{
    return Parser(text).parse();
}

void TomlWriter::value(const QString &key, const QVariant &value)
{
    m_text += formatKey(key) + QStringLiteral(" = ") + formatValue(value) + QLatin1Char('\n');
}

void TomlWriter::table(const QString &name)
{
    if (!m_text.isEmpty())
        m_text += QLatin1Char('\n');
    m_text += QLatin1Char('[') + formatKey(name) + QStringLiteral("]\n");
}

QString TomlWriter::formatKey(const QString &key)
{
    bool bare = !key.isEmpty();
    for (const QChar c : key) {
        const bool ok = (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
                        || (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
                        || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                        || c == QLatin1Char('_') || c == QLatin1Char('-');
        bare = bare && ok;
    }
    return bare ? key : formatValue(key);
}

QString TomlWriter::formatValue(const QVariant &value)
{
    switch (value.typeId()) {
    case QMetaType::Bool:
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Long:
    case QMetaType::ULong:
        return QString::number(value.toLongLong());
    case QMetaType::Double:
    case QMetaType::Float: {
        const double number = value.toDouble();
        if (std::isnan(number))
            return QStringLiteral("nan");
        if (std::isinf(number))
            return number > 0 ? QStringLiteral("inf") : QStringLiteral("-inf");
        QString text = QString::number(number, 'g', QLocale::FloatingPointShortest);
        if (!text.contains(QLatin1Char('.')) && !text.contains(QLatin1Char('e')))
            text += QStringLiteral(".0");
        return text;
    }
    case QMetaType::QStringList:
    case QMetaType::QVariantList: {
        QStringList items;
        for (const QVariant &item : value.toList())
            items << formatValue(item);
        return QLatin1Char('[') + items.join(QStringLiteral(", ")) + QLatin1Char(']');
    }
    default:
        break;
    }
    const QString text = value.toString();
    QString out = QStringLiteral("\"");
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '"':
            out += QStringLiteral("\\\"");
            break;
        case '\\':
            out += QStringLiteral("\\\\");
            break;
        case '\b':
            out += QStringLiteral("\\b");
            break;
        case '\t':
            out += QStringLiteral("\\t");
            break;
        case '\n':
            out += QStringLiteral("\\n");
            break;
        case '\f':
            out += QStringLiteral("\\f");
            break;
        case '\r':
            out += QStringLiteral("\\r");
            break;
        default:
            if (c.unicode() < 0x20 || c.unicode() == 0x7F)
                out += QStringLiteral("\\u%1").arg(uint(c.unicode()), 4, 16, QLatin1Char('0'));
            else
                out += c;
        }
    }
    out += QLatin1Char('"');
    return out;
}

} // namespace portside
