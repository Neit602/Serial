// Named TOML profiles keyed by device identity (F20, F2).

#include "core/discovery.h"
#include "core/profiles.h"
#include "core/toml.h"
#include "support/support.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>

using namespace portside;
using namespace portside::test;

namespace {

Profile sample()
{
    Profile profile;
    profile.name = QStringLiteral("cisco-lab");
    profile.description = QStringLiteral("Core switches, rack 4");
    profile.serial.baudrate = 9600;
    profile.serial.parity = Parity::Even;
    profile.serial.stopBits = StopBits::OnePointFive;
    profile.text.sendEnding = LineEnding::CrLf;
    profile.text.localEcho = true;
    profile.text.backspace = Backspace::Bs;
    profile.terminal.scrollback = 2000;
    profile.terminal.charset = CharacterSet::Cp437;
    profile.terminal.fontFamily = QStringLiteral("DejaVu Sans Mono");
    profile.terminal.theme = QStringLiteral("light");
    profile.pacing.charDelay = 0.002;
    profile.pacing.lineDelay = 0.05;
    profile.pacing.waitForPrompt = QStringLiteral("[>#]\\s*$");
    profile.logging.directory = QStringLiteral("/var/log/portside");
    profile.logging.format = LogFormat::Raw;
    profile.logging.timestamps = TimestampMode::Delta;
    profile.logging.autoStart = true;
    profile.logging.maxBytes = 1'048'576;
    profile.breakMode = BreakMode::Emulated;
    profile.breakDuration = 0.5;
    profile.deviceKeys = {QStringLiteral("usb_0403_6001_A50285BI")};
    return profile;
}

QString messageFor(const QString &toml)
{
    try {
        Profile::fromMap(parseToml(toml));
    } catch (const PortsideError &error) {
        return error.message();
    }
    return {};
}

} // namespace

class TestProfiles : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void everyFieldRoundTripsThroughToml()
    {
        const Profile profile = sample();
        QCOMPARE(Profile::fromMap(parseToml(profile.toToml())), profile);
    }

    void theFileStartsWithItsSchemaAndName()
    {
        const QString text = sample().toToml();
        QVERIFY(text.startsWith(QStringLiteral("schema_version = 1\nname = \"cisco-lab\"\n")));
        QVERIFY(text.contains(QStringLiteral("[break]\nmode = \"emulated\"\nduration = 0.5\n")));
    }

    void theReadmeExampleLoads()
    {
        const Profile profile = Profile::fromMap(parseToml(QStringLiteral(
            "name = \"cisco-lab\"\n"
            "device_keys = [\"usb_0403_6001_A50285BI\"]\n\n"
            "[serial]\nbaudrate = 9600\n\n"
            "[text]\nsend_ending = \"cr\"\nbackspace = \"del\"\n\n"
            "[pacing]\nline_delay = 0.05\nwait_for_prompt = \"[>#]\\\\s*$\"\n\n"
            "[break]\nmode = \"auto\"\nduration = 0.25\n")));
        QCOMPARE(profile.serial.baudrate, 9600);
        QCOMPARE(profile.pacing.waitForPrompt, QStringLiteral("[>#]\\s*$"));
        QCOMPARE(profile.terminal.scrollback, 10'000); // defaults fill the gaps
    }

    void aProfileFromANewerPortsideIsRefused()
    {
        try {
            Profile::fromMap(parseToml(QStringLiteral("schema_version = 99\nname = \"x\"\n")));
            QFAIL("loaded");
        } catch (const PortsideError &error) {
            QCOMPARE(error.kind(), ErrorKind::Profile);
            QVERIFY(error.message().contains(QStringLiteral("schema version 99")));
            QVERIFY(!error.remedy().isEmpty());
        }
    }

    void badFieldsSayWhatIsWrong()
    {
        QCOMPARE(messageFor(QStringLiteral("name = \"x\"\n[text]\nsend_ending = \"cr-lf\"\n")),
                 QStringLiteral("malformed profile: text.send_ending 'cr-lf' is not one of cr, lf, crlf, none"));
        QCOMPARE(messageFor(QStringLiteral("name = \"x\"\n[text]\nlocal_echo = \"yes\"\n")),
                 QStringLiteral("malformed profile: text.local_echo must be true or false"));
        QCOMPARE(messageFor(QStringLiteral("name = \"x\"\n[serial]\nbaudrate = 12\n")),
                 QStringLiteral("invalid profile: baud rate 12 is outside the supported range 50-4000000"));
        QCOMPARE(messageFor(QStringLiteral("name = \"x\"\n[pacing]\nwait_for_prompt = \"([\"\n")).left(39),
                 QStringLiteral("invalid profile: invalid prompt pattern"));
        QCOMPARE(messageFor(QStringLiteral("name = \"x\"\ntext = 3\n")),
                 QStringLiteral("malformed profile: [text] must be a table"));
        QCOMPARE(messageFor(QStringLiteral("description = \"no name\"\n")),
                 QStringLiteral("malformed profile: it has no name"));
    }

    void bindingToADeviceIsIdempotent()
    {
        RawPort raw;
        raw.device = QStringLiteral("COM7");
        raw.vid = 0x10C4;
        raw.pid = 0xEA60;
        raw.serialNumber = QStringLiteral("0001");
        const DeviceIdentity identity = DeviceIdentity::fromPort(portFromRaw(raw));
        Profile profile;
        profile.name = QStringLiteral("p");
        QVERIFY(!profile.matches(identity));
        const Profile bound = profile.withDevice(identity).withDevice(identity);
        QCOMPARE(bound.deviceKeys, QStringList{QStringLiteral("usb_10c4_ea60_0001")});
        QVERIFY(bound.matches(identity));
        QVERIFY(!bound.matches(std::nullopt));
    }

    // -- the store -----------------------------------------------------------

    void storeSavesLoadsListsAndDeletes()
    {
        QTemporaryDir dir;
        const ProfileStore store(dir.filePath(QStringLiteral("profiles")));
        QVERIFY(store.names().isEmpty());
        const QString path = store.save(sample());
        QCOMPARE(path, dir.filePath(QStringLiteral("profiles/cisco-lab.toml")));
        Profile second;
        second.name = QStringLiteral("arduino");
        store.save(second);
        QCOMPARE(store.names(), (QStringList{QStringLiteral("arduino"), QStringLiteral("cisco-lab")}));
        QCOMPARE(store.load(QStringLiteral("cisco-lab")), sample());
        store.remove(QStringLiteral("arduino"));
        store.remove(QStringLiteral("never-existed"));
        QCOMPARE(store.names(), QStringList{QStringLiteral("cisco-lab")});
    }

    void aMissingProfileExplainsWhereProfilesLive()
    {
        QTemporaryDir dir;
        const ProfileStore store(dir.path());
        try {
            store.load(QStringLiteral("nope"));
            QFAIL("loaded");
        } catch (const PortsideError &error) {
            QCOMPARE(error.message(), QStringLiteral("no profile named 'nope'"));
            QVERIFY(error.remedy().contains(QDir::toNativeSeparators(dir.path())));
        }
    }

    void unreadableProfilesAreSkippedNotFatal()
    {
        QTemporaryDir dir;
        const ProfileStore store(dir.path());
        store.save(sample());
        writeFile(dir.filePath(QStringLiteral("broken.toml")), "name = [unterminated\n");
        QCOMPARE(store.all().size(), 1);
    }

    void autoApplyFindsTheProfileForADevice()
    {
        QTemporaryDir dir;
        const ProfileStore store(dir.path());
        store.save(sample());
        DeviceIdentity identity;
        identity.vid = 0x0403;
        identity.pid = 0x6001;
        identity.serialNumber = QStringLiteral("A50285BI");
        QCOMPARE(store.forDevice(identity)->name, QStringLiteral("cisco-lab"));
        identity.serialNumber = QStringLiteral("other");
        QVERIFY(!store.forDevice(identity));
        QVERIFY(!store.forDevice(std::nullopt));
    }

    void importAndExport()
    {
        QTemporaryDir dir;
        const ProfileStore store(dir.filePath(QStringLiteral("store")));
        const QString exported = store.exportTo(sample(), dir.filePath(QStringLiteral("shared.toml")));
        QCOMPARE(store.importFile(exported), sample());
        // A file without a name takes its file name.
        writeFile(dir.filePath(QStringLiteral("from-a-colleague.toml")), "[serial]\nbaudrate = 19200\n");
        const Profile imported = store.importFile(dir.filePath(QStringLiteral("from-a-colleague.toml")));
        QCOMPARE(imported.name, QStringLiteral("from-a-colleague"));
        QCOMPARE(imported.serial.baudrate, 19200);
        // A file that is not TOML says so.
        writeFile(dir.filePath(QStringLiteral("bad.toml")), "= nope");
        try {
            store.importFile(dir.filePath(QStringLiteral("bad.toml")));
            QFAIL("imported");
        } catch (const PortsideError &error) {
            QVERIFY(error.message().contains(QStringLiteral("is not valid TOML: line 1:")));
        }
    }

    void namesAreSanitisedForTheFileSystem()
    {
        QTemporaryDir dir;
        const ProfileStore store(dir.path());
        QCOMPARE(QFileInfo(store.pathFor(QStringLiteral("lab/core:1"))).fileName(), QStringLiteral("lab_core_1.toml"));
    }
};

QTEST_GUILESS_MAIN(TestProfiles)
#include "tst_profiles.moc"
