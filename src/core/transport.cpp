#include "core/transport.h"

#include <QStringList>

namespace portside {

QString ModemLines::describe() const
{
    QStringList parts;
    const std::pair<const char *, std::optional<bool>> lines[] = {
        {"cts", cts}, {"dsr", dsr}, {"dcd", dcd}, {"ri", ri}};
    for (const auto &[name, value] : lines) {
        if (!value)
            parts << QStringLiteral("--");
        else
            parts << (*value ? QString::fromLatin1(name).toUpper() : QString::fromLatin1(name));
    }
    return parts.join(QLatin1Char(' '));
}

} // namespace portside
