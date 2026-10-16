#include "gui/profiles_dialog.h"

#include "gui/settings_dialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace portside::gui {

ProfilesDialog::ProfilesDialog(ProfileStore store, std::optional<DeviceIdentity> identity,
                               QWidget *parent)
    : QDialog(parent)
    , m_store(std::move(store))
    , m_identity(std::move(identity))
{
    setWindowTitle(tr("Profiles"));
    auto *layout = new QVBoxLayout(this);
    m_list = new QListWidget(this);
    m_list->setAccessibleName(tr("Saved profiles"));
    connect(m_list, &QListWidget::currentRowChanged, this, [this] { updateButtons(); });
    layout->addWidget(m_list);

    m_detail = new QLabel(this);
    m_detail->setWordWrap(true);
    layout->addWidget(m_detail);

    auto *buttons = new QHBoxLayout;
    m_edit = addButton(buttons, tr("&Edit…"), &ProfilesDialog::editSelected);
    m_bind = addButton(buttons, tr("&Bind to device"), &ProfilesDialog::bindSelected);
    m_duplicate = addButton(buttons, tr("D&uplicate"), nullptr);
    connect(m_duplicate, &QPushButton::clicked, this, [this] { duplicateSelected(); });
    m_delete = addButton(buttons, tr("&Delete"), nullptr);
    connect(m_delete, &QPushButton::clicked, this, [this] { deleteSelected(); });
    m_export = addButton(buttons, tr("E&xport…"), nullptr);
    connect(m_export, &QPushButton::clicked, this, [this] { exportSelected(); });
    m_import = addButton(buttons, tr("&Import…"), nullptr);
    connect(m_import, &QPushButton::clicked, this, [this] { importProfile(); });
    layout->addLayout(buttons);

    auto *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(close);
    refresh();
}

QPushButton *ProfilesDialog::addButton(QHBoxLayout *layout, const QString &text,
                                       void (ProfilesDialog::*slot)())
{
    auto *button = new QPushButton(text, this);
    button->setAccessibleName(QString(text).remove(QLatin1Char('&')));
    if (slot != nullptr)
        connect(button, &QPushButton::clicked, this, slot);
    layout->addWidget(button);
    return button;
}

void ProfilesDialog::refresh()
{
    const auto selected = selectedProfile();
    const QSignalBlocker blocker(m_list);
    m_list->clear();
    m_profiles = m_store.all();
    for (int i = 0; i < m_profiles.size(); ++i) {
        const Profile &profile = m_profiles.at(i);
        auto *item = new QListWidgetItem(profile.name, m_list);
        item->setData(Qt::UserRole, i);
        if (profile.matches(m_identity))
            item->setText(tr("%1  (this device)").arg(profile.name));
        if (selected && profile.name == selected->name)
            m_list->setCurrentItem(item);
    }
    updateButtons();
}

std::optional<Profile> ProfilesDialog::selectedProfile() const
{
    const QListWidgetItem *item = m_list->currentItem();
    if (item == nullptr)
        return std::nullopt;
    const int index = item->data(Qt::UserRole).toInt();
    if (index < 0 || index >= m_profiles.size())
        return std::nullopt;
    return m_profiles.at(index);
}

bool ProfilesDialog::selectProfile(const QString &name)
{
    for (int row = 0; row < m_list->count(); ++row) {
        const int index = m_list->item(row)->data(Qt::UserRole).toInt();
        if (m_profiles.at(index).name == name) {
            m_list->setCurrentRow(row);
            return true;
        }
    }
    return false;
}

void ProfilesDialog::updateButtons()
{
    const auto profile = selectedProfile();
    for (QPushButton *button : {m_edit, m_duplicate, m_delete, m_export})
        button->setEnabled(profile.has_value());
    m_bind->setEnabled(profile.has_value() && m_identity.has_value());
    if (!profile) {
        m_detail->setText(tr("Profiles are stored in %1").arg(QDir::toNativeSeparators(m_store.directory())));
        return;
    }
    const QString devices = profile->deviceKeys.isEmpty() ? tr("not bound to a device")
                                                          : profile->deviceKeys.join(QStringLiteral(", "));
    QString text = QStringLiteral("%1 · %2").arg(profile->serial.describe(), devices);
    if (!profile->description.isEmpty())
        text += QLatin1Char('\n') + profile->description;
    m_detail->setText(text);
}

void ProfilesDialog::editSelected()
{
    const auto profile = selectedProfile();
    if (!profile)
        return;
    SettingsDialog dialog(*profile, this);
    if (dialog.exec() == QDialog::Accepted)
        save(dialog.toProfile());
}

void ProfilesDialog::bindSelected()
{
    const auto profile = selectedProfile();
    if (!profile || !m_identity)
        return;
    save(profile->withDevice(*m_identity));
}

void ProfilesDialog::duplicateSelected(const QString &name)
{
    const auto profile = selectedProfile();
    if (!profile)
        return;
    QString chosen = name;
    if (chosen.isEmpty()) {
        bool ok = false;
        chosen = QInputDialog::getText(this, tr("Duplicate profile"), tr("New name:"), QLineEdit::Normal,
                                       tr("%1 copy").arg(profile->name), &ok);
        if (!ok)
            return;
    }
    chosen = chosen.trimmed();
    if (chosen.isEmpty())
        return;
    Profile copy = *profile;
    copy.name = chosen;
    copy.deviceKeys.clear();
    save(copy);
    selectProfile(chosen);
}

void ProfilesDialog::deleteSelected(bool confirm)
{
    const auto profile = selectedProfile();
    if (!profile)
        return;
    if (confirm
        && QMessageBox::question(this, tr("Delete profile"), tr("Delete the profile %1?").arg(profile->name))
               != QMessageBox::Yes) {
        return;
    }
    m_store.remove(profile->name);
    refresh();
}

QString ProfilesDialog::exportSelected(const QString &path)
{
    const auto profile = selectedProfile();
    if (!profile)
        return {};
    QString target = path;
    if (target.isEmpty()) {
        target = QFileDialog::getSaveFileName(this, tr("Export profile"), profile->name + QStringLiteral(".toml"),
                                              tr("TOML (*.toml)"));
        if (target.isEmpty())
            return {};
    }
    try {
        return m_store.exportTo(*profile, target);
    } catch (const PortsideError &error) {
        report(error);
        return {};
    }
}

std::optional<Profile> ProfilesDialog::importProfile(const QString &path)
{
    QString source = path;
    if (source.isEmpty()) {
        source = QFileDialog::getOpenFileName(this, tr("Import profile"), QString(), tr("TOML (*.toml)"));
        if (source.isEmpty())
            return std::nullopt;
    }
    try {
        const Profile imported = m_store.importFile(source);
        save(imported);
        selectProfile(imported.name);
        return imported;
    } catch (const PortsideError &error) {
        report(error);
        return std::nullopt;
    }
}

void ProfilesDialog::save(const Profile &profile)
{
    try {
        m_store.save(profile);
    } catch (const PortsideError &error) {
        report(error);
        return;
    }
    refresh();
}

void ProfilesDialog::report(const PortsideError &error)
{
    m_lastError = error.display();
    if (m_interactive)
        QMessageBox::warning(this, tr("Profile problem"), m_lastError);
}

} // namespace portside::gui
