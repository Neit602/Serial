#pragma once

// The port description handed to UIs (PRD F1).

#include <QJsonObject>
#include <QMap>
#include <QMetaType>
#include <QString>

#include <optional>

namespace portside {

// Everything known about one serial port.
//
// Every field except `device` may be empty: plain motherboard UARTs and
// Bluetooth ports carry no USB identity, and drivers differ in what they
// report.  Nothing here is ever guessed.  An empty string means "unknown".
struct PortInfo
{
    QString device;
    QString description;
    std::optional<quint16> vid;
    std::optional<quint16> pid;
    QString serialNumber;
    QString manufacturer;
    QString product;
    QString chipFamily;
    QString driver;
    QString stablePath;
    QMap<QString, QString> extra;

    // "0403:6001" style identifier, or empty for non-USB ports.
    QString vidPid() const;
    // One-line friendly name, e.g. "/dev/ttyUSB0 — FTDI FT232R (0403:6001)".
    QString label() const;
    // JSON mapping for `portside --list --json`; unknown fields are null.
    QJsonObject toJson() const;

    bool operator==(const PortInfo &) const = default;
};

} // namespace portside

Q_DECLARE_METATYPE(portside::PortInfo)
