#include "SystemDetailPanel.h"

#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QTabBar>
#include <QSet>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace noxshell::ui {

namespace {
QString formatBytes(double bytes)
{
    static constexpr const char *units[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int unit = 0;
    while (bytes >= 1024.0 && unit < 3) {
        bytes /= 1024.0;
        ++unit;
    }
    const int precision = bytes >= 100.0 || unit == 0 ? 0 : 1;
    return QStringLiteral("%1 %2").arg(bytes, 0, 'f', precision).arg(QString::fromLatin1(units[unit]));
}

QString formatCapacity(quint64 bytes)
{
    const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gib >= 1.0) return QStringLiteral("%1G").arg(gib, 0, 'f', gib >= 10.0 ? 0 : 1);
    const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return QStringLiteral("%1M").arg(mib, 0, 'f', mib >= 10.0 ? 0 : 1);
}

void configureTree(QTreeWidget *tree)
{
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    tree->setSelectionMode(QAbstractItemView::NoSelection);
    tree->setFocusPolicy(Qt::NoFocus);
    tree->setUniformRowHeights(true);
    tree->header()->setStretchLastSection(false);
    tree->header()->setFixedHeight(24);
    tree->setIndentation(0);
    tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}
} // namespace

SystemDetailPanel::SystemDetailPanel(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("systemDetailPanel"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto *networkCard = new QFrame;
    networkCard->setObjectName(QStringLiteral("networkSectionCard"));
    auto *networkLayout = new QVBoxLayout(networkCard);
    networkLayout->setContentsMargins(10, 10, 10, 8);
    networkLayout->setSpacing(8);

    auto *networkHeader = new QHBoxLayout;
    auto *networkTitle = new QLabel(QStringLiteral("网络流速"));
    networkTitle->setObjectName(QStringLiteral("detailSectionTitle"));
    m_networkInterface = new QComboBox;
    m_networkInterface->setObjectName(QStringLiteral("networkInterfaceCombo"));
    m_networkInterface->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_networkInterface->setMinimumContentsLength(8);
    m_networkInterface->setFixedWidth(100);
    m_networkInterface->setMaxVisibleItems(8);
    auto *networkView = new QListView(m_networkInterface);
    networkView->setUniformItemSizes(true);
    networkView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_networkInterface->setView(networkView);
    networkHeader->addWidget(networkTitle);
    networkHeader->addStretch();
    networkHeader->addWidget(m_networkInterface);
    networkLayout->addLayout(networkHeader);

    auto *rateRow = new QWidget;
    rateRow->setObjectName(QStringLiteral("networkRateRow"));
    auto *rateLayout = new QHBoxLayout(rateRow);
    rateLayout->setContentsMargins(0, 1, 0, 0);
    rateLayout->setSpacing(8);
    m_uploadRate = new QLabel(QStringLiteral("↑ --"));
    m_uploadRate->setObjectName(QStringLiteral("networkUploadRate"));
    m_uploadRate->setAccessibleName(QStringLiteral("上传速率"));
    m_uploadRate->setToolTip(QStringLiteral("上传速率"));
    m_downloadRate = new QLabel(QStringLiteral("↓ --"));
    m_downloadRate->setObjectName(QStringLiteral("networkDownloadRate"));
    m_downloadRate->setAccessibleName(QStringLiteral("下载速率"));
    m_downloadRate->setToolTip(QStringLiteral("下载速率"));
    m_downloadRate->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    rateLayout->addWidget(m_uploadRate, 1);
    rateLayout->addWidget(m_downloadRate, 1);
    networkLayout->addWidget(rateRow);

    m_networkChart = new NetworkRateChart;
    networkLayout->addWidget(m_networkChart);
    layout->addWidget(networkCard);

    auto *processCard = new QFrame;
    processCard->setObjectName(QStringLiteral("processSectionCard"));
    auto *processLayout = new QVBoxLayout(processCard);
    processLayout->setContentsMargins(8, 8, 8, 8);
    processLayout->setSpacing(7);

    m_processTabs = new QTabBar;
    m_processTabs->setObjectName(QStringLiteral("processMetricTabs"));
    m_processTabs->setExpanding(true);
    m_processTabs->setDrawBase(false);
    m_processTabs->addTab(QStringLiteral("CPU"));
    m_processTabs->addTab(QStringLiteral("内存"));
    m_processTabs->addTab(QStringLiteral("命令"));
    processLayout->addWidget(m_processTabs);

    m_processList = new QTreeWidget;
    m_processList->setObjectName(QStringLiteral("realtimeProcessList"));
    // 1 行表头 + 4 行进程 + 边框，固定为紧凑四行且不产生内部滚动。
    m_processList->setFixedHeight(122);
    m_processList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    configureTree(m_processList);
    processLayout->addWidget(m_processList);
    layout->addWidget(processCard);

    auto *fileSystemCard = new QFrame;
    fileSystemCard->setObjectName(QStringLiteral("fileSystemSectionCard"));
    auto *fileSystemLayout = new QVBoxLayout(fileSystemCard);
    fileSystemLayout->setContentsMargins(8, 10, 8, 8);
    fileSystemLayout->setSpacing(7);

    auto *fileSystemTitle = new QLabel(QStringLiteral("目录 / 挂载点占用"));
    fileSystemTitle->setObjectName(QStringLiteral("detailSectionTitle"));
    fileSystemLayout->addWidget(fileSystemTitle);
    m_fileSystemList = new QTreeWidget;
    m_fileSystemList->setObjectName(QStringLiteral("fileSystemUsageList"));
    m_fileSystemList->setHeaderLabels({QStringLiteral("路径"), QStringLiteral("可用/总量")});
    m_fileSystemList->setMinimumHeight(120);
    m_fileSystemList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    configureTree(m_fileSystemList);
    m_fileSystemList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_fileSystemList->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    fileSystemLayout->addWidget(m_fileSystemList);
    layout->addWidget(fileSystemCard, 1);

    m_emptyHint = new QLabel(QStringLiteral("等待实时采样数据"));
    m_emptyHint->setObjectName(QStringLiteral("detailMuted"));
    m_emptyHint->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_emptyHint);

    connect(m_networkInterface, &QComboBox::currentIndexChanged, this, [this] { refreshNetwork(); });
    connect(m_processTabs, &QTabBar::currentChanged, this, [this] { refreshProcesses(); });
}

void SystemDetailPanel::setSample(const MetricSample &sample)
{
    const QString selectedInterface = m_networkInterface->currentData().toString();
    m_sample = sample;

    const qint64 now = sample.capturedAt.isValid() ? sample.capturedAt.toMSecsSinceEpoch()
                                                  : QDateTime::currentMSecsSinceEpoch();
    if (!m_sample.capturedAt.isValid()) m_sample.capturedAt = QDateTime::fromMSecsSinceEpoch(now);
    const auto appendRate = [this, now](const QString &name, double upload, double download) {
        auto &history = m_networkHistory[name];
        if (!history.isEmpty() && now < history.last().timestampMs) history.clear();
        if (!history.isEmpty() && now == history.last().timestampMs) history.removeLast();
        history.append({now, upload, download});
        while (!history.isEmpty() && (history.first().timestampMs < now - 60000 || history.size() > 240))
            history.removeFirst();
    };
    QSet<QString> activeInterfaces{QStringLiteral("__all__")};

    double totalUpload = 0.0;
    double totalDownload = 0.0;
    for (const auto &network : sample.networkRates) {
        totalUpload += network.transmittedBytesPerSecond;
        totalDownload += network.receivedBytesPerSecond;
        appendRate(network.interfaceName, network.transmittedBytesPerSecond, network.receivedBytesPerSecond);
        activeInterfaces.insert(network.interfaceName);
    }
    if (!sample.networkRates.isEmpty()) appendRate(QStringLiteral("__all__"), totalUpload, totalDownload);
    for (auto it = m_networkHistory.begin(); it != m_networkHistory.end();) {
        if (!activeInterfaces.contains(it.key())) it = m_networkHistory.erase(it); else ++it;
    }

    m_networkInterface->blockSignals(true);
    m_networkInterface->clear();
    m_networkInterface->addItem(QStringLiteral("全部网卡"), QStringLiteral("__all__"));
    for (const auto &network : sample.networkRates)
        m_networkInterface->addItem(network.interfaceName, network.interfaceName);
    int selectedIndex = m_networkInterface->findData(selectedInterface);
    if (selectedIndex < 0) selectedIndex = 0;
    m_networkInterface->setCurrentIndex(selectedIndex);
    m_networkInterface->blockSignals(false);

    refreshNetwork();
    refreshProcesses();
    refreshFileSystems();
    m_emptyHint->setVisible(sample.networkRates.isEmpty() && sample.processes.isEmpty() && sample.disks.isEmpty());
}

void SystemDetailPanel::reset(const QString &detail)
{
    m_sample = {};
    m_networkHistory.clear();
    m_networkInterface->clear();
    m_uploadRate->setText(QStringLiteral("↑ --"));
    m_downloadRate->setText(QStringLiteral("↓ --"));
    m_networkChart->setRates({}, 0);
    m_processList->clear();
    m_fileSystemList->clear();
    m_emptyHint->setText(detail);
    m_emptyHint->show();
}

void SystemDetailPanel::refreshNetwork()
{
    const QString selected = m_networkInterface->currentData().toString();
    if (selected == QStringLiteral("__all__")) {
        if (m_sample.networkRates.isEmpty()) {
            m_uploadRate->setText(QStringLiteral("↑ --"));
            m_downloadRate->setText(QStringLiteral("↓ --"));
            m_networkChart->setRates({}, 0);
            return;
        }
        double upload = 0.0;
        double download = 0.0;
        for (const auto &network : m_sample.networkRates) {
            upload += network.transmittedBytesPerSecond;
            download += network.receivedBytesPerSecond;
        }
        m_uploadRate->setText(QStringLiteral("↑ %1").arg(formatBytes(upload)));
        m_downloadRate->setText(QStringLiteral("↓ %1").arg(formatBytes(download)));
        m_networkChart->setRates(m_networkHistory.value(selected), m_sample.capturedAt.toMSecsSinceEpoch());
        return;
    }
    const auto network = std::find_if(m_sample.networkRates.cbegin(), m_sample.networkRates.cend(),
        [&selected](const LinuxNetworkRate &candidate) { return candidate.interfaceName == selected; });
    if (network == m_sample.networkRates.cend()) {
        m_uploadRate->setText(QStringLiteral("↑ --"));
        m_downloadRate->setText(QStringLiteral("↓ --"));
        m_networkChart->setRates({}, 0);
        return;
    }
    m_uploadRate->setText(QStringLiteral("↑ %1").arg(formatBytes(network->transmittedBytesPerSecond)));
    m_downloadRate->setText(QStringLiteral("↓ %1").arg(formatBytes(network->receivedBytesPerSecond)));
    m_networkChart->setRates(m_networkHistory.value(selected), m_sample.capturedAt.toMSecsSinceEpoch());
}

void SystemDetailPanel::refreshProcesses()
{
    m_processList->clear();
    auto processes = m_sample.processes;
    const int tab = m_processTabs->currentIndex();
    if (tab == 1) {
        std::sort(processes.begin(), processes.end(), [](const auto &left, const auto &right) {
            return left.memoryPercent > right.memoryPercent;
        });
        m_processList->setHeaderLabels({QStringLiteral("命令"), QStringLiteral("内存"), QStringLiteral("PID")});
    } else if (tab == 2) {
        std::sort(processes.begin(), processes.end(), [](const auto &left, const auto &right) {
            return left.pid < right.pid;
        });
        m_processList->setHeaderLabels({QStringLiteral("命令"), QStringLiteral("用户"), QStringLiteral("PID")});
    } else {
        std::sort(processes.begin(), processes.end(), [](const auto &left, const auto &right) {
            return left.cpuPercent > right.cpuPercent;
        });
        m_processList->setHeaderLabels({QStringLiteral("命令"), QStringLiteral("CPU"), QStringLiteral("PID")});
    }
    const int count = qMin(4, processes.size());
    for (int index = 0; index < count; ++index) {
        const auto &process = processes.at(index);
        const QString value = tab == 1 ? QStringLiteral("%1%").arg(process.memoryPercent, 0, 'f', 1)
            : tab == 2 ? process.user
                       : QStringLiteral("%1%").arg(process.cpuPercent, 0, 'f', 1);
        auto *item = new QTreeWidgetItem({process.command, value, QString::number(process.pid)});
        item->setToolTip(0, process.command);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        m_processList->addTopLevelItem(item);
    }
    m_processList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_processList->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_processList->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_processList->headerItem()->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    m_processList->headerItem()->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
}

void SystemDetailPanel::refreshFileSystems()
{
    m_fileSystemList->clear();
    auto disks = m_sample.disks;
    std::sort(disks.begin(), disks.end(), [](const auto &left, const auto &right) {
        if (left.mountPoint == right.mountPoint) return false;
        if (left.mountPoint == QStringLiteral("/")) return true;
        if (right.mountPoint == QStringLiteral("/")) return false;
        return left.mountPoint < right.mountPoint;
    });
    for (const auto &disk : disks) {
        if (disk.totalBytes == 0) continue;
        auto *item = new QTreeWidgetItem({disk.mountPoint,
            QStringLiteral("%1/%2").arg(formatCapacity(disk.availableBytes), formatCapacity(disk.totalBytes))});
        item->setToolTip(0, QStringLiteral("%1 · 已使用 %2%")
                                .arg(disk.fileSystem)
                                .arg(disk.usagePercent));
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        m_fileSystemList->addTopLevelItem(item);
    }
    // 挂载点列表自身不滚动，由外层监控栏统一滚动，避免两层滚轮抢焦点。
    // 这样常见主机的全部目录能直接展开显示。
    const int visibleRows = qMax(4, m_fileSystemList->topLevelItemCount());
    m_fileSystemList->headerItem()->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    m_fileSystemList->setFixedHeight(24 + visibleRows * 24);
}

} // namespace noxshell::ui
