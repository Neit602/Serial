#include "core/find.h"

#include "core/errors.h"

#include <QRegularExpression>

namespace portside {

QList<Match> findAll(const QStringList &lines, const QString &query, const SearchOptions &options)
{
    if (query.isEmpty())
        return {};
    QString pattern = options.regex ? query : QRegularExpression::escape(query);
    if (options.wholeWord)
        pattern = QStringLiteral("\\b(?:%1)\\b").arg(pattern);
    QRegularExpression::PatternOptions flags = QRegularExpression::UseUnicodePropertiesOption;
    if (!options.caseSensitive)
        flags |= QRegularExpression::CaseInsensitiveOption;
    const QRegularExpression expression(pattern, flags);
    if (!expression.isValid()) {
        throw invalidSettings(QStringLiteral("invalid search pattern '%1': %2")
                                  .arg(query, expression.errorString()));
    }

    QList<Match> matches;
    for (int index = 0; index < lines.size(); ++index) {
        auto it = expression.globalMatch(lines.at(index));
        while (it.hasNext()) {
            const auto found = it.next();
            if (found.capturedLength() > 0)
                matches.append({index, int(found.capturedStart()), int(found.capturedEnd())});
        }
    }
    return matches;
}

namespace {

bool before(const Match &a, const Match &b)
{
    return a.line < b.line || (a.line == b.line && a.start < b.start);
}

} // namespace

std::optional<Match> findNext(const QList<Match> &matches, const std::optional<Match> &current)
{
    if (matches.isEmpty())
        return std::nullopt;
    if (!current)
        return matches.first();
    for (const Match &match : matches) {
        if (before(*current, match))
            return match;
    }
    return matches.first();
}

std::optional<Match> findPrevious(const QList<Match> &matches, const std::optional<Match> &current)
{
    if (matches.isEmpty())
        return std::nullopt;
    if (!current)
        return matches.last();
    for (auto it = matches.crbegin(); it != matches.crend(); ++it) {
        if (before(*it, *current))
            return *it;
    }
    return matches.last();
}

} // namespace portside
