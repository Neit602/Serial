#include "core/profiles.h"

#include "core/errors.h"
#include "core/platform.h"
#include "core/toml.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <cmath>

namespace portside {

namespace {

const QString kSuffix = QStringLiteral(".toml");

// Typed access to one table of a parsed profile.  Every failure names the
// field, because "malformed profile" alone helps nobody fix the file.
class Fields
{
public:
    Fields(QVariantMap map, QString table)
        : m_map(std::move(map))
        , m_table(std::move(table))
    {
    }

    QString string(const QString &key, const QString &fallback) const
    {
        const QVariant value = m_map.value(key);
        if (!value.isValid())
            return fallback;
        if (value.typeId() != QMetaType::QString)
            throw malformed(key, QStringLiteral("a string"));
        return value.toString();
    }

    bool boolean(const QString &key, bool fallback) const
    {
        const QVariant value = m_map.value(key);
        if (!value.isValid())
            return fallback;
        if (value.typeId() != QMetaType::Bool)
            throw malformed(key, QStringLiteral("true or false"));
        return value.toBool();
    }

    qint64 integer(const QString &key, qint64 fallback) const
    {
        const QVariant value = m_map.value(key);
        if (!value.isValid())
            return fallback;
        if (value.typeId() == QMetaType::Double) {
            const double number = value.toDouble();
            if (number == std::floor(number) && std::isfinite(number))
                return qint64(number);
        }
        if (value.typeId() != QMetaType::LongLong)
            throw malformed(key, QStringLiteral("an integer"));
        return value.toLongLong();
    }

    double real(const QString &key, double fallback) const
    {
        const QVariant value = m_map.value(key);
        if (!value.isValid())
            return fallback;
        if (value.typeId() != QMetaType::Double && value.typeId() != QMetaType::LongLong)
            throw malformed(key, QStringLiteral("a number"));
        return value.toDouble();
    }

    template <typename E>
    E choice(const QString &key, E fallback) const
    {
        const QVariant value = m_map.value(key);
        if (!value.isValid())
            return fallback;
        const auto parsed = enumFromName<E>(value.toString());
        if (value.typeId() != QMetaType::QString || !parsed) {
            throw profileError(QStringLiteral("malformed profile: %1 '%2' is not one of %3")
                                   .arg(where(key), value.toString(),
                                        enumNameList<E>().join(QStringLiteral(", "))));
        }
        return *parsed;
    }

    QStringList strings(const QString &key) const
    {
        const QVariant value = m_map.value(key);
        if (!value.isValid())
            return {};
        if (value.typeId() != QMetaType::QVariantList)
            throw malformed(key, QStringLiteral("a list of strings"));
        QStringList out;
        for (const QVariant &item : value.toList()) {
            if (item.typeId() != QMetaType::QString)
                throw malformed(key, QStringLiteral("a list of strings"));
            out << item.toString();
        }
        return out;
    }

private:
    QString where(const QString &key) const
    {
        return m_table.isEmpty() ? key : m_table + QLatin1Char('.') + key;
    }

    PortsideError malformed(const QString &key, const QString &expected) const
    {
        return profileError(
            QStringLiteral("malformed profile: %1 must be %2").arg(where(key), expected));
    }

    QVariantMap m_map;
    QString m_table;
};

Fields table(const QVariantMap &data, const QString &name)
{
    const QVariant value = data.value(name);
    if (value.isValid() && value.typeId() != QMetaType::QVariantMap)
        throw profileError(QStringLiteral("malformed profile: [%1] must be a table").arg(name));
    return Fields(value.toMap(), name);
}

} // namespace

Profile Profile::withDevice(const DeviceIdentity &identity) const
{
    const QString key = identity.key();
    if (deviceKeys.contains(key))
        return *this;
    Profile bound = *this;
    bound.deviceKeys << key;
    return bound;
}

bool Profile::matches(const std::optional<DeviceIdentity> &identity) const
{
    return identity && deviceKeys.contains(identity->key());
}

QString Profile::toToml() const
{
    // No empty values are skipped: TOML has no null, and a hand-editor
    // should see every setting that exists.
    TomlWriter w;
    w.value(QStringLiteral("schema_version"), kSchemaVersion);
    w.value(QStringLiteral("name"), name);
    w.value(QStringLiteral("description"), description);
    w.value(QStringLiteral("device_keys"), QVariant(deviceKeys));

    w.table(QStringLiteral("serial"));
    w.value(QStringLiteral("baudrate"), serial.baudrate);
    w.value(QStringLiteral("data_bits"), serial.dataBits);
    w.value(QStringLiteral("parity"), enumName(serial.parity));
    w.value(QStringLiteral("stop_bits"), stopBitsValue(serial.stopBits));
    w.value(QStringLiteral("flow_control"), enumName(serial.flowControl));

    w.table(QStringLiteral("text"));
    w.value(QStringLiteral("send_ending"), enumName(text.sendEnding));
    w.value(QStringLiteral("receive_ending"), enumName(text.receiveEnding));
    w.value(QStringLiteral("local_echo"), text.localEcho);
    w.value(QStringLiteral("backspace"), enumName(text.backspace));

    w.table(QStringLiteral("terminal"));
    w.value(QStringLiteral("scrollback"), terminal.scrollback);
    w.value(QStringLiteral("charset"), enumName(terminal.charset));
    w.value(QStringLiteral("strip_high_bit"), terminal.stripHighBit);
    w.value(QStringLiteral("columns"), terminal.columns);
    w.value(QStringLiteral("rows"), terminal.rows);
    w.value(QStringLiteral("font_family"), terminal.fontFamily);
    w.value(QStringLiteral("font_size"), terminal.fontSize);
    w.value(QStringLiteral("theme"), terminal.theme);
    w.value(QStringLiteral("select_to_copy"), terminal.selectToCopy);

    w.table(QStringLiteral("pacing"));
    w.value(QStringLiteral("char_delay"), pacing.charDelay);
    w.value(QStringLiteral("line_delay"), pacing.lineDelay);
    w.value(QStringLiteral("wait_for_prompt"), pacing.waitForPrompt);
    w.value(QStringLiteral("prompt_timeout"), pacing.promptTimeout);
    w.value(QStringLiteral("abort_on_timeout"), pacing.abortOnTimeout);

    w.table(QStringLiteral("logging"));
    w.value(QStringLiteral("directory"), logging.directory);
    w.value(QStringLiteral("template"), logging.filenameTemplate);
    w.value(QStringLiteral("format"), enumName(logging.format));
    w.value(QStringLiteral("timestamps"), enumName(logging.timestamps));
    w.value(QStringLiteral("charset"), enumName(logging.charset));
    w.value(QStringLiteral("auto_start"), logging.autoStart);
    w.value(QStringLiteral("max_bytes"), logging.maxBytes);
    w.value(QStringLiteral("keep"), logging.keep);

    w.table(QStringLiteral("break"));
    w.value(QStringLiteral("mode"), enumName(breakMode));
    w.value(QStringLiteral("duration"), breakDuration);
    return w.text();
}

Profile Profile::fromMap(const QVariantMap &data)
{
    const Fields top(data, {});
    const qint64 version = top.integer(QStringLiteral("schema_version"), kSchemaVersion);
    if (version > kSchemaVersion) {
        throw profileError(
            QStringLiteral("profile uses schema version %1, but this Portside understands up to %2")
                .arg(version)
                .arg(kSchemaVersion),
            QStringLiteral("Update Portside, or export the profile again from the older version."));
    }

    Profile profile;
    if (!data.contains(QStringLiteral("name")))
        throw profileError(QStringLiteral("malformed profile: it has no name"));
    profile.name = top.string(QStringLiteral("name"), {});
    profile.description = top.string(QStringLiteral("description"), {});
    profile.deviceKeys = top.strings(QStringLiteral("device_keys"));

    try {
        const QVariant serial = data.value(QStringLiteral("serial"));
        if (serial.isValid() && serial.typeId() != QMetaType::QVariantMap)
            throw profileError(QStringLiteral("malformed profile: [serial] must be a table"));
        profile.serial = SerialSettings::fromMap(serial.toMap());
    } catch (const PortsideError &error) {
        if (error.kind() == ErrorKind::Profile)
            throw;
        // A validation failure deeper down (a bad baud rate, say) is still a
        // bad profile from the user's point of view; the message says why.
        throw profileError(QStringLiteral("invalid profile: %1").arg(error.message()));
    }

    const Fields text = table(data, QStringLiteral("text"));
    profile.text.sendEnding = text.choice(QStringLiteral("send_ending"), LineEnding::Cr);
    profile.text.receiveEnding = text.choice(QStringLiteral("receive_ending"), LineEnding::None);
    profile.text.localEcho = text.boolean(QStringLiteral("local_echo"), false);
    profile.text.backspace = text.choice(QStringLiteral("backspace"), Backspace::Del);

    const Fields terminal = table(data, QStringLiteral("terminal"));
    TerminalOptions &options = profile.terminal;
    options.scrollback = int(terminal.integer(QStringLiteral("scrollback"), kDefaultScrollback));
    options.charset = terminal.choice(QStringLiteral("charset"), CharacterSet::Utf8);
    options.stripHighBit = terminal.boolean(QStringLiteral("strip_high_bit"), false);
    options.columns = int(terminal.integer(QStringLiteral("columns"), kDefaultColumns));
    options.rows = int(terminal.integer(QStringLiteral("rows"), kDefaultRows));
    options.fontFamily = terminal.string(QStringLiteral("font_family"), {});
    options.fontSize = int(terminal.integer(QStringLiteral("font_size"), 11));
    options.theme = terminal.string(QStringLiteral("theme"), QStringLiteral("dark"));
    options.selectToCopy = terminal.boolean(QStringLiteral("select_to_copy"), false);
    if (options.scrollback < 0 || options.columns < 1 || options.rows < 1 || options.fontSize < 1)
        throw profileError(QStringLiteral("invalid profile: terminal sizes must be positive"));

    const Fields pacing = table(data, QStringLiteral("pacing"));
    profile.pacing.charDelay = pacing.real(QStringLiteral("char_delay"), 0.0);
    profile.pacing.lineDelay = pacing.real(QStringLiteral("line_delay"), 0.05);
    profile.pacing.waitForPrompt = pacing.string(QStringLiteral("wait_for_prompt"), {});
    profile.pacing.promptTimeout = pacing.real(QStringLiteral("prompt_timeout"), 5.0);
    profile.pacing.abortOnTimeout = pacing.boolean(QStringLiteral("abort_on_timeout"), true);
    try {
        profile.pacing.validate();
    } catch (const PortsideError &error) {
        throw profileError(QStringLiteral("invalid profile: %1").arg(error.message()));
    }

    const Fields logs = table(data, QStringLiteral("logging"));
    LogSettings &logging = profile.logging;
    logging.directory = logs.string(QStringLiteral("directory"), {});
    logging.filenameTemplate = logs.string(QStringLiteral("template"), kDefaultLogTemplate);
    logging.format = logs.choice(QStringLiteral("format"), LogFormat::Text);
    logging.timestamps = logs.choice(QStringLiteral("timestamps"), TimestampMode::Off);
    logging.charset = logs.choice(QStringLiteral("charset"), CharacterSet::Utf8);
    logging.autoStart = logs.boolean(QStringLiteral("auto_start"), false);
    logging.maxBytes = logs.integer(QStringLiteral("max_bytes"), 0);
    logging.keep = int(logs.integer(QStringLiteral("keep"), 5));

    const Fields breaks = table(data, QStringLiteral("break"));
    profile.breakMode = breaks.choice(QStringLiteral("mode"), BreakMode::Auto);
    profile.breakDuration = breaks.real(QStringLiteral("duration"), 0.25);
    if (profile.breakDuration <= 0)
        throw profileError(QStringLiteral("invalid profile: break duration must be positive"));
    return profile;
}

ProfileStore::ProfileStore(QString directory)
    : m_directory(directory.isEmpty() ? QDir(Platform::configDir()).filePath(QStringLiteral("profiles"))
                                      : std::move(directory))
{
}

QString ProfileStore::pathFor(const QString &name) const
{
    return QDir(m_directory).filePath(sanitiseFileComponent(name) + kSuffix);
}

QStringList ProfileStore::names() const
{
    const QDir dir(m_directory);
    if (!dir.exists())
        return {};
    QStringList names;
    for (const QString &file : dir.entryList({QStringLiteral("*") + kSuffix}, QDir::Files, QDir::Name))
        names << file.chopped(kSuffix.size());
    return names;
}

QString ProfileStore::save(const Profile &profile) const
{
    QDir().mkpath(m_directory);
    return exportTo(profile, pathFor(profile.name));
}

Profile ProfileStore::load(const QString &name) const
{
    const QString path = pathFor(name);
    if (!QFileInfo(path).isFile()) {
        throw profileError(QStringLiteral("no profile named '%1'").arg(name),
                           QStringLiteral("Profiles live in %1. Use 'portside profiles' to list them.")
                               .arg(QDir::toNativeSeparators(m_directory)));
    }
    return importFile(path);
}

void ProfileStore::remove(const QString &name) const
{
    QFile::remove(pathFor(name));
}

QList<Profile> ProfileStore::all() const
{
    QList<Profile> profiles;
    for (const QString &name : names()) {
        try {
            profiles << load(name);
        } catch (const PortsideError &) {
            continue;
        }
    }
    return profiles;
}

std::optional<Profile> ProfileStore::forDevice(const std::optional<DeviceIdentity> &identity) const
{
    if (!identity)
        return std::nullopt;
    for (const Profile &profile : all()) {
        if (profile.matches(identity))
            return profile;
    }
    return std::nullopt;
}

QString ProfileStore::exportTo(const Profile &profile, const QString &path) const
{
    // QSaveFile: a crash mid-write never leaves half a profile behind.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        throw profileError(QStringLiteral("could not write %1: %2")
                               .arg(QDir::toNativeSeparators(path), file.errorString()));
    }
    file.write(profile.toToml().toUtf8());
    if (!file.commit()) {
        throw profileError(QStringLiteral("could not write %1: %2")
                               .arg(QDir::toNativeSeparators(path), file.errorString()));
    }
    return path;
}

Profile ProfileStore::importFile(const QString &path) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw profileError(QStringLiteral("could not read %1: %2")
                               .arg(QDir::toNativeSeparators(path), file.errorString()));
    }
    QVariantMap data;
    try {
        data = parseToml(QString::fromUtf8(file.readAll()));
    } catch (const PortsideError &error) {
        throw profileError(QStringLiteral("%1 is not valid TOML: %2")
                               .arg(QDir::toNativeSeparators(path), error.message()));
    }
    if (!data.contains(QStringLiteral("name")))
        data.insert(QStringLiteral("name"), QFileInfo(path).completeBaseName());
    return Profile::fromMap(data);
}

} // namespace portside
