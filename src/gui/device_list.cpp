#include "gui/device_list.h"

#include "core/identity.h"

#include <QHeaderView>
#include <QLabel>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace portside::gui {

DeviceList::DeviceList(PortLister lister, int pollMs, QWidget *parent)
    : QWidget(parent)
    , m_lister(std::move(lister))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({tr("Port"), tr("Device"), tr("VID:PID"), tr("Serial")});
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setAccessibleName(tr("Serial ports"));
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    connect(m_tree, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem *item) { onActivated(item); });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item) { onActivated(item); });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &DeviceList::portSelected);
    layout->addWidget(m_tree);

    m_empty = new QLabel(tr("No serial ports found.\n\n"
                            "Plug in an adapter and this list will refresh automatically.\n"
                            "If it is already plugged in, check that its driver is installed and "
                            "that you have permission to open the port."),
                         this);
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setAccessibleName(tr("No ports found"));
    layout->addWidget(m_empty);

    m_timer = new QTimer(this);
    m_timer->setInterval(pollMs);
    connect(m_timer, &QTimer::timeout, this, &DeviceList::refresh);
    m_timer->start();
    refresh();
}

void DeviceList::refresh()
{
    const QList<PortInfo> current = listPorts(m_lister);
    if (current == m_ports && m_tree->topLevelItemCount() == current.size())
        return;

    const auto selected = selectedPort();
    m_ports = current;
    const QSignalBlocker blocker(m_tree);
    m_tree->clear();
    QTreeWidgetItem *reselect = nullptr;
    for (int index = 0; index < current.size(); ++index) {
        const PortInfo &port = current.at(index);
        const QString device = !port.chipFamily.isEmpty()   ? port.chipFamily
                               : !port.description.isEmpty() ? port.description
                                                             : tr("serial port");
        auto *item = new QTreeWidgetItem({port.device, device,
                                          port.vidPid().isEmpty() ? QStringLiteral("-") : port.vidPid(),
                                          port.serialNumber.isEmpty() ? QStringLiteral("-") : port.serialNumber});
        item->setData(0, Qt::UserRole, index);
        item->setToolTip(0, tooltipFor(port));
        m_tree->addTopLevelItem(item);
        if (selected && port.device == selected->device)
            reselect = item;
    }
    if (reselect != nullptr) {
        reselect->setSelected(true);
        m_tree->setCurrentItem(reselect);
    }
    const bool hasPorts = !current.isEmpty();
    m_tree->setVisible(hasPorts);
    m_empty->setVisible(!hasPorts);
    // The selection may have vanished with the port.
    if (selected && !reselect)
        Q_EMIT portSelected();
}

QString DeviceList::tooltipFor(const PortInfo &port)
{
    QStringList lines = {port.label(), DeviceIdentity::fromPort(port).describe()};
    if (!port.driver.isEmpty())
        lines << tr("driver: %1").arg(port.driver);
    if (!port.stablePath.isEmpty())
        lines << tr("stable path: %1").arg(port.stablePath);
    return lines.join(QLatin1Char('\n'));
}

std::optional<PortInfo> DeviceList::selectedPort() const
{
    const auto items = m_tree->selectedItems();
    if (items.isEmpty())
        return std::nullopt;
    const int index = items.first()->data(0, Qt::UserRole).toInt();
    if (index < 0 || index >= m_ports.size())
        return std::nullopt;
    return m_ports.at(index);
}

bool DeviceList::selectDevice(const QString &device)
{
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_tree->topLevelItem(i);
        if (item->text(0) == device) {
            m_tree->setCurrentItem(item);
            item->setSelected(true);
            return true;
        }
    }
    return false;
}

void DeviceList::activateSelected()
{
    if (const auto port = selectedPort())
        Q_EMIT portActivated(*port);
}

void DeviceList::onActivated(QTreeWidgetItem *item)
{
    const int index = item->data(0, Qt::UserRole).toInt();
    if (index >= 0 && index < m_ports.size())
        Q_EMIT portActivated(m_ports.at(index));
}

void DeviceList::stop()
{
    m_timer->stop();
}

} // namespace portside::gui
