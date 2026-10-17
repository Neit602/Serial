#pragma once

// Named TOML profiles (PRD F20).
//
// A profile is everything a session needs to behave the way a given device
// wants: line settings, line discipline, break mode, pacing, logging and
// terminal options.  Profiles are keyed by DeviceIdentity so the right one is
// applied when an adapter comes back on a different port name (F2), on any of
// the three platforms.
//
// Files are plain TOML in the platform's config directory, with a
// schema_version so a future Portside can migrate them — and so an older
// Portside refuses a file it does not understand instead of silently
// misreading it (DECISIONS D25).

#include "core/breaks.h"
#include "core/decoder.h"
#include "core/identity.h"
#include "core/pacing.h"
#include "core/session_log.h"
#include "core/settings.h"
#include "core/terminal.h"
#include "core/textio.h"
#include "core/transfer/transfer.h"

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>

namespace portside {

// Bumped only for changes that older versions cannot read.
inline constexpr int kSchemaVersion = 1;

// Terminal-side settings a profile carries (F7, F8, F10, F11).  An empty
// fontFamily means "the platform's monospace font", the only portable
// default: no font name exists on all three OSes.
struct TerminalOptions
{
    int scrollback = kDefaultScrollback;
    CharacterSet charset = CharacterSet::Utf8;
    bool stripHighBit = false;
    int columns = kDefaultColumns;
    int rows = kDefaultRows;
    QString fontFamily;
    int fontSize = 11;
    QString theme = QStringLiteral("dark");
    bool selectToCopy = false;

    bool operator==(const TerminalOptions &) const = default;
};

// File transfer defaults (F17–F20).  An empty directory means the user's
// Downloads folder.
struct TransferDefaults
{
    Protocol protocol = Protocol::Zmodem;
    QString directory;
    OverwritePolicy overwrite = OverwritePolicy::Refuse;
    // Offer to receive when the device starts a ZMODEM send (F18).
    bool zmodemAutoStart = true;

    bool operator==(const TransferDefaults &) const = default;
};

// A named bundle of settings, optionally bound to devices (F20).
struct Profile
{
    QString name;
    SerialSettings serial;
    TextPolicy text;
    TerminalOptions terminal;
    PacePolicy pacing;
    LogSettings logging;
    TransferDefaults transfer;
    BreakMode breakMode = BreakMode::Auto;
    double breakDuration = 0.25;
    // Device identity keys this profile auto-applies to (F2 + F20).
    QStringList deviceKeys;
    QString description;

    // A copy bound to `identity` as well as its current devices.
    Profile withDevice(const DeviceIdentity &identity) const;
    // Whether this profile should auto-apply to `identity`.
    bool matches(const std::optional<DeviceIdentity> &identity) const;

    QString toToml() const;
    // Rebuild a profile, rejecting files from a newer Portside.  Throws
    // PortsideError(Profile) saying what is wrong.
    static Profile fromMap(const QVariantMap &data);

    bool operator==(const Profile &) const = default;
};

// Profiles on disk, one TOML file each (F20).
class ProfileStore
{
public:
    // Empty means <config dir>/profiles for this platform.
    explicit ProfileStore(QString directory = {});

    QString directory() const { return m_directory; }
    // The file backing the profile called `name`.
    QString pathFor(const QString &name) const;
    // Every profile name, alphabetically.
    QStringList names() const;

    // Write `profile`, creating the directory if needed.  Returns the path.
    QString save(const Profile &profile) const;
    // Throws PortsideError(Profile) if missing or unreadable.
    Profile load(const QString &name) const;
    // Remove a profile; a missing profile is not an error.
    void remove(const QString &name) const;
    // Every readable profile; unreadable ones are skipped, not fatal.
    QList<Profile> all() const;
    // The profile bound to `identity`, if any (F2 + F20 auto-apply).
    std::optional<Profile> forDevice(const std::optional<DeviceIdentity> &identity) const;

    // Import/export to arbitrary paths (F20).
    QString exportTo(const Profile &profile, const QString &path) const;
    Profile importFile(const QString &path) const;

private:
    QString m_directory;
};

} // namespace portside

Q_DECLARE_METATYPE(portside::Profile)
