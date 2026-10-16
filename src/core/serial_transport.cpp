#include "core/serial_transport.h"

#include "core/platform.h"
#include "core/serial_errors.h"

#include <QDeadlineTimer>
#include <QFile>
#include <QSerialPort>
#include <QThread>

#include <cmath>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <unistd.h>
#endif

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace portside {

namespace {

QSerialPort::Parity toQt(Parity parity)
{
    switch (parity) {
    case Parity::None:
        return QSerialPort::NoParity;
    case Parity::Even:
        return QSerialPort::EvenParity;
    case Parity::Odd:
        return QSerialPort::OddParity;
    case Parity::Mark:
        return QSerialPort::MarkParity;
    case Parity::Space:
        return QSerialPort::SpaceParity;
    }
    return QSerialPort::NoParity;
}

QSerialPort::StopBits toQt(StopBits bits)
{
    switch (bits) {
    case StopBits::One:
        return QSerialPort::OneStop;
    case StopBits::OnePointFive:
        return QSerialPort::OneAndHalfStop;
    case StopBits::Two:
        return QSerialPort::TwoStop;
    }
    return QSerialPort::OneStop;
}

QSerialPort::FlowControl toQt(FlowControl flow)
{
    switch (flow) {
    case FlowControl::RtsCts:
        return QSerialPort::HardwareControl;
    case FlowControl::XonXoff:
        return QSerialPort::SoftwareControl;
    case FlowControl::None:
    case FlowControl::DsrDtr: // Qt has no DSR/DTR mode; see applyNativeFlow()
        return QSerialPort::NoFlowControl;
    }
    return QSerialPort::NoFlowControl;
}

// DSR/DTR handshaking exists only in the Windows DCB; termios has no such
// mode, so on POSIX it behaves as "none", exactly as pySerial did.
void applyNativeFlow(QSerialPort &port, const SerialSettings &settings)
{
#if defined(Q_OS_WIN)
    if (!port.isOpen())
        return;
    const HANDLE handle = static_cast<HANDLE>(port.handle());
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!::GetCommState(handle, &dcb))
        return;
    const bool dsrdtr = settings.flowControl == FlowControl::DsrDtr;
    dcb.fOutxDsrFlow = dsrdtr ? TRUE : FALSE;
    dcb.fDtrControl = dsrdtr ? DTR_CONTROL_HANDSHAKE : DTR_CONTROL_ENABLE;
    ::SetCommState(handle, &dcb);
#else
    Q_UNUSED(port);
    Q_UNUSED(settings);
#endif
}

} // namespace

SerialTransport::SerialTransport(QString port, SerialSettings settings, const Platform *platform)
    : m_portName(std::move(port))
    , m_settings(settings)
    , m_platform(platform ? platform : &currentPlatform())
{
}

SerialTransport::~SerialTransport()
{
    close();
}

bool SerialTransport::isOpen() const
{
    return m_port && m_port->isOpen();
}

void SerialTransport::configure(QSerialPort &port, const SerialSettings &settings)
{
    const auto unsupported = [&](const QString &what) {
        const QString reason = port.errorString();
        port.clearError();
        return PortsideError(ErrorKind::InvalidSettings,
                             QStringLiteral("%1 is not supported by this port (%2)").arg(what, reason),
                             m_portName);
    };
    const auto check = [&](bool ok, const QString &what) {
        if (ok)
            return;
        if (port.error() == QSerialPort::UnsupportedOperationError || !port.isOpen())
            throw unsupported(what);
        throw mapCurrentError(true);
    };

    check(port.setBaudRate(settings.baudrate), QStringLiteral("baud rate %1").arg(settings.baudrate));
    check(port.setDataBits(QSerialPort::DataBits(settings.dataBits)),
          QStringLiteral("%1 data bits").arg(settings.dataBits));
    check(port.setParity(toQt(settings.parity)),
          QStringLiteral("parity %1").arg(enumName(settings.parity)));
    if (!port.setStopBits(toQt(settings.stopBits))) {
        // termios has no 1.5 stop bits; like pySerial, fall back to two.
        if (settings.stopBits == StopBits::OnePointFive) {
            port.clearError();
            check(port.setStopBits(QSerialPort::TwoStop), QStringLiteral("1.5 stop bits"));
        } else {
            check(false, QStringLiteral("%1 stop bits").arg(enumName(settings.stopBits)));
        }
    }
    check(port.setFlowControl(toQt(settings.flowControl)),
          QStringLiteral("flow control %1").arg(enumName(settings.flowControl)));
    applyNativeFlow(port, settings);
}

void SerialTransport::open()
{
    if (isOpen())
        return;
    auto *port = new QSerialPort(this);
    port->setPortName(m_portName);
    try {
        configure(*port, m_settings);
    } catch (...) {
        delete port;
        throw;
    }
    if (!port->open(QIODevice::ReadWrite)) {
        PortsideError error = mapSerialError(port->error(), port->errorString(), m_portName,
                                             *m_platform, false);
#if defined(Q_OS_UNIX)
        // Qt reports EBUSY and EACCES with the same code.  Asking the OS
        // whether we may open the node at all tells the two apart.
        if (error.kind() == ErrorKind::PermissionDenied || error.kind() == ErrorKind::PortBusy) {
            QString path = m_portName;
            if (!path.startsWith(QLatin1Char('/')))
                path.prepend(QStringLiteral("/dev/"));
            if (::access(QFile::encodeName(path).constData(), R_OK | W_OK) == 0) {
                error = PortsideError(ErrorKind::PortBusy,
                                      QStringLiteral("%1 is busy").arg(m_portName), m_portName,
                                      m_platform->busyRemedy(m_portName));
            } else if (errno == EACCES) {
                error = PortsideError(ErrorKind::PermissionDenied,
                                      QStringLiteral("permission denied opening %1").arg(m_portName),
                                      m_portName, m_platform->permissionRemedy(m_portName));
            }
        }
#endif
        delete port;
        throw error;
    }
    applyNativeFlow(*port, m_settings);
    // Assert DTR and RTS on open, as pySerial and most terminals do; drivers
    // without the lines (or RTS under hardware flow control) just say no.
    port->setDataTerminalReady(true);
    if (m_settings.flowControl != FlowControl::RtsCts)
        port->setRequestToSend(true);
    port->clearError();

    connect(port, &QSerialPort::readyRead, this, &Transport::readyRead);
    connect(port, &QSerialPort::errorOccurred, this,
            [this](QSerialPort::SerialPortError code) { onPortError(int(code)); });
    m_port = port;
    m_failed = false;
}

void SerialTransport::close() noexcept
{
    QSerialPort *port = m_port.data();
    m_port = nullptr;
    if (port == nullptr)
        return;
    port->disconnect(this);
    port->close();
    // close() can be reached from inside the port's own error signal, so the
    // object must outlive this call stack.
    port->deleteLater();
}

void SerialTransport::onPortError(int code)
{
    const auto error = QSerialPort::SerialPortError(code);
    const bool fatal = error == QSerialPort::ResourceError
                       || error == QSerialPort::DeviceNotFoundError
                       || error == QSerialPort::ReadError || error == QSerialPort::WriteError;
    if (!fatal || m_failed || !m_port)
        return;
    m_failed = true;
    const PortsideError mapped = mapSerialError(error, m_port->errorString(), m_portName,
                                                *m_platform, true);
    Q_EMIT failed(mapped.isTransportError()
                      ? mapped
                      : PortsideError(ErrorKind::DeviceVanished,
                                      QStringLiteral("%1 disappeared").arg(m_portName),
                                      m_portName, m_platform->vanishedRemedy(m_portName)));
}

QSerialPort &SerialTransport::requireOpen()
{
    if (!isOpen()) {
        throw PortsideError(ErrorKind::TransportClosed,
                            QStringLiteral("%1 is not open").arg(m_portName), m_portName);
    }
    return *m_port;
}

PortsideError SerialTransport::mapCurrentError(bool whileOpen) const
{
    if (!m_port)
        return PortsideError(ErrorKind::TransportClosed,
                             QStringLiteral("%1 is not open").arg(m_portName), m_portName);
    const PortsideError error = mapSerialError(m_port->error(), m_port->errorString(), m_portName,
                                               *m_platform, whileOpen);
    m_port->clearError();
    return error;
}

QByteArray SerialTransport::readAll()
{
    QSerialPort &port = requireOpen();
    QByteArray data = port.readAll();
    if (port.error() == QSerialPort::ResourceError)
        throw mapCurrentError(true);
    return data;
}

qint64 SerialTransport::write(const QByteArray &data)
{
    QSerialPort &port = requireOpen();
    const qint64 written = port.write(data);
    if (written < 0)
        throw mapCurrentError(true);
    return written;
}

void SerialTransport::flushOutput()
{
    QSerialPort &port = requireOpen();
    // Generous: ten bit times per byte at the current rate, plus slack.
    const qint64 budgetMs =
        2000 + qint64(std::ceil(port.bytesToWrite() * 10.0 * 1000.0 / m_settings.baudrate));
    const QDeadlineTimer deadline(budgetMs);
    while (port.bytesToWrite() > 0 && !deadline.hasExpired()) {
        if (!port.waitForBytesWritten(int(qMax<qint64>(1, deadline.remainingTime())))) {
            if (port.error() == QSerialPort::TimeoutError || port.error() == QSerialPort::NoError) {
                port.clearError();
                break;
            }
            throw mapCurrentError(true);
        }
    }
}

void SerialTransport::applySettings(const SerialSettings &settings)
{
    settings.validate();
    if (m_port)
        configure(*m_port, settings);
    m_settings = settings;
}

void SerialTransport::sendBreak(double seconds)
{
    QSerialPort &port = requireOpen();
    if (!port.setBreakEnabled(true)) {
        if (port.error() == QSerialPort::ResourceError)
            throw mapCurrentError(true);
        port.clearError();
        // The emulated break (F12) catches this and takes over.
        throw PortsideError(ErrorKind::NotSupported,
                            QStringLiteral("this driver cannot send a native break"), m_portName);
    }
    QThread::msleep(static_cast<unsigned long>(std::lround(seconds * 1000.0)));
    if (!port.setBreakEnabled(false))
        port.clearError();
}

bool SerialTransport::setDtr(bool value)
{
    QSerialPort &port = requireOpen();
    if (port.setDataTerminalReady(value))
        return true;
    if (port.error() == QSerialPort::ResourceError)
        throw mapCurrentError(true);
    port.clearError();
    return false;
}

bool SerialTransport::setRts(bool value)
{
    QSerialPort &port = requireOpen();
    if (port.setRequestToSend(value))
        return true;
    if (port.error() == QSerialPort::ResourceError)
        throw mapCurrentError(true);
    port.clearError();
    return false;
}

ModemLines SerialTransport::modemLines()
{
    QSerialPort &port = requireOpen();
    port.clearError();
    const QSerialPort::PinoutSignals pins = port.pinoutSignals();
    if (port.error() != QSerialPort::NoError) {
        // Virtual ports and some drivers cannot report the lines at all;
        // "unknown" is reported as such rather than guessed as low.
        if (port.error() == QSerialPort::ResourceError)
            throw mapCurrentError(true);
        port.clearError();
        return {};
    }
    return {
        pins.testFlag(QSerialPort::ClearToSendSignal),
        pins.testFlag(QSerialPort::DataSetReadySignal),
        pins.testFlag(QSerialPort::DataCarrierDetectSignal),
        pins.testFlag(QSerialPort::RingIndicatorSignal),
    };
}

} // namespace portside
