#include "gui/transfer_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace portside::gui {

QString transferHint(Protocol protocol, TransferDirection direction)
{
    const bool sending = direction == TransferDirection::Send;
    switch (protocol) {
    case Protocol::Xmodem:
    case Protocol::Xmodem1k:
        return sending ? QObject::tr("Start the receiver on the device first: rx FILE, or loadx in U-Boot.")
                       : QObject::tr("Start the sender on the device after this: sx FILE.");
    case Protocol::Ymodem:
    case Protocol::YmodemG:
        return sending ? QObject::tr("Start the receiver on the device first: rb, or loady in U-Boot.")
                       : QObject::tr("Start the sender on the device after this: sb FILE.");
    case Protocol::Zmodem:
        return sending ? QObject::tr("Portside types \"rz\" for you; a device at a shell prompt starts receiving.")
                       : QObject::tr("Start the sender on the device after this: sz FILE.");
    case Protocol::Kermit:
        return sending ? QObject::tr("Start the receiver on the device first: kermit -r.")
                       : QObject::tr("Start the sender on the device after this: kermit -s FILE.");
    case Protocol::Raw:
        break;
    }
    return QObject::tr("The file's bytes go out as they are, with no protocol and no error checking.");
}

TransferDialog::TransferDialog(TransferDirection direction, const TransferOptions &defaults,
                               const QStringList &files, QWidget *parent)
    : QDialog(parent)
    , m_direction(direction)
    , m_defaults(defaults)
{
    const bool sending = direction == TransferDirection::Send;
    setWindowTitle(sending ? tr("Send files") : tr("Receive files"));
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    layout->addLayout(form);

    m_protocol = new QComboBox(this);
    for (const Protocol protocol : enumValues<Protocol>()) {
        if (!sending && !protocolCanReceive(protocol))
            continue;
        m_protocol->addItem(protocolTitle(protocol), enumName(protocol));
    }
    m_protocol->setAccessibleName(tr("Protocol"));
    const int index = m_protocol->findData(enumName(defaults.protocol));
    m_protocol->setCurrentIndex(index < 0 ? 0 : index);
    form->addRow(tr("&Protocol"), m_protocol);

    if (sending) {
        auto *box = new QWidget(this);
        auto *row = new QHBoxLayout(box);
        row->setContentsMargins(0, 0, 0, 0);
        m_files = new QListWidget(box);
        m_files->setAccessibleName(tr("Files to send"));
        m_files->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_files->setMinimumHeight(90);
        row->addWidget(m_files, 1);
        auto *buttons = new QVBoxLayout;
        auto *add = new QPushButton(tr("&Add…"), box);
        add->setAccessibleName(tr("Add files"));
        auto *remove = new QPushButton(tr("&Remove"), box);
        remove->setAccessibleName(tr("Remove files"));
        connect(add, &QPushButton::clicked, this, &TransferDialog::addFiles);
        connect(remove, &QPushButton::clicked, this, &TransferDialog::removeFiles);
        buttons->addWidget(add);
        buttons->addWidget(remove);
        buttons->addStretch(1);
        row->addLayout(buttons);
        form->addRow(tr("&Files"), box);

        m_chunkSize = new QSpinBox(this);
        m_chunkSize->setRange(1, 65536);
        m_chunkSize->setValue(defaults.rawChunkSize);
        m_chunkSize->setSuffix(tr(" bytes"));
        m_chunkSize->setAccessibleName(tr("Chunk size"));
        form->addRow(tr("Raw &chunk size"), m_chunkSize);
        m_chunkDelay = new QDoubleSpinBox(this);
        m_chunkDelay->setRange(0.0, 10.0);
        m_chunkDelay->setDecimals(3);
        m_chunkDelay->setSingleStep(0.01);
        m_chunkDelay->setSuffix(tr(" s"));
        m_chunkDelay->setValue(defaults.rawChunkDelay);
        m_chunkDelay->setAccessibleName(tr("Delay between chunks"));
        form->addRow(tr("Raw &delay"), m_chunkDelay);
    } else {
        auto *box = new QWidget(this);
        auto *row = new QHBoxLayout(box);
        row->setContentsMargins(0, 0, 0, 0);
        m_directory = new QLineEdit(QDir::toNativeSeparators(defaults.directory), box);
        m_directory->setAccessibleName(tr("Download folder"));
        auto *browse = new QPushButton(tr("&Browse…"), box);
        browse->setAccessibleName(tr("Choose download folder"));
        connect(browse, &QPushButton::clicked, this, &TransferDialog::browseDirectory);
        row->addWidget(m_directory, 1);
        row->addWidget(browse);
        form->addRow(tr("Save &into"), box);

        m_overwrite = new QComboBox(this);
        const std::pair<OverwritePolicy, QString> policies[] = {
            {OverwritePolicy::Refuse, tr("Keep the existing file (skip)")},
            {OverwritePolicy::Rename, tr("Keep both (name.1.ext)")},
            {OverwritePolicy::Overwrite, tr("Replace it")}};
        for (const auto &[policy, label] : policies)
            m_overwrite->addItem(label, enumName(policy));
        m_overwrite->setCurrentIndex(m_overwrite->findData(enumName(defaults.overwrite)));
        m_overwrite->setAccessibleName(tr("If the file exists"));
        form->addRow(tr("If the file &exists"), m_overwrite);

        m_outputName = new QLineEdit(defaults.outputName, this);
        m_outputName->setPlaceholderText(tr("xmodem-<date>-<time>.bin"));
        m_outputName->setAccessibleName(tr("File name"));
        m_outputName->setToolTip(tr("XMODEM sends no file name, so choose one."));
        form->addRow(tr("File &name"), m_outputName);

        m_textMode = new QCheckBox(tr("Text file: strip the padding at the end"), this);
        m_textMode->setChecked(defaults.textMode);
        m_textMode->setAccessibleName(tr("Text file"));
        form->addRow(QString(), m_textMode);

        m_resume = new QCheckBox(tr("Continue an interrupted download"), this);
        m_resume->setChecked(defaults.resume);
        m_resume->setAccessibleName(tr("Resume"));
        form->addRow(QString(), m_resume);
    }

    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);
    layout->addWidget(m_hint);
    m_error = new QLabel(this);
    m_error->setStyleSheet(QStringLiteral("color: #c92a2a;"));
    m_error->setWordWrap(true);
    layout->addWidget(m_error);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    okButton()->setText(sending ? tr("&Send") : tr("&Receive"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, [this] {
        if (problem().isEmpty())
            accept();
    });
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);

    connect(m_protocol, &QComboBox::currentIndexChanged, this, &TransferDialog::update);
    if (m_directory != nullptr)
        connect(m_directory, &QLineEdit::textChanged, this, &TransferDialog::update);
    setFiles(files);
    update();
}

QPushButton *TransferDialog::okButton() const
{
    return m_buttons->button(QDialogButtonBox::Ok);
}

Protocol TransferDialog::chosenProtocol() const
{
    return enumFromName<Protocol>(m_protocol->currentData().toString()).value_or(Protocol::Zmodem);
}

QStringList TransferDialog::files() const
{
    QStringList paths;
    if (m_files == nullptr)
        return paths;
    for (int i = 0; i < m_files->count(); ++i)
        paths << m_files->item(i)->data(Qt::UserRole).toString();
    return paths;
}

void TransferDialog::setFiles(const QStringList &files)
{
    if (m_files == nullptr)
        return;
    m_files->clear();
    for (const QString &path : files) {
        auto *item = new QListWidgetItem(QDir::toNativeSeparators(path), m_files);
        item->setData(Qt::UserRole, path);
    }
    update();
}

void TransferDialog::addFiles()
{
    const QStringList chosen = QFileDialog::getOpenFileNames(this, tr("Files to send"));
    if (!chosen.isEmpty())
        setFiles(files() + chosen);
}

void TransferDialog::removeFiles()
{
    qDeleteAll(m_files->selectedItems());
    update();
}

void TransferDialog::browseDirectory()
{
    const QString chosen = QFileDialog::getExistingDirectory(this, tr("Save received files into"),
                                                             m_directory->text());
    if (!chosen.isEmpty())
        m_directory->setText(QDir::toNativeSeparators(chosen));
}

QString TransferDialog::problem() const
{
    const Protocol protocol = chosenProtocol();
    if (m_direction == TransferDirection::Send) {
        const QStringList paths = files();
        if (paths.isEmpty())
            return tr("Add the files to send.");
        if (!protocolIsBatch(protocol) && paths.size() > 1)
            return tr("%1 sends one file at a time; choose YMODEM, ZMODEM or Kermit for several.")
                .arg(protocolTitle(protocol));
        for (const QString &path : paths) {
            if (!QFileInfo(path).isFile())
                return tr("%1 is not a file.").arg(QDir::toNativeSeparators(path));
        }
        return {};
    }
    if (m_directory->text().trimmed().isEmpty())
        return tr("Choose where to save the files.");
    return {};
}

void TransferDialog::update()
{
    const Protocol protocol = chosenProtocol();
    const bool xmodem = !protocolIsBatch(protocol);
    if (m_chunkSize != nullptr) {
        m_chunkSize->setEnabled(protocol == Protocol::Raw);
        m_chunkDelay->setEnabled(protocol == Protocol::Raw);
    }
    if (m_outputName != nullptr) {
        m_outputName->setEnabled(xmodem);
        m_textMode->setEnabled(xmodem);
        m_resume->setEnabled(protocol == Protocol::Zmodem);
    }
    m_hint->setText(transferHint(protocol, m_direction));
    const QString issue = problem();
    m_error->setText(issue);
    okButton()->setEnabled(issue.isEmpty());
}

TransferOptions TransferDialog::options() const
{
    TransferOptions options = m_defaults;
    options.protocol = chosenProtocol();
    if (m_direction == TransferDirection::Send) {
        options.rawChunkSize = m_chunkSize->value();
        options.rawChunkDelay = m_chunkDelay->value();
    } else {
        options.directory = QDir::fromNativeSeparators(m_directory->text().trimmed());
        options.overwrite = enumFromName<OverwritePolicy>(m_overwrite->currentData().toString())
                                .value_or(OverwritePolicy::Refuse);
        options.outputName = m_outputName->text().trimmed();
        options.textMode = m_textMode->isChecked();
        options.resume = m_resume->isChecked();
    }
    return options;
}

} // namespace portside::gui
