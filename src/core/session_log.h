#pragma once

// Session logging (PRD F15, F16).
//
// The logger listens to a session's signals, so it captures every byte that
// arrives regardless of what any UI is doing with the data — a slow or
// scrolled terminal never costs the log a line (DECISIONS D22).
//
// Two formats:
//
// * text — decoded, optionally timestamped, UTF-8 with LF endings on every
//   platform (no CRLF logs on Windows) — DECISIONS D21.
// * raw  — the exact bytes from the wire, for protocol debugging.
//
// Writes go through a buffer flushed on an interval, on rotation and on stop,
// so logging cannot stall the session.

#include "core/decoder.h"
#include "core/timestamps.h"

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QList>
#include <QMetaObject>
#include <QString>

#include <functional>
#include <memory>

namespace portside {

class Session;

enum class LogFormat { Text, Raw };

template <>
struct EnumNames<LogFormat>
{
    static constexpr std::array<std::pair<LogFormat, std::string_view>, 2> table{{
        {LogFormat::Text, "text"},
        {LogFormat::Raw, "raw"},
    }};
};

inline const QString kDefaultLogTemplate = QStringLiteral("portside-{port}-{date}-{time}.log");

// Make one value safe as a filename component on every OS: the characters
// Windows forbids become '_', so one template works everywhere.
QString sanitiseFileComponent(const QString &value);

// Expand a filename template (F15).  Placeholders: {port}, {device} (an alias
// for port), {date}, {time}, {profile}, {n}; {{ and }} are literal braces.
// Every substitution is sanitised, so a port named /dev/ttyUSB0 or COM3:
// cannot produce an illegal path.  An unknown placeholder falls back to the
// default template rather than failing a connect.
QString resolveTemplate(const QString &pattern, const QString &port = {},
                        const QString &profile = {}, const QDateTime &when = {}, int index = 0);

// How a session should be logged (F15, F16).
struct LogSettings
{
    // Empty means the platform's log directory.
    QString directory;
    QString filenameTemplate = kDefaultLogTemplate;
    LogFormat format = LogFormat::Text;
    TimestampMode timestamps = TimestampMode::Off;
    CharacterSet charset = CharacterSet::Utf8;
    bool autoStart = false;
    // Bytes per file before rotation; 0 disables rotation.
    qint64 maxBytes = 0;
    // How many rotated files to keep, oldest deleted first.
    int keep = 5;
    double flushInterval = 1.0;

    bool operator==(const LogSettings &) const = default;
};

using Clock = std::function<double()>;

class SessionLogger
{
public:
    explicit SessionLogger(LogSettings settings = {}, QString port = {}, QString profile = {},
                           Clock now = wallClock);
    ~SessionLogger();
    SessionLogger(const SessionLogger &) = delete;
    SessionLogger &operator=(const SessionLogger &) = delete;

    const LogSettings &settings() const { return m_settings; }
    void setSettings(const LogSettings &settings) { m_settings = settings; }
    QString port() const { return m_port; }
    void setPort(const QString &port) { m_port = port; }
    QString profile() const { return m_profile; }

    // True while a file is open.
    bool isLogging() const { return m_file != nullptr; }
    // The file currently being written, if any.
    QString path() const { return m_path; }
    // Bytes in the current file, buffer included.
    qint64 bytesWritten() const { return m_written + m_buffer.size(); }
    int rotations() const { return m_rotations; }

    // Open a log file (appending: a reconnect continues the same log) and
    // start recording.  Returns the path used.  Throws PortsideError if the
    // file cannot be opened.
    QString start(const QString &path = {});
    // Flush and close.  Idempotent.
    void stop();

    // Follow a session: received data, errors as notes, and — with autoStart
    // — open when it connects.  The file closes when the session ends.
    void attach(Session *session);
    void detach();

    // Record bytes received from the device.
    void write(const QByteArray &data);
    // Record a Portside-generated line (errors, break sent, paste finished).
    // Notes are for humans reading a text log; a raw log stays byte-exact.
    void writeNote(const QString &note);
    // Force pending data to disk.
    void flush();

    // Restart the elapsed-time clock (F16).
    void resetElapsed();
    // Change the timestamp mode of a log that is already open (F16).
    void restamp(TimestampMode mode);

private:
    QByteArray asText(const QByteArray &data);
    void maybeFlush();
    bool rotationDue() const;
    void flushBuffer();
    void rotateIfNeeded();
    void prune();
    QString defaultPath() const;

    LogSettings m_settings;
    QString m_port;
    QString m_profile;
    Clock m_now;

    std::unique_ptr<QFile> m_file;
    QString m_path;
    QByteArray m_buffer;
    qint64 m_written = 0;
    int m_rotations = 0;
    double m_lastFlush = 0.0;
    bool m_lineOpen = false;
    bool m_pendingCr = false;
    Timestamper m_timestamper;
    IncrementalTextDecoder m_decoder;
    QList<QMetaObject::Connection> m_connections;
};

} // namespace portside
