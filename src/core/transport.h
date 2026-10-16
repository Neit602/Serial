#pragma once

// The transport interface (PRD: "the only place OS or hardware differences
// leak in").
//
// A transport is a QObject that lives on its session's I/O thread and is only
// ever called there.  It is event driven: `readyRead` says there is something
// for readAll(), `failed` reports an asynchronous loss of the device.  Calls
// that can fail throw PortsideError of the transport family, already mapped to
// a typed error carrying the platform's remedy.  Keeping one contract is what
// lets a QSerialPort, an in-memory loopback and (later) a direct-USB backend
// share the session code.

#include "core/errors.h"
#include "core/settings.h"

#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QString>

#include <optional>

namespace portside {

// PRD F12 default break length, in seconds.
inline constexpr double kDefaultBreakDuration = 0.25;

// Input modem lines (PRD F4).  A field is empty when the driver does not
// report that line: empty means "unknown", never "low", so a UI can grey the
// indicator out instead of lying about it.
struct ModemLines
{
    std::optional<bool> cts;
    std::optional<bool> dsr;
    std::optional<bool> dcd;
    std::optional<bool> ri;

    // "CTS DSR dcd --" style summary: upper = asserted, "--" = unknown.
    QString describe() const;

    bool operator==(const ModemLines &) const = default;
};

class Transport : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    ~Transport() override = default;

    // Port name or URL this transport talks to (for messages and logs).
    virtual QString name() const = 0;
    virtual bool isOpen() const = 0;
    virtual SerialSettings settings() const = 0;

    // Throws PermissionDenied, PortBusy, PortNotFound or DeviceVanished (F6).
    virtual void open() = 0;
    // Idempotent and never throws.
    virtual void close() noexcept = 0;

    // Everything received so far; empty when nothing is waiting.  Throws
    // DeviceVanished if the device went away.
    virtual QByteArray readAll() = 0;
    // Queue `data` for the wire, returning the number of bytes accepted.
    virtual qint64 write(const QByteArray &data) = 0;
    // Block until queued output has been handed to the driver.
    virtual void flushOutput() {}

    // Apply new line settings, on an open or a closed port (F3).
    virtual void applySettings(const SerialSettings &settings) = 0;

    // Assert a break for `seconds` (F12).  Throws PortsideError(NotSupported)
    // when the driver cannot, which is the hook the emulated break hangs off.
    virtual void sendBreak(double seconds) = 0;

    // Drive DTR/RTS (F4).  Return false when the driver does not implement the
    // line: a limitation of the adapter, not a session-ending error.
    virtual bool setDtr(bool value) = 0;
    virtual bool setRts(bool value) = 0;

    // Read CTS/DSR/DCD/RI; unsupported lines come back empty (F4).
    virtual ModemLines modemLines() = 0;

Q_SIGNALS:
    void readyRead();
    void failed(const portside::PortsideError &error);
};

} // namespace portside

Q_DECLARE_METATYPE(portside::ModemLines)
