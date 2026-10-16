#pragma once

// The device list panel (PRD F1, F2).
//
// Shows what is plugged in, refreshes on hot-plug within the PRD's two
// seconds, and is the shortest path to a connected session: pick a port,
// press Enter.

#include "core/discovery.h"
#include "core/ports.h"

#include <QList>
#include <QWidget>

#include <optional>

class QLabel;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace portside::gui {

// Polling covers every platform; 1 s keeps a replug inside F1's 2 s budget.
inline constexpr int kPortPollMs = 1000;

class DeviceList : public QWidget
{
    Q_OBJECT

public:
    explicit DeviceList(PortLister lister = {}, int pollMs = kPortPollMs, QWidget *parent = nullptr);

    // Rescan and update the list in place, keeping the selection (F1).
    void refresh();
    QList<PortInfo> ports() const { return m_ports; }
    std::optional<PortInfo> selectedPort() const;
    // Select the row for `device`; returns false when it is not listed.
    bool selectDevice(const QString &device);
    void activateSelected();
    void stop();

    QTreeWidget *tree() const { return m_tree; }
    QLabel *emptyLabel() const { return m_empty; }

Q_SIGNALS:
    // The user picked a port to connect to.
    void portActivated(const portside::PortInfo &port);
    // The selection changed (an empty optional when nothing is selected).
    void portSelected();

private:
    void onActivated(QTreeWidgetItem *item);
    static QString tooltipFor(const PortInfo &port);

    PortLister m_lister;
    QList<PortInfo> m_ports;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_empty = nullptr;
    QTimer *m_timer = nullptr;
};

} // namespace portside::gui
