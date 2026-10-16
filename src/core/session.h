#pragma once

// The session core: one device, one Qt-facing API (PRD architecture).
//
// Threading contract, which the whole design rests on:
//
// * A Session lives on the thread that created it (the GUI thread, or the
//   CLI's main thread).  Its state machine, signals and counters belong there.
// * The transport lives on a dedicated I/O thread the session owns, inside a
//   SessionWorker.  Every transport call is queued to that thread, so writes
//   and control calls are strictly ordered and never block the caller.
// * Received data crosses back through a bounded buffer and a queued signal.
//
// Front ends only ever call this class and listen to its signals.  No front
// end ever touches the transport.

#include "core/breaks.h"
#include "core/errors.h"
#include "core/factory.h"
#include "core/identity.h"
#include "core/settings.h"
#include "core/transport.h"

#include <QFuture>
#include <QMetaType>
#include <QObject>

#include <memory>
#include <optional>

class QThread;
class QTimer;

namespace portside {

class SessionWorker;

// Lifecycle of a session; UIs render this directly.
enum class SessionState { Disconnected, Connecting, Connected, Reconnecting, Closed };

template <>
struct EnumNames<SessionState>
{
    static constexpr std::array<std::pair<SessionState, std::string_view>, 5> table{{
        {SessionState::Disconnected, "disconnected"},
        {SessionState::Connecting, "connecting"},
        {SessionState::Connected, "connected"},
        {SessionState::Reconnecting, "reconnecting"},
        {SessionState::Closed, "closed"},
    }};
};

// Auto-reconnect behaviour (PRD F5).
struct ReconnectPolicy
{
    bool enabled = false;
    double initialDelay = 0.5;
    double maxDelay = 5.0;
    double factor = 2.0;
    // Empty means keep trying until the session is closed.
    std::optional<int> maxAttempts;

    // Capped exponential backoff for attempt number `attempt` (1-based).
    double delayFor(int attempt) const;
};

struct SessionOptions
{
    // Line settings to apply on open; empty means the transport's own.
    std::optional<SerialSettings> settings;
    ReconnectPolicy reconnect;
    // Factory returning a fresh transport for reconnects — normally built
    // from a DeviceIdentity so a renamed port still matches.
    Reopener reopen;
    // The device this session belongs to, for status and profiles.
    std::optional<DeviceIdentity> identity;
    // Bounded receive buffer: a consumer that falls behind loses the *oldest*
    // data rather than growing memory forever (DECISIONS D3).
    qsizetype rxLimit = 4 * 1024 * 1024;
    BreakMode breakMode = BreakMode::Auto;
};

class Session : public QObject
{
    Q_OBJECT

public:
    explicit Session(std::unique_ptr<Transport> transport, SessionOptions options = {},
                     QObject *parent = nullptr);
    ~Session() override;

    // -- introspection -----------------------------------------------------

    SessionState state() const { return m_state; }
    bool isConnected() const { return m_state == SessionState::Connected; }
    // Name of the port currently in use.
    QString port() const { return m_portName; }
    SerialSettings settings() const { return m_settings; }
    std::optional<DeviceIdentity> identity() const { return m_options.identity; }
    BreakMode breakMode() const { return m_options.breakMode; }
    void setBreakMode(BreakMode mode) { m_options.breakMode = mode; }

    qint64 bytesIn() const { return m_bytesIn; }
    qint64 bytesOut() const { return m_bytesOut; }
    // Bytes dropped because the consumer could not keep up (see rxLimit).
    qint64 rxDropped() const { return m_rxDropped; }

    // "/dev/ttyUSB0 115200 8N1 none · in 12 B · out 3 B · connected" (F22).
    QString statusLine() const;

    // For tests and diagnostics, not for front ends.
    Transport *transport() const { return m_transport; }
    QThread *ioThread() const { return m_thread; }

    // -- lifecycle ---------------------------------------------------------

    // Open the port.  Blocks until the driver answers (milliseconds); throws
    // the typed PortsideError on failure and stays Disconnected.
    void open();
    // Close the port and stop the I/O thread.  Idempotent; never throws.
    void close() noexcept;

    // -- actions (non-blocking; results and errors arrive in the future) ----

    // Writes are serialised on the I/O thread, so two callers can never
    // interleave their bytes.
    QFuture<qint64> send(const QByteArray &data);
    // Send a break, emulating it when the driver cannot (F12).  Runs on the
    // I/O thread because the emulated form changes the baud rate and sleeps.
    QFuture<BreakResult> sendBreak(double duration = kDefaultBreakDuration,
                                   std::optional<BreakMode> mode = std::nullopt);
    // Drive DTR/RTS; false means the driver has no such line (F4).
    QFuture<bool> setDtr(bool value);
    QFuture<bool> setRts(bool value);
    // Read the input modem lines (F4).
    QFuture<ModemLines> modemLines();
    // Change line settings on the fly (F3).  Throws InvalidSettings at once
    // for a bad value; the future reports driver failures.
    QFuture<void> applySettings(const SerialSettings &settings);

Q_SIGNALS:
    void stateChanged(portside::SessionState oldState, portside::SessionState newState,
                      const QString &detail);
    void dataReceived(const QByteArray &data);
    void bytesSent(qint64 count);
    // `fatal` marks the end of the session (no reconnect will follow).
    void errorRaised(const portside::PortsideError &error, bool fatal);
    // An auto-reconnect attempt is about to happen (F5).
    void reconnectAttempt(int attempt, double delay, const QString &reason);

private:
    template <typename R, typename F>
    QFuture<R> runIo(F &&work);
    template <typename R>
    QFuture<R> notConnected() const;

    void setState(SessionState state, const QString &detail = {});
    void drainReceived();
    void handleFailure(const PortsideError &error, quint64 generation);
    void scheduleReconnect();
    void attemptReconnect();
    void onReconnected(Transport *transport, quint64 generation);
    void onReconnectFailed(const PortsideError &error);

    SessionOptions m_options;
    SerialSettings m_settings;
    QString m_portName;
    SessionState m_state = SessionState::Disconnected;
    QThread *m_thread = nullptr;
    SessionWorker *m_worker = nullptr;
    Transport *m_transport = nullptr;
    QTimer *m_reconnectTimer = nullptr;
    quint64 m_generation = 0;
    int m_attempt = 0;
    QString m_reconnectReason;
    bool m_closing = false;
    qint64 m_bytesIn = 0;
    qint64 m_bytesOut = 0;
    qint64 m_rxDropped = 0;
};

} // namespace portside

Q_DECLARE_METATYPE(portside::SessionState)
