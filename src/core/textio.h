#pragma once

// Line discipline: endings, local echo, backspace, XON/XOFF (PRD F9, F14).
//
// Per the PRD these are per-profile settings, never OS defaults: a session
// opened with the same profile must behave identically on Linux, macOS and
// Windows, whatever the host's own line-ending convention is.

#include "core/enums.h"

#include <QByteArray>
#include <QString>

#include <optional>

namespace portside {

// DC3 / DC1, the software flow-control pair (F14).
inline constexpr char kXoff = 0x13;
inline constexpr char kXon = 0x11;

// What a line break becomes on the wire, and what to do with one received.
enum class LineEnding { Cr, Lf, CrLf, None };

template <>
struct EnumNames<LineEnding>
{
    static constexpr std::array<std::pair<LineEnding, std::string_view>, 4> table{{
        {LineEnding::Cr, "cr"},
        {LineEnding::Lf, "lf"},
        {LineEnding::CrLf, "crlf"},
        {LineEnding::None, "none"},
    }};
};

QByteArray endingBytes(LineEnding ending);

// What the backspace key sends (F9).  Cisco and most Unix hosts expect DEL;
// some embedded monitors expect BS.  Getting this wrong makes editing a
// command line impossible, so it is explicit rather than guessed.
enum class Backspace { Bs, Del };

template <>
struct EnumNames<Backspace>
{
    static constexpr std::array<std::pair<Backspace, std::string_view>, 2> table{{
        {Backspace::Bs, "bs"},
        {Backspace::Del, "del"},
    }};
};

char backspaceByte(Backspace backspace);

// Line-discipline settings for one session (F9).
struct TextPolicy
{
    // What Enter sends.  CR is the serial-console norm.
    LineEnding sendEnding = LineEnding::Cr;
    // How received line breaks are normalised for the screen and the text
    // log; None passes them through untouched.
    LineEnding receiveEnding = LineEnding::None;
    // Echo typed characters locally, for devices that do not.
    bool localEcho = false;
    Backspace backspace = Backspace::Del;

    // One line plus the configured ending, ready for the wire.
    QByteArray encodeLine(const QString &line) const;
    // Rewrite line breaks in outgoing data to the configured ending.  Any of
    // CRLF, CR or LF counts as one break, so a file pasted from any platform
    // reaches the device the same way.
    QByteArray translateOutput(const QByteArray &data) const;
    // Normalise received line breaks; None leaves the stream untouched.
    QByteArray translateInput(const QByteArray &data) const;
    // What local echo should display for `data` (empty when echo is off).
    QByteArray echoFor(const QByteArray &data) const;

    bool operator==(const TextPolicy &) const = default;
};

// Software flow control for drivers that do not implement it (F14).
//
// A received XOFF pauses sending; XON resumes it.  A lost XON would otherwise
// wedge the session forever, so the pause expires after `maxPause` seconds and
// the expiry is counted.
class XonXoffGate
{
public:
    explicit XonXoffGate(bool enabled = false, double maxPause = 30.0);

    bool enabled() const { return m_enabled; }
    int timeouts() const { return m_timeouts; }

    // Update the gate from data received from the device at time `now`.
    void observe(const QByteArray &data, double now);
    // Whether sending should be held right now.
    bool isPaused(double now);
    // Force a resume (the user pressing Ctrl-Q, or a reconnect).
    void resume();

private:
    bool m_enabled;
    double m_maxPause;
    std::optional<double> m_pausedAt;
    int m_timeouts = 0;
};

} // namespace portside
