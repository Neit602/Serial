#include "gui/settings_dialog.h"

#include "gui/terminal_widget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace portside::gui {

SettingsDialog::SettingsDialog(const Profile &profile, QWidget *parent)
    : QDialog(parent)
    , m_profile(profile)
{
    setWindowTitle(tr("Session settings"));
    auto *layout = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildConnection(), tr("Connection"));
    m_tabs->addTab(buildTerminal(), tr("Terminal"));
    m_tabs->addTab(buildLogging(), tr("Logging"));
    m_tabs->addTab(buildPacing(), tr("Paste && break"));
    layout->addWidget(m_tabs);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::acceptIfValid);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);

    m_error = new QLabel(this);
    m_error->setStyleSheet(QStringLiteral("color: #c92a2a;"));
    m_error->setWordWrap(true);
    layout->addWidget(m_error);

    load(profile);
}

// -- pages --------------------------------------------------------------------

QWidget *SettingsDialog::buildConnection()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);

    m_baud = new QComboBox(page);
    m_baud->setEditable(true);
    for (const int rate : kCommonBaudRates)
        m_baud->addItem(QString::number(rate));
    m_baud->lineEdit()->setValidator(new QIntValidator(kBaudMin, kBaudMax, this));
    m_baud->setAccessibleName(tr("Baud rate"));
    form->addRow(tr("&Baud rate"), m_baud);

    m_dataBits = new QComboBox(page);
    for (const int bits : kDataBitsChoices)
        m_dataBits->addItem(QString::number(bits), bits);
    m_dataBits->setAccessibleName(tr("Data bits"));
    form->addRow(tr("&Data bits"), m_dataBits);

    m_parity = new QComboBox(page);
    const std::pair<Parity, QString> parities[] = {
        {Parity::None, tr("None")}, {Parity::Even, tr("Even")}, {Parity::Odd, tr("Odd")},
        {Parity::Mark, tr("Mark")}, {Parity::Space, tr("Space")}};
    for (const auto &[value, label] : parities)
        m_parity->addItem(label, enumName(value));
    m_parity->setAccessibleName(tr("Parity"));
    form->addRow(tr("&Parity"), m_parity);

    m_stopBits = new QComboBox(page);
    for (const StopBits bits : enumValues<StopBits>())
        m_stopBits->addItem(enumName(bits), enumName(bits));
    m_stopBits->setAccessibleName(tr("Stop bits"));
    form->addRow(tr("&Stop bits"), m_stopBits);

    m_flow = new QComboBox(page);
    const std::pair<FlowControl, QString> flows[] = {{FlowControl::None, tr("None")},
                                                     {FlowControl::RtsCts, QStringLiteral("RTS/CTS")},
                                                     {FlowControl::DsrDtr, QStringLiteral("DSR/DTR")},
                                                     {FlowControl::XonXoff, QStringLiteral("XON/XOFF")}};
    for (const auto &[value, label] : flows)
        m_flow->addItem(label, enumName(value));
    m_flow->setAccessibleName(tr("Flow control"));
    form->addRow(tr("&Flow control"), m_flow);
    return page;
}

QWidget *SettingsDialog::buildTerminal()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);

    m_sendEnding = new QComboBox(page);
    const std::pair<LineEnding, QString> sends[] = {{LineEnding::Cr, tr("CR (typical serial console)")},
                                                    {LineEnding::Lf, QStringLiteral("LF")},
                                                    {LineEnding::CrLf, QStringLiteral("CR LF")},
                                                    {LineEnding::None, tr("Send nothing extra")}};
    for (const auto &[value, label] : sends)
        m_sendEnding->addItem(label, enumName(value));
    m_sendEnding->setAccessibleName(tr("Enter sends"));
    form->addRow(tr("Enter &sends"), m_sendEnding);

    m_receiveEnding = new QComboBox(page);
    const std::pair<LineEnding, QString> receives[] = {{LineEnding::None, tr("Leave as received")},
                                                       {LineEnding::Lf, tr("Normalise to LF")},
                                                       {LineEnding::Cr, tr("Normalise to CR")},
                                                       {LineEnding::CrLf, tr("Normalise to CR LF")}};
    for (const auto &[value, label] : receives)
        m_receiveEnding->addItem(label, enumName(value));
    m_receiveEnding->setAccessibleName(tr("Received endings"));
    form->addRow(tr("&Received endings"), m_receiveEnding);

    m_localEcho = new QCheckBox(tr("Echo typed characters locally"), page);
    form->addRow(QString(), m_localEcho);

    m_backspace = new QComboBox(page);
    m_backspace->addItem(tr("DEL (0x7F)"), enumName(Backspace::Del));
    m_backspace->addItem(tr("BS (0x08)"), enumName(Backspace::Bs));
    m_backspace->setAccessibleName(tr("Backspace sends"));
    form->addRow(tr("Bac&kspace sends"), m_backspace);

    m_charset = new QComboBox(page);
    for (const QString &name : enumNameList<CharacterSet>())
        m_charset->addItem(name, name);
    m_charset->setAccessibleName(tr("Character set"));
    form->addRow(tr("Character se&t"), m_charset);

    m_stripHighBit = new QCheckBox(tr("Strip the high bit from received bytes"), page);
    form->addRow(QString(), m_stripHighBit);

    m_scrollback = new QSpinBox(page);
    m_scrollback->setRange(0, 1'000'000);
    m_scrollback->setSingleStep(1000);
    m_scrollback->setAccessibleName(tr("Scrollback lines"));
    form->addRow(tr("Scr&ollback lines"), m_scrollback);

    m_fontFamily = new QLineEdit(page);
    m_fontFamily->setPlaceholderText(tr("System monospace"));
    m_fontFamily->setAccessibleName(tr("Font family"));
    form->addRow(tr("Font f&amily"), m_fontFamily);

    m_fontSize = new QSpinBox(page);
    m_fontSize->setRange(6, 48);
    m_fontSize->setAccessibleName(tr("Font size"));
    form->addRow(tr("Font si&ze"), m_fontSize);

    m_selectToCopy = new QCheckBox(tr("Copy the selection as soon as it is made"), page);
    form->addRow(QString(), m_selectToCopy);
    return page;
}

QWidget *SettingsDialog::buildLogging()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);

    m_logEnabled = new QCheckBox(tr("Start logging when the session connects"), page);
    form->addRow(QString(), m_logEnabled);

    m_logFormat = new QComboBox(page);
    m_logFormat->addItem(tr("Text (decoded, timestamped)"), enumName(LogFormat::Text));
    m_logFormat->addItem(tr("Raw (byte-exact)"), enumName(LogFormat::Raw));
    m_logFormat->setAccessibleName(tr("Log format"));
    form->addRow(tr("For&mat"), m_logFormat);

    auto *directoryRow = new QWidget(page);
    auto *directoryLayout = new QVBoxLayout(directoryRow);
    directoryLayout->setContentsMargins(0, 0, 0, 0);
    m_logDirectory = new QLineEdit(directoryRow);
    m_logDirectory->setPlaceholderText(tr("Default log folder for this platform"));
    m_logDirectory->setAccessibleName(tr("Log folder"));
    directoryLayout->addWidget(m_logDirectory);
    auto *browse = new QPushButton(tr("Browse…"), directoryRow);
    browse->setAccessibleName(tr("Browse for a log folder"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString chosen = QFileDialog::getExistingDirectory(this, tr("Choose a log folder"));
        if (!chosen.isEmpty())
            m_logDirectory->setText(chosen);
    });
    directoryLayout->addWidget(browse);
    form->addRow(tr("Folde&r"), directoryRow);

    m_logTemplate = new QLineEdit(page);
    m_logTemplate->setAccessibleName(tr("Log file name"));
    connect(m_logTemplate, &QLineEdit::textChanged, this, &SettingsDialog::updateTemplatePreview);
    form->addRow(tr("File&name"), m_logTemplate);

    m_templatePreview = new QLabel(page);
    m_templatePreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Preview"), m_templatePreview);

    m_timestamps = new QComboBox(page);
    const std::pair<TimestampMode, QString> modes[] = {{TimestampMode::Off, tr("Off")},
                                                       {TimestampMode::Absolute, tr("Absolute")},
                                                       {TimestampMode::Elapsed, tr("Elapsed since connect")},
                                                       {TimestampMode::Delta, tr("Delta between lines")}};
    for (const auto &[value, label] : modes)
        m_timestamps->addItem(label, enumName(value));
    m_timestamps->setAccessibleName(tr("Timestamps"));
    form->addRow(tr("Timestam&ps"), m_timestamps);

    m_rotateBytes = new QSpinBox(page);
    m_rotateBytes->setRange(0, 1'000'000'000);
    m_rotateBytes->setSingleStep(1'048'576);
    m_rotateBytes->setSpecialValueText(tr("No rotation"));
    m_rotateBytes->setAccessibleName(tr("Rotate after bytes"));
    form->addRow(tr("Rotate after (bytes)"), m_rotateBytes);

    m_keepFiles = new QSpinBox(page);
    m_keepFiles->setRange(1, 100);
    m_keepFiles->setAccessibleName(tr("Rotated files to keep"));
    form->addRow(tr("Keep files"), m_keepFiles);
    return page;
}

QWidget *SettingsDialog::buildPacing()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);

    m_charDelay = new QDoubleSpinBox(page);
    m_charDelay->setRange(0.0, 1.0);
    m_charDelay->setDecimals(3);
    m_charDelay->setSingleStep(0.005);
    m_charDelay->setSuffix(tr(" s"));
    m_charDelay->setAccessibleName(tr("Delay per character"));
    form->addRow(tr("Delay per &character"), m_charDelay);

    m_lineDelay = new QDoubleSpinBox(page);
    m_lineDelay->setRange(0.0, 10.0);
    m_lineDelay->setDecimals(3);
    m_lineDelay->setSingleStep(0.05);
    m_lineDelay->setSuffix(tr(" s"));
    m_lineDelay->setAccessibleName(tr("Delay per line"));
    form->addRow(tr("Delay per &line"), m_lineDelay);

    m_waitForPrompt = new QLineEdit(page);
    m_waitForPrompt->setPlaceholderText(QStringLiteral("[>#]\\s*$"));
    m_waitForPrompt->setAccessibleName(tr("Wait for prompt"));
    connect(m_waitForPrompt, &QLineEdit::textChanged, this, &SettingsDialog::validatePrompt);
    form->addRow(tr("&Wait for prompt"), m_waitForPrompt);

    m_promptTimeout = new QDoubleSpinBox(page);
    m_promptTimeout->setRange(0.1, 120.0);
    m_promptTimeout->setSuffix(tr(" s"));
    m_promptTimeout->setAccessibleName(tr("Prompt timeout"));
    form->addRow(tr("Prompt &timeout"), m_promptTimeout);

    m_abortOnTimeout = new QCheckBox(tr("Stop the paste if the prompt never arrives"), page);
    form->addRow(QString(), m_abortOnTimeout);

    m_breakMode = new QComboBox(page);
    const std::pair<BreakMode, QString> modes[] = {{BreakMode::Auto, tr("Automatic (native, else emulated)")},
                                                   {BreakMode::Native, tr("Native only")},
                                                   {BreakMode::Emulated, tr("Emulated only")}};
    for (const auto &[value, label] : modes)
        m_breakMode->addItem(label, enumName(value));
    m_breakMode->setAccessibleName(tr("Break mode"));
    form->addRow(tr("Break &mode"), m_breakMode);

    m_breakDuration = new QDoubleSpinBox(page);
    m_breakDuration->setRange(0.01, 5.0);
    m_breakDuration->setSingleStep(0.05);
    m_breakDuration->setSuffix(tr(" s"));
    m_breakDuration->setAccessibleName(tr("Break duration"));
    form->addRow(tr("Break &duration"), m_breakDuration);
    return page;
}

// -- load and save ------------------------------------------------------------

void SettingsDialog::select(QComboBox *combo, const QVariant &value)
{
    const int index = combo->findData(value);
    if (index >= 0)
        combo->setCurrentIndex(index);
}

void SettingsDialog::load(const Profile &profile)
{
    m_profile = profile;
    const SerialSettings &serial = profile.serial;
    m_baud->setCurrentText(QString::number(serial.baudrate));
    select(m_dataBits, serial.dataBits);
    select(m_parity, enumName(serial.parity));
    select(m_stopBits, enumName(serial.stopBits));
    select(m_flow, enumName(serial.flowControl));

    const TextPolicy &text = profile.text;
    select(m_sendEnding, enumName(text.sendEnding));
    select(m_receiveEnding, enumName(text.receiveEnding));
    m_localEcho->setChecked(text.localEcho);
    select(m_backspace, enumName(text.backspace));

    const TerminalOptions &terminal = profile.terminal;
    select(m_charset, enumName(terminal.charset));
    m_stripHighBit->setChecked(terminal.stripHighBit);
    m_scrollback->setValue(terminal.scrollback);
    m_fontFamily->setText(terminal.fontFamily);
    m_fontSize->setValue(terminal.fontSize > 0 ? terminal.fontSize : kDefaultFontSize);
    m_selectToCopy->setChecked(terminal.selectToCopy);

    const LogSettings &logs = profile.logging;
    m_logEnabled->setChecked(logs.autoStart);
    select(m_logFormat, enumName(logs.format));
    m_logDirectory->setText(logs.directory);
    m_logTemplate->setText(logs.filenameTemplate.isEmpty() ? kDefaultLogTemplate : logs.filenameTemplate);
    select(m_timestamps, enumName(logs.timestamps));
    m_rotateBytes->setValue(int(std::min<qint64>(logs.maxBytes, m_rotateBytes->maximum())));
    m_keepFiles->setValue(logs.keep);

    const PacePolicy &pace = profile.pacing;
    m_charDelay->setValue(pace.charDelay);
    m_lineDelay->setValue(pace.lineDelay);
    m_waitForPrompt->setText(pace.waitForPrompt);
    m_promptTimeout->setValue(pace.promptTimeout);
    m_abortOnTimeout->setChecked(pace.abortOnTimeout);

    select(m_breakMode, enumName(profile.breakMode));
    m_breakDuration->setValue(profile.breakDuration);
    updateTemplatePreview();
}

Profile SettingsDialog::toProfile() const
{
    Profile profile = m_profile;

    bool ok = false;
    const int baud = m_baud->currentText().trimmed().toInt(&ok);
    if (!ok)
        throw invalidSettings(tr("'%1' is not a baud rate").arg(m_baud->currentText()));
    profile.serial.baudrate = baud;
    profile.serial.dataBits = m_dataBits->currentData().toInt();
    profile.serial.parity = *enumFromName<Parity>(m_parity->currentData().toString());
    profile.serial.stopBits = *enumFromName<StopBits>(m_stopBits->currentData().toString());
    profile.serial.flowControl = *enumFromName<FlowControl>(m_flow->currentData().toString());
    profile.serial.validate();

    profile.text.sendEnding = *enumFromName<LineEnding>(m_sendEnding->currentData().toString());
    profile.text.receiveEnding = *enumFromName<LineEnding>(m_receiveEnding->currentData().toString());
    profile.text.localEcho = m_localEcho->isChecked();
    profile.text.backspace = *enumFromName<Backspace>(m_backspace->currentData().toString());

    // Columns, rows and theme have no control here and are kept as they were.
    profile.terminal.scrollback = m_scrollback->value();
    profile.terminal.charset = *enumFromName<CharacterSet>(m_charset->currentData().toString());
    profile.terminal.stripHighBit = m_stripHighBit->isChecked();
    profile.terminal.fontFamily = m_fontFamily->text().trimmed();
    profile.terminal.fontSize = m_fontSize->value();
    profile.terminal.selectToCopy = m_selectToCopy->isChecked();

    // The log's own character set has no control either, so a hand-edited
    // value survives the dialog.
    profile.logging.directory = m_logDirectory->text().trimmed();
    const QString pattern = m_logTemplate->text().trimmed();
    profile.logging.filenameTemplate = pattern.isEmpty() ? kDefaultLogTemplate : pattern;
    profile.logging.format = *enumFromName<LogFormat>(m_logFormat->currentData().toString());
    profile.logging.timestamps = *enumFromName<TimestampMode>(m_timestamps->currentData().toString());
    profile.logging.autoStart = m_logEnabled->isChecked();
    profile.logging.maxBytes = m_rotateBytes->value();
    profile.logging.keep = m_keepFiles->value();

    profile.pacing.charDelay = m_charDelay->value();
    profile.pacing.lineDelay = m_lineDelay->value();
    profile.pacing.waitForPrompt = m_waitForPrompt->text().trimmed();
    profile.pacing.promptTimeout = m_promptTimeout->value();
    profile.pacing.abortOnTimeout = m_abortOnTimeout->isChecked();
    profile.pacing.validate();

    profile.breakMode = *enumFromName<BreakMode>(m_breakMode->currentData().toString());
    profile.breakDuration = m_breakDuration->value();
    return profile;
}

QString SettingsDialog::errorText() const
{
    return m_error->text();
}

// -- validation ---------------------------------------------------------------

void SettingsDialog::acceptIfValid()
{
    try {
        (void)toProfile();
    } catch (const PortsideError &error) {
        m_error->setText(error.text());
        return;
    }
    m_error->clear();
    accept();
}

void SettingsDialog::validatePrompt(const QString &pattern)
{
    if (pattern.isEmpty()) {
        m_waitForPrompt->setStyleSheet(QString());
        m_error->clear();
        return;
    }
    const QRegularExpression expression(pattern);
    if (!expression.isValid()) {
        m_waitForPrompt->setStyleSheet(QStringLiteral("border: 1px solid #c92a2a;"));
        m_error->setText(tr("Invalid prompt pattern: %1").arg(expression.errorString()));
        return;
    }
    m_waitForPrompt->setStyleSheet(QString());
    m_error->clear();
}

void SettingsDialog::updateTemplatePreview()
{
    const QString pattern = m_logTemplate->text().trimmed();
    const QString name = m_profile.name.isEmpty() ? QStringLiteral("port") : m_profile.name;
    m_templatePreview->setText(
        resolveTemplate(pattern.isEmpty() ? kDefaultLogTemplate : pattern, name, m_profile.name));
}

} // namespace portside::gui
