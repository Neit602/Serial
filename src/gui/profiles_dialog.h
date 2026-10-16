#pragma once

// Managing saved profiles (PRD F20, F2): the list, bind-to-device, edit,
// duplicate, delete, import and export.

#include "core/profiles.h"

#include <QDialog>

#include <optional>

class QHBoxLayout;
class QLabel;
class QListWidget;
class QPushButton;

namespace portside::gui {

class ProfilesDialog : public QDialog
{
    Q_OBJECT

public:
    ProfilesDialog(ProfileStore store, std::optional<DeviceIdentity> identity = std::nullopt,
                   QWidget *parent = nullptr);

    // Reload the list from disk, keeping the selection.
    void refresh();
    std::optional<Profile> selectedProfile() const;
    bool selectProfile(const QString &name);

    void editSelected();
    // Bind the highlighted profile to the connected device (F2 + F20).
    void bindSelected();
    // Copy a profile under `name` (asks when empty).
    void duplicateSelected(const QString &name = {});
    // Delete the highlighted profile; `confirm` false skips the question.
    void deleteSelected(bool confirm = true);
    // Write the highlighted profile to `path` (asks when empty).
    QString exportSelected(const QString &path = {});
    // Read a profile from `path` (asks when empty) and save it.
    std::optional<Profile> importProfile(const QString &path = {});

    QListWidget *list() const { return m_list; }
    QLabel *detail() const { return m_detail; }
    QPushButton *bindButton() const { return m_bind; }
    // The last problem reported, for tests to assert on.
    QString lastError() const { return m_lastError; }
    // Show problems in a message box (off in tests).
    void setInteractive(bool interactive) { m_interactive = interactive; }

private:
    QPushButton *addButton(QHBoxLayout *layout, const QString &text, void (ProfilesDialog::*slot)());
    void updateButtons();
    void save(const Profile &profile);
    void report(const PortsideError &error);

    ProfileStore m_store;
    std::optional<DeviceIdentity> m_identity;
    QList<Profile> m_profiles;
    QListWidget *m_list = nullptr;
    QLabel *m_detail = nullptr;
    QPushButton *m_edit = nullptr;
    QPushButton *m_bind = nullptr;
    QPushButton *m_duplicate = nullptr;
    QPushButton *m_delete = nullptr;
    QPushButton *m_export = nullptr;
    QPushButton *m_import = nullptr;
    QString m_lastError;
    bool m_interactive = true;
};

} // namespace portside::gui
