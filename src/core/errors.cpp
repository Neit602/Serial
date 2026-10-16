#include "core/errors.h"

namespace portside {

PortsideError::PortsideError(ErrorKind kind, QString message, QString port, QString remedy)
    : m_kind(kind)
    , m_message(std::move(message))
    , m_port(std::move(port))
    , m_remedy(std::move(remedy))
    , m_what(text().toUtf8())
{
}

bool PortsideError::isTransportError() const
{
    switch (m_kind) {
    case ErrorKind::Transport:
    case ErrorKind::PortNotFound:
    case ErrorKind::PortBusy:
    case ErrorKind::PermissionDenied:
    case ErrorKind::DeviceVanished:
    case ErrorKind::TransportClosed:
        return true;
    default:
        return false;
    }
}

QString PortsideError::text() const
{
    if (!m_port.isEmpty())
        return m_port + QStringLiteral(": ") + m_message;
    return m_message;
}

QString PortsideError::display() const
{
    if (!m_remedy.isEmpty())
        return text() + QStringLiteral("\n\n") + m_remedy;
    return text();
}

} // namespace portside
