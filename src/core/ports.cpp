#include "core/ports.h"

#include <QJsonValue>
#include <QStringList>

namespace portside {

QString PortInfo::vidPid() const
{
    if (!vid || !pid)
        return {};
    return QStringLiteral("%1:%2")
        .arg(*vid, 4, 16, QLatin1Char('0'))
        .arg(*pid, 4, 16, QLatin1Char('0'));
}

QString PortInfo::label() const
{
    QStringList bits;
    if (!chipFamily.isEmpty())
        bits << chipFamily;
    else if (!description.isEmpty())
        bits << description;
    else
        bits << QStringLiteral("serial port");
    if (!vidPid().isEmpty())
        bits << QStringLiteral("(%1)").arg(vidPid());
    if (!serialNumber.isEmpty())
        bits << QStringLiteral("SN %1").arg(serialNumber);
    return device + QStringLiteral(" — ") + bits.join(QLatin1Char(' '));
}

namespace {

QJsonValue orNull(const QString &value)
{
    return value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value);
}

QJsonValue orNull(const std::optional<quint16> &value)
{
    return value ? QJsonValue(int(*value)) : QJsonValue(QJsonValue::Null);
}

} // namespace

QJsonObject PortInfo::toJson() const
{
    QJsonObject extraObject;
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
        extraObject.insert(it.key(), it.value());
    return {
        {QStringLiteral("device"), device},
        {QStringLiteral("description"), orNull(description)},
        {QStringLiteral("vid"), orNull(vid)},
        {QStringLiteral("pid"), orNull(pid)},
        {QStringLiteral("vid_pid"), orNull(vidPid())},
        {QStringLiteral("serial_number"), orNull(serialNumber)},
        {QStringLiteral("manufacturer"), orNull(manufacturer)},
        {QStringLiteral("product"), orNull(product)},
        {QStringLiteral("chip_family"), orNull(chipFamily)},
        {QStringLiteral("driver"), orNull(driver)},
        {QStringLiteral("stable_path"), orNull(stablePath)},
        {QStringLiteral("extra"), extraObject},
    };
}

} // namespace portside
