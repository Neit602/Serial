#include "core/settings.h"

#include "core/errors.h"

#include <algorithm>
#include <cmath>

namespace portside {

double stopBitsValue(StopBits bits)
{
    switch (bits) {
    case StopBits::One:
        return 1.0;
    case StopBits::OnePointFive:
        return 1.5;
    case StopBits::Two:
        return 2.0;
    }
    return 1.0;
}

std::optional<StopBits> stopBitsFromValue(double value)
{
    if (value == 1.0)
        return StopBits::One;
    if (value == 1.5)
        return StopBits::OnePointFive;
    if (value == 2.0)
        return StopBits::Two;
    return std::nullopt;
}

void SerialSettings::validate() const
{
    if (baudrate < kBaudMin || baudrate > kBaudMax) {
        throw invalidSettings(QStringLiteral("baud rate %1 is outside the supported range %2-%3")
                                  .arg(baudrate)
                                  .arg(kBaudMin)
                                  .arg(kBaudMax));
    }
    if (std::find(kDataBitsChoices.begin(), kDataBitsChoices.end(), dataBits)
        == kDataBitsChoices.end()) {
        throw invalidSettings(
            QStringLiteral("data bits must be one of 5, 6, 7, 8, got %1").arg(dataBits));
    }
    // The enum fields are always valid when they come from typed code; text
    // input is checked where it is parsed (fromMap, the CLI).
}

QString SerialSettings::describe() const
{
    return QStringLiteral("%1 %2%3%4 %5")
        .arg(baudrate)
        .arg(dataBits)
        .arg(enumName(parity), enumName(stopBits), enumName(flowControl));
}

QVariantMap SerialSettings::toMap() const
{
    return {
        {QStringLiteral("baudrate"), baudrate},
        {QStringLiteral("data_bits"), dataBits},
        {QStringLiteral("parity"), enumName(parity)},
        {QStringLiteral("stop_bits"), stopBitsValue(stopBits)},
        {QStringLiteral("flow_control"), enumName(flowControl)},
    };
}

namespace {

int intField(const QVariantMap &data, const QString &key, int fallback)
{
    const auto it = data.constFind(key);
    if (it == data.constEnd())
        return fallback;
    const QVariant &value = *it;
    if (value.typeId() == QMetaType::Bool || value.typeId() == QMetaType::QString)
        throw invalidSettings(QStringLiteral("%1 must be an integer").arg(key));
    bool ok = false;
    const double number = value.toDouble(&ok);
    if (!ok || number != std::floor(number))
        throw invalidSettings(QStringLiteral("%1 must be an integer").arg(key));
    return int(number);
}

} // namespace

SerialSettings SerialSettings::fromMap(const QVariantMap &data)
{
    SerialSettings settings;
    settings.baudrate = intField(data, QStringLiteral("baudrate"), 115200);
    settings.dataBits = intField(data, QStringLiteral("data_bits"), 8);

    const QString parity = data.value(QStringLiteral("parity"), QStringLiteral("N")).toString();
    const auto parsedParity = enumFromName<Parity>(parity);
    if (!parsedParity)
        throw invalidSettings(QStringLiteral("unknown parity '%1'").arg(parity));
    settings.parity = *parsedParity;

    const QVariant stop = data.value(QStringLiteral("stop_bits"), 1.0);
    bool ok = false;
    const double stopValue = stop.toDouble(&ok);
    const auto parsedStop = ok ? stopBitsFromValue(stopValue) : std::nullopt;
    if (!parsedStop)
        throw invalidSettings(QStringLiteral("unknown stop bits '%1'").arg(stop.toString()));
    settings.stopBits = *parsedStop;

    const QString flow =
        data.value(QStringLiteral("flow_control"), QStringLiteral("none")).toString();
    const auto parsedFlow = enumFromName<FlowControl>(flow);
    if (!parsedFlow)
        throw invalidSettings(QStringLiteral("unknown flow control '%1'").arg(flow));
    settings.flowControl = *parsedFlow;

    settings.validate();
    return settings;
}

} // namespace portside
