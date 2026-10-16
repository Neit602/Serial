#include "gui/signals_panel.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>

namespace portside::gui {

namespace {

constexpr int kPollMs = 1000;
const QStringList kLineNames = {QStringLiteral("CTS"), QStringLiteral("DSR"),
                                QStringLiteral("DCD"), QStringLiteral("RI")};

QString styleFor(const std::optional<bool> &value)
{
    if (!value)
        return QStringLiteral("color: #adb5bd; font-style: italic;");
    return *value ? QStringLiteral("color: #2f9e44; font-weight: bold;")
                  : QStringLiteral("color: #868e96;");
}

} // namespace

SignalsPanel::SignalsPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 0, 4, 0);
    for (const QString &name : kLineNames) {
        auto *label = new QLabel(name, this);
        label->setAccessibleName(tr("%1 signal").arg(name));
        m_indicators.insert(name, label);
        layout->addWidget(label);
    }
    layout->addSpacing(12);

    m_dtr = new QCheckBox(QStringLiteral("DTR"), this);
    m_dtr->setAccessibleName(tr("Data Terminal Ready"));
    // Lines are asserted on open, as pySerial did and most terminals do.
    m_dtr->setChecked(true);
    connect(m_dtr, &QCheckBox::toggled, this, &SignalsPanel::dtrToggled);
    layout->addWidget(m_dtr);

    m_rts = new QCheckBox(QStringLiteral("RTS"), this);
    m_rts->setAccessibleName(tr("Request To Send"));
    m_rts->setChecked(true);
    connect(m_rts, &QCheckBox::toggled, this, &SignalsPanel::rtsToggled);
    layout->addWidget(m_rts);
    layout->addStretch(1);

    setLines(ModemLines{});
    m_timer = new QTimer(this);
    m_timer->setInterval(kPollMs);
}

void SignalsPanel::setLines(const ModemLines &lines)
{
    const std::optional<bool> values[] = {lines.cts, lines.dsr, lines.dcd, lines.ri};
    for (int i = 0; i < kLineNames.size(); ++i) {
        const QString &name = kLineNames.at(i);
        const auto &value = values[i];
        const QString state = !value ? tr("not reported by this driver")
                                     : (*value ? tr("asserted") : tr("low"));
        QLabel *label = m_indicators.value(name);
        label->setStyleSheet(styleFor(value));
        label->setToolTip(QStringLiteral("%1: %2").arg(name, state));
        label->setAccessibleDescription(QStringLiteral("%1 %2").arg(name, state));
        m_states.insert(name, state);
    }
}

QString SignalsPanel::stateOf(const QString &name) const
{
    return m_states.value(name);
}

void SignalsPanel::setControlLinesSupported(bool supported)
{
    for (QCheckBox *box : {m_dtr, m_rts}) {
        box->setEnabled(supported);
        box->setToolTip(supported ? QString()
                                  : tr("This adapter's driver does not implement this line."));
    }
}

void SignalsPanel::setEnabledForSession(bool connected)
{
    setEnabled(connected);
    if (!connected)
        setLines(ModemLines{});
}

void SignalsPanel::startPolling(const std::function<void()> &poll)
{
    connect(m_timer, &QTimer::timeout, this, poll);
    m_timer->start();
}

void SignalsPanel::stopPolling()
{
    m_timer->stop();
}

} // namespace portside::gui
