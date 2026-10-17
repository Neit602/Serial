#pragma once

// Connection, terminal, logging, pacing and transfer settings (PRD F3,
// F9-F20).
//
// Everything round-trips through the core's own types, so the dialog cannot
// express a configuration the core would reject — validation happens as the
// user types rather than at connect time.  Fields the dialog has no control
// for are preserved, never invented (DECISIONS D34).

#include "core/profiles.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTabWidget;

namespace portside::gui {

// Offered in the combo; any value in range can still be typed.
inline constexpr std::array<int, 14> kCommonBaudRates{300,    1200,   2400,   4800,    9600,
                                                      19200,  38400,  57600,  115200,  230400,
                                                      460800, 921600, 1500000, 3000000};

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(const Profile &profile, QWidget *parent = nullptr);

    // Fill every control from `profile`.
    void load(const Profile &profile);
    // Build a profile from the controls.  Throws PortsideError on invalid input.
    Profile toProfile() const;
    // The validation message shown under the tabs (empty when valid).
    QString errorText() const;

    QTabWidget *tabs() const { return m_tabs; }
    QComboBox *baud() const { return m_baud; }
    QComboBox *dataBits() const { return m_dataBits; }
    QComboBox *parity() const { return m_parity; }
    QComboBox *stopBits() const { return m_stopBits; }
    QComboBox *flow() const { return m_flow; }
    QComboBox *sendEnding() const { return m_sendEnding; }
    QComboBox *charset() const { return m_charset; }
    QCheckBox *localEcho() const { return m_localEcho; }
    QSpinBox *scrollback() const { return m_scrollback; }
    QLineEdit *fontFamily() const { return m_fontFamily; }
    QSpinBox *fontSize() const { return m_fontSize; }
    QCheckBox *logEnabled() const { return m_logEnabled; }
    QLineEdit *logTemplate() const { return m_logTemplate; }
    QLabel *templatePreview() const { return m_templatePreview; }
    QLineEdit *waitForPrompt() const { return m_waitForPrompt; }
    QDoubleSpinBox *lineDelay() const { return m_lineDelay; }
    QComboBox *breakMode() const { return m_breakMode; }
    QComboBox *transferProtocol() const { return m_transferProtocol; }
    QLineEdit *transferDirectory() const { return m_transferDirectory; }
    QComboBox *transferOverwrite() const { return m_transferOverwrite; }
    QCheckBox *zmodemAutoStart() const { return m_zmodemAutoStart; }

    // OK, but only when everything is valid.
    void acceptIfValid();

private:
    QWidget *buildConnection();
    QWidget *buildTerminal();
    QWidget *buildLogging();
    QWidget *buildPacing();
    QWidget *buildTransfers();
    void validatePrompt(const QString &pattern);
    void updateTemplatePreview();
    static void select(QComboBox *combo, const QVariant &value);

    Profile m_profile;
    QTabWidget *m_tabs = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
    QLabel *m_error = nullptr;

    QComboBox *m_baud = nullptr;
    QComboBox *m_dataBits = nullptr;
    QComboBox *m_parity = nullptr;
    QComboBox *m_stopBits = nullptr;
    QComboBox *m_flow = nullptr;

    QComboBox *m_sendEnding = nullptr;
    QComboBox *m_receiveEnding = nullptr;
    QCheckBox *m_localEcho = nullptr;
    QComboBox *m_backspace = nullptr;
    QComboBox *m_charset = nullptr;
    QCheckBox *m_stripHighBit = nullptr;
    QSpinBox *m_scrollback = nullptr;
    QLineEdit *m_fontFamily = nullptr;
    QSpinBox *m_fontSize = nullptr;
    QCheckBox *m_selectToCopy = nullptr;

    QCheckBox *m_logEnabled = nullptr;
    QComboBox *m_logFormat = nullptr;
    QLineEdit *m_logDirectory = nullptr;
    QLineEdit *m_logTemplate = nullptr;
    QLabel *m_templatePreview = nullptr;
    QComboBox *m_timestamps = nullptr;
    QSpinBox *m_rotateBytes = nullptr;
    QSpinBox *m_keepFiles = nullptr;

    QDoubleSpinBox *m_charDelay = nullptr;
    QDoubleSpinBox *m_lineDelay = nullptr;
    QLineEdit *m_waitForPrompt = nullptr;
    QDoubleSpinBox *m_promptTimeout = nullptr;
    QCheckBox *m_abortOnTimeout = nullptr;
    QComboBox *m_breakMode = nullptr;
    QDoubleSpinBox *m_breakDuration = nullptr;
    QComboBox *m_transferProtocol = nullptr;
    QLineEdit *m_transferDirectory = nullptr;
    QComboBox *m_transferOverwrite = nullptr;
    QCheckBox *m_zmodemAutoStart = nullptr;
};

} // namespace portside::gui
