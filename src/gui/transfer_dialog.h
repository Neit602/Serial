#pragma once

// Choosing what to send or where to receive (PRD F17–F19): the protocol, the
// files or the download folder, and the few options a protocol needs.  Only
// the controls that matter for the chosen protocol are enabled, so the dialog
// cannot describe a transfer the engines would refuse.

#include "core/transfer/transfer.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace portside::gui {

class TransferDialog : public QDialog
{
    Q_OBJECT

public:
    TransferDialog(TransferDirection direction, const TransferOptions &defaults,
                   const QStringList &files = {}, QWidget *parent = nullptr);

    TransferDirection direction() const { return m_direction; }
    TransferOptions options() const;
    QStringList files() const;
    void setFiles(const QStringList &files);
    // Why OK is disabled (empty when the transfer can start).
    QString problem() const;

    QComboBox *protocol() const { return m_protocol; }
    QListWidget *fileList() const { return m_files; }
    QLineEdit *directory() const { return m_directory; }
    QComboBox *overwrite() const { return m_overwrite; }
    QLineEdit *outputName() const { return m_outputName; }
    QCheckBox *textMode() const { return m_textMode; }
    QCheckBox *resume() const { return m_resume; }
    QSpinBox *chunkSize() const { return m_chunkSize; }
    QDoubleSpinBox *chunkDelay() const { return m_chunkDelay; }
    QPushButton *okButton() const;

private:
    void addFiles();
    void removeFiles();
    void browseDirectory();
    void update();
    Protocol chosenProtocol() const;

    const TransferDirection m_direction;
    TransferOptions m_defaults;
    QComboBox *m_protocol = nullptr;
    QListWidget *m_files = nullptr;
    QLineEdit *m_directory = nullptr;
    QComboBox *m_overwrite = nullptr;
    QLineEdit *m_outputName = nullptr;
    QCheckBox *m_textMode = nullptr;
    QCheckBox *m_resume = nullptr;
    QSpinBox *m_chunkSize = nullptr;
    QDoubleSpinBox *m_chunkDelay = nullptr;
    QLabel *m_hint = nullptr;
    QLabel *m_error = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};

// One-line advice for starting the other end, per protocol and direction.
QString transferHint(Protocol protocol, TransferDirection direction);

} // namespace portside::gui
