#pragma once

// In-memory transport for tests, demos and the `loop://` URL.
//
// Two things real hardware cannot do on demand: behave deterministically, and
// fail exactly when you want it to.  LoopbackTransport does both — it echoes
// what is written to it (or plays what a test feeds it) and can be told to
// refuse to open, or to make the device vanish after N bytes, so the session's
// error and reconnect paths are exercised without hardware.
//
// The test helpers (feed, written, breaks, ...) are thread-safe: a test calls
// them from its own thread while the transport lives on the session's thread.

#include "core/transport.h"

#include <QList>
#include <QMutex>

#include <atomic>

namespace portside {

struct LoopbackOptions
{
    QString name = QStringLiteral("loopback://");
    // Written bytes become readable bytes.
    bool echo = true;
    // Raised by open() — for permission/busy tests.
    std::optional<PortsideError> failOpenWith;
    // Raise DeviceVanished once this many bytes have been read or written.
    std::optional<qint64> vanishAfter;
    // What modemLines() reports.
    ModemLines modemLines{true, true, false, false};
    // When false, DTR/RTS report "not supported" the way a PTY does.
    bool supportsControlLines = true;
    // When false, sendBreak() throws NotSupported (exercises the emulated break).
    bool supportsBreak = true;
};

class LoopbackTransport : public Transport
{
    Q_OBJECT

public:
    explicit LoopbackTransport(LoopbackOptions options = {}, SerialSettings settings = {});

    // -- test helpers ------------------------------------------------------

    // Make `data` readable, as if the device had sent it.  Any thread.
    void feed(const QByteArray &data);
    // Arm (or disarm) the vanish fault at runtime.
    void setVanishAfter(std::optional<qint64> bytes);
    // Everything the "device" was sent.
    QByteArray written() const;
    // Breaks sent, as durations in seconds.
    QList<double> breaks() const;
    // Settings applied over the transport's life, in order.
    QList<SerialSettings> appliedSettings() const;
    std::optional<bool> dtr() const;
    std::optional<bool> rts() const;
    int openCount() const;

    // -- Transport ---------------------------------------------------------

    QString name() const override { return m_options.name; }
    bool isOpen() const override { return m_open.load(); }
    SerialSettings settings() const override;
    void open() override;
    void close() noexcept override;
    QByteArray readAll() override;
    qint64 write(const QByteArray &data) override;
    void applySettings(const SerialSettings &settings) override;
    void sendBreak(double seconds) override;
    bool setDtr(bool value) override;
    bool setRts(bool value) override;
    ModemLines modemLines() override;

private:
    void requireOpen() const;
    void account(qint64 count);
    void notifyReadable();

    LoopbackOptions m_options;
    mutable QMutex m_mutex;
    SerialSettings m_settings;
    QByteArray m_buffer;
    QByteArray m_written;
    QList<double> m_breaks;
    QList<SerialSettings> m_applied;
    std::optional<bool> m_dtr;
    std::optional<bool> m_rts;
    int m_openCount = 0;
    qint64 m_transferred = 0;
    std::atomic<bool> m_open{false};
    std::atomic<bool> m_notifyPending{false};
};

} // namespace portside
