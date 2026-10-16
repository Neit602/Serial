#pragma once

// QSerialPort-backed transport — the primary path on all three platforms.
//
// Every call that can fail goes through mapSerialError(), so callers above
// this module never see a QSerialPort error code.

#include "core/transport.h"

#include <QPointer>

class QSerialPort;

namespace portside {

class Platform;

class SerialTransport : public Transport
{
    Q_OBJECT

public:
    explicit SerialTransport(QString port, SerialSettings settings = {},
                             const Platform *platform = nullptr);
    ~SerialTransport() override;

    QString name() const override { return m_portName; }
    bool isOpen() const override;
    SerialSettings settings() const override { return m_settings; }
    void open() override;
    void close() noexcept override;
    QByteArray readAll() override;
    qint64 write(const QByteArray &data) override;
    void flushOutput() override;
    void applySettings(const SerialSettings &settings) override;
    void sendBreak(double seconds) override;
    bool setDtr(bool value) override;
    bool setRts(bool value) override;
    ModemLines modemLines() override;

private:
    QSerialPort &requireOpen();
    void configure(QSerialPort &port, const SerialSettings &settings);
    PortsideError mapCurrentError(bool whileOpen) const;
    void onPortError(int code);

    QString m_portName;
    SerialSettings m_settings;
    const Platform *m_platform;
    QPointer<QSerialPort> m_port;
    bool m_failed = false;
};

} // namespace portside
