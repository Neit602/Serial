// The settings and profiles dialogs (F3, F9-F16, F20).

#include "gui/gui_support.h"
#include "gui/profiles_dialog.h"
#include "gui/settings_dialog.h"
#include "support/support.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QAbstractSpinBox>
#include <QSpinBox>
#include <QTest>

using namespace portside;
using namespace portside::gui;
using namespace portside::test;

namespace {

Profile handEdited()
{
    Profile profile;
    profile.name = QStringLiteral("lab");
    profile.serial.baudrate = 9600;
    profile.serial.parity = Parity::Odd;
    profile.text.sendEnding = LineEnding::CrLf;
    profile.terminal.columns = 132;
    profile.terminal.rows = 50;
    profile.terminal.theme = QStringLiteral("light");
    profile.logging.charset = CharacterSet::Cp437; // no control for this in the dialog
    profile.pacing.waitForPrompt = QStringLiteral("#\\s*$");
    profile.breakMode = BreakMode::Emulated;
    profile.deviceKeys = {QStringLiteral("usb_1a86_7523_desc_USB")};
    return profile;
}

} // namespace

class TestDialogs : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // -- settings ------------------------------------------------------------------

    void theDialogRoundTripsAProfile()
    {
        const Profile profile = handEdited();
        SettingsDialog dialog(profile);
        QCOMPARE(dialog.toProfile(), profile);
    }

    void fieldsTheDialogCannotShowArePreserved()
    {
        // DECISIONS D34: editing must not silently discard hand-edited values.
        SettingsDialog dialog(handEdited());
        dialog.fontSize()->setValue(14);
        const Profile edited = dialog.toProfile();
        QCOMPARE(edited.logging.charset, CharacterSet::Cp437);
        QCOMPARE(edited.terminal.columns, 132);
        QCOMPARE(edited.terminal.theme, QStringLiteral("light"));
        QCOMPARE(edited.deviceKeys, handEdited().deviceKeys);
        QCOMPARE(edited.terminal.fontSize, 14);
    }

    void anyBaudRateInRangeCanBeTyped()
    {
        SettingsDialog dialog(handEdited());
        dialog.baud()->setCurrentText(QStringLiteral("250000"));
        QCOMPARE(dialog.toProfile().serial.baudrate, 250000);
    }

    void invalidInputKeepsTheDialogOpenAndSaysWhy()
    {
        SettingsDialog dialog(handEdited());
        dialog.baud()->setEditText(QStringLiteral("12"));
        dialog.acceptIfValid();
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QVERIFY(dialog.errorText().contains(QStringLiteral("baud rate 12")));
    }

    void anInvalidPromptIsMarkedWhileTyping()
    {
        SettingsDialog dialog(handEdited());
        dialog.waitForPrompt()->setText(QStringLiteral("(["));
        QVERIFY(dialog.errorText().startsWith(QStringLiteral("Invalid prompt pattern")));
        QVERIFY(!dialog.waitForPrompt()->styleSheet().isEmpty());
        dialog.waitForPrompt()->setText(QStringLiteral("[>#]"));
        QVERIFY(dialog.errorText().isEmpty());
    }

    void theLogNameIsPreviewed()
    {
        SettingsDialog dialog(handEdited());
        dialog.logTemplate()->setText(QStringLiteral("{profile}-{n}.log"));
        QCOMPARE(dialog.templatePreview()->text(), QStringLiteral("lab-0.log"));
    }

    void everyControlHasAnAccessibleName()
    {
        SettingsDialog dialog(handEdited());
        for (QWidget *control : dialog.findChildren<QWidget *>()) {
            if (qobject_cast<QComboBox *>(control) || qobject_cast<QSpinBox *>(control)
                || qobject_cast<QDoubleSpinBox *>(control)
                || (qobject_cast<QLineEdit *>(control) && !qobject_cast<QComboBox *>(control->parent())
                    && !qobject_cast<QAbstractSpinBox *>(control->parent()))) {
                QVERIFY2(!control->accessibleName().isEmpty(), control->metaObject()->className());
            }
        }
    }

    // -- profiles --------------------------------------------------------------------

    void theListShowsProfilesAndMarksThisDevice()
    {
        QTemporaryDir dir;
        const ProfileStore store(dir.path());
        store.save(handEdited());
        Profile other;
        other.name = QStringLiteral("other");
        store.save(other);
        DeviceIdentity identity;
        identity.vid = 0x1a86;
        identity.pid = 0x7523;
        identity.fallback = QStringLiteral("USB");
        identity.weak = true;
        ProfilesDialog dialog(store, identity);
        QCOMPARE(dialog.list()->count(), 2);
        QCOMPARE(dialog.list()->item(0)->text(), QStringLiteral("lab  (this device)"));
        QCOMPARE(dialog.list()->item(1)->text(), QStringLiteral("other"));
    }

    void bindingDuplicatingAndDeleting()
    {
        QTemporaryDir dir;
        const ProfileStore store(dir.path());
        Profile plain;
        plain.name = QStringLiteral("plain");
        store.save(plain);
        DeviceIdentity identity;
        identity.vid = 0x0403;
        identity.pid = 0x6001;
        identity.serialNumber = QStringLiteral("X9");
        ProfilesDialog dialog(store, identity);
        dialog.setInteractive(false);
        QVERIFY(dialog.selectProfile(QStringLiteral("plain")));
        QVERIFY(dialog.bindButton()->isEnabled());
        dialog.bindSelected();
        QCOMPARE(store.load(QStringLiteral("plain")).deviceKeys, QStringList{QStringLiteral("usb_0403_6001_X9")});
        dialog.duplicateSelected(QStringLiteral("copy"));
        QVERIFY(store.load(QStringLiteral("copy")).deviceKeys.isEmpty()); // a copy is not bound
        QVERIFY(dialog.selectProfile(QStringLiteral("copy")));
        dialog.deleteSelected(false);
        QCOMPARE(store.names(), QStringList{QStringLiteral("plain")});
    }

    void withoutADeviceThereIsNothingToBind()
    {
        QTemporaryDir dir;
        const ProfileStore store(dir.path());
        store.save(handEdited());
        ProfilesDialog dialog(store);
        dialog.selectProfile(QStringLiteral("lab"));
        QVERIFY(!dialog.bindButton()->isEnabled());
        QVERIFY(dialog.detail()->text().contains(QStringLiteral("9600 8O1 none")));
    }

    void exportAndImport()
    {
        QTemporaryDir dir;
        const ProfileStore store(dir.filePath(QStringLiteral("store")));
        store.save(handEdited());
        ProfilesDialog dialog(store);
        dialog.setInteractive(false);
        dialog.selectProfile(QStringLiteral("lab"));
        const QString exported = dialog.exportSelected(dir.filePath(QStringLiteral("lab-export.toml")));
        QVERIFY(QFile::exists(exported));
        store.remove(QStringLiteral("lab"));
        dialog.refresh();
        QCOMPARE(dialog.list()->count(), 0);
        const auto imported = dialog.importProfile(exported);
        QVERIFY(imported);
        QCOMPARE(*imported, handEdited());
        QCOMPARE(dialog.list()->count(), 1);
    }

    void aBrokenImportIsReported()
    {
        QTemporaryDir dir;
        const ProfileStore store(dir.path());
        ProfilesDialog dialog(store);
        dialog.setInteractive(false);
        writeFile(dir.filePath(QStringLiteral("broken.toml")), "[serial\n");
        QVERIFY(!dialog.importProfile(dir.filePath(QStringLiteral("broken.toml"))));
        QVERIFY(dialog.lastError().contains(QStringLiteral("is not valid TOML")));
    }
};

QTEST_MAIN(TestDialogs)
#include "tst_dialogs.moc"
