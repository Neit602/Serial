#pragma once

// Typed error hierarchy (PRD F6).
//
// Every error a front end may have to display carries a short human readable
// message and an optional remedy: a platform-specific, copy-pasteable fix
// produced by the platform layer.  UIs render the remedy verbatim; they never
// have to pattern-match on driver error strings.

#include <QByteArray>
#include <QMetaType>
#include <QString>

#include <exception>

namespace portside {

enum class ErrorKind {
    Generic,
    InvalidSettings,  // a setting is outside the range the PRD allows (F3)
    Transport,        // base of the transport family
    PortNotFound,     // the named port does not exist
    PortBusy,         // held open by another process
    PermissionDenied, // the current user may not open it
    DeviceVanished,   // disappeared while open (unplugged, driver reset)
    TransportClosed,  // I/O on a transport that is not open
    NotSupported,     // the driver lacks the feature (native break, ...)
    Session,          // the session's state does not allow the request
    Profile,          // a profile could not be read, written or understood
    Transfer,         // a file transfer failed: protocol error, timeout, peer gave up
    Cancelled,        // the user cancelled a file transfer
};

class PortsideError : public std::exception
{
public:
    PortsideError() = default;
    PortsideError(ErrorKind kind, QString message, QString port = {}, QString remedy = {});

    ErrorKind kind() const { return m_kind; }
    const QString &message() const { return m_message; }
    const QString &port() const { return m_port; }
    const QString &remedy() const { return m_remedy; }

    // True for every member of the transport family (TransportError in the
    // Python original): the kinds a failing port can produce.
    bool isTransportError() const;

    // "port: message", or just the message without a port.
    QString text() const;
    // Message plus remedy, formatted for a terminal or a dialog box.
    QString display() const;

    const char *what() const noexcept override { return m_what.constData(); }

private:
    ErrorKind m_kind = ErrorKind::Generic;
    QString m_message;
    QString m_port;
    QString m_remedy;
    QByteArray m_what;
};

inline PortsideError invalidSettings(const QString &message)
{
    return PortsideError(ErrorKind::InvalidSettings, message);
}

inline PortsideError sessionError(const QString &message, const QString &port = {})
{
    return PortsideError(ErrorKind::Session, message, port);
}

inline PortsideError transferError(const QString &message, const QString &remedy = {})
{
    return PortsideError(ErrorKind::Transfer, message, {}, remedy);
}

inline PortsideError profileError(const QString &message, const QString &remedy = {})
{
    return PortsideError(ErrorKind::Profile, message, {}, remedy);
}

} // namespace portside

Q_DECLARE_METATYPE(portside::PortsideError)
