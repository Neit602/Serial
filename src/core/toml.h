#pragma once

// A small TOML 1.0 reader and writer for profiles (PRD F20).
//
// Profiles are meant to be edited by hand and kept in git, so the reader
// accepts what people actually write: comments, bare/quoted/dotted keys, all
// four string forms, integers in any base, floats, booleans, arrays (with
// trailing commas and comments), inline tables, [tables] and [[arrays of
// tables]].  Dates and times are rejected with a clear message — nothing in a
// profile is a date.
//
// Tables come back as QVariantMap, arrays as QVariantList, integers as
// qint64, floats as double.

#include <QString>
#include <QVariant>
#include <QVariantMap>

namespace portside {

// Parse a TOML document.  Throws PortsideError(Profile) whose message starts
// with "line N:" on malformed input.
QVariantMap parseToml(const QString &text);

// Builds a TOML document in the order values are added (QVariantMap would
// sort the keys, and a profile reads better with `name` first).
class TomlWriter
{
public:
    // key = value, in the current table.
    void value(const QString &key, const QVariant &value);
    // Start a [name] table.
    void table(const QString &name);
    QString text() const { return m_text; }

    static QString formatKey(const QString &key);
    static QString formatValue(const QVariant &value);

private:
    QString m_text;
};

} // namespace portside
