#pragma once

// Modem line indicators and DTR/RTS toggles (PRD F4).
//
// Three states per indicator, not two: a driver that does not report a line
// gives "unknown", and the panel says so rather than claiming the line is low
// (DECISIONS D33).

#include "core/transport.h"

#include <QHash>
#include <QWidget>

#include <functional>

class QCheckBox;
class QLabel;
class QTimer;

namespace portside::gui {

class SignalsPanel : public QWidget
{
    Q_OBJECT

public:
    explicit SignalsPanel(QWidget *parent = nullptr);

    // Update the indicators from a reading (F4).
    void setLines(const ModemLines &lines);
    // The rendered state of one indicator: "asserted", "low" or
    // "not reported by this driver".
    QString stateOf(const QString &name) const;
    // Disable DTR/RTS when the driver has no such lines, and say why.
    void setControlLinesSupported(bool supported);
    // Grey the whole panel out when nothing is connected.
    void setEnabledForSession(bool connected);
    // Poll the modem lines once a second while running.
    void startPolling(const std::function<void()> &poll);
    void stopPolling();

    QCheckBox *dtrBox() const { return m_dtr; }
    QCheckBox *rtsBox() const { return m_rts; }

Q_SIGNALS:
    void dtrToggled(bool value);
    void rtsToggled(bool value);

private:
    QHash<QString, QLabel *> m_indicators;
    QHash<QString, QString> m_states;
    QCheckBox *m_dtr = nullptr;
    QCheckBox *m_rts = nullptr;
    QTimer *m_timer = nullptr;
};

} // namespace portside::gui
