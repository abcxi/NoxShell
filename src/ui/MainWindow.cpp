#include "MainWindow.h"

#include "../core/SshSession.h"
#include "../core/CredentialStore.h"
#include "../core/ServerRepository.h"
#include "FilePanel.h"
#include "HostSidebar.h"
#include "MetricCard.h"
#include "MonitoringThresholdDialog.h"
#ifdef Q_OS_MACOS
#include "MacTitleBarControls.h"
#endif
#include "ServerDialog.h"
#include "TerminalWorkspace.h"
#include "TrendChart.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDebug>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#ifndef NOXSHELL_APP_VERSION
#define NOXSHELL_APP_VERSION "0.0.0"
#endif

namespace noxshell::ui {

namespace {
QString formatGib(quint64 bytes)
{
    constexpr double bytesPerGib = 1024.0 * 1024.0 * 1024.0;
    return QString::number(static_cast<double>(bytes) / bytesPerGib, 'f', 1);
}

bool isConnectingMessage(const QString &message)
{
    return message.startsWith(QStringLiteral("正在"))
        || message.startsWith(QStringLiteral("TCP 连接"))
        || message.startsWith(QStringLiteral("等待确认"));
}

} // namespace

MainWindow::MainWindow(QString databasePath, QWidget *parent)
    : QMainWindow(parent)
    , m_repository(new ServerRepository(std::move(databasePath), true, this))
    , m_credentialStore(new CredentialStore(this))
{
    setWindowTitle(QStringLiteral("玄壳 · SSH 远程管理"));
    setMinimumSize(1180, 720);
    resize(1440, 900);
#ifdef Q_OS_MACOS
    m_nativeTitleBarControls = QApplication::platformName() == QStringLiteral("cocoa");
#endif
    if (!m_nativeTitleBarControls) addToolBar(Qt::TopToolBarArea, createWindowToolbar());

    if (!m_repository->initialize()) {
        QMessageBox::critical(this, QStringLiteral("数据初始化失败"), m_repository->lastError());
    }
    auto *root = new QWidget;
    root->setObjectName(QStringLiteral("appRoot"));
    auto *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto *body = new QWidget;
    auto *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    auto servers = m_repository->loadServers();
    for (auto &server : servers) server.state = ServerState::Offline;
    m_sidebar = new HostSidebar(std::move(servers));
    bodyLayout->addWidget(m_sidebar);
    bodyLayout->addWidget(createWorkspace(), 1);
    rootLayout->addWidget(body, 1);
    rootLayout->addWidget(createStatusBar());
    setCentralWidget(root);

    if (m_sidebarToggleButton) {
        connect(m_sidebarToggleButton, &QToolButton::clicked, this, [this] {
            setSidebarVisible(m_sidebar->isHidden());
        });
    }
    if (m_monitorToggleButton) {
        connect(m_monitorToggleButton, &QToolButton::clicked, this, [this] {
            setMonitorVisible(m_monitorRail->isHidden());
        });
    }
#ifdef Q_OS_MACOS
    if (m_nativeTitleBarControls) {
        QTimer::singleShot(0, this, [this] {
            const auto install = [this] {
                return installMacTitleBarControls(this,
                    [this] { setSidebarVisible(m_sidebar->isHidden()); },
                    [this] { setMonitorVisible(m_monitorRail->isHidden()); });
            };
            if (install()) {
                qInfo().noquote() << QStringLiteral("macOS 原生标题栏双栏开关已安装");
            } else {
                QTimer::singleShot(100, this, [this] {
                    const bool installed = installMacTitleBarControls(this,
                        [this] { setSidebarVisible(m_sidebar->isHidden()); },
                        [this] { setMonitorVisible(m_monitorRail->isHidden()); });
                    if (installed) qInfo().noquote() << QStringLiteral("macOS 原生标题栏双栏开关已安装");
                    else qWarning().noquote() << QStringLiteral("macOS 原生标题栏双栏开关安装失败");
                });
            }
        });
    }
#endif

    connect(m_sidebar, &HostSidebar::serverConnectRequested, this, &MainWindow::connectToServer);
    connect(m_sidebar, &HostSidebar::collapseRequested, this, [this] {
        setSidebarVisible(false);
    });
    connect(m_sidebar, &HostSidebar::serverEditRequested, this, &MainWindow::selectAndEditServer);
    connect(m_sidebar, &HostSidebar::serverDuplicateRequested, this, &MainWindow::duplicateServer);
    connect(m_sidebar, &HostSidebar::serverDeleteRequested, this, &MainWindow::selectAndDeleteServer);
    connect(m_sidebar, &HostSidebar::addServerRequested, this, &MainWindow::addServer);
    m_metricTimer = new QTimer(this);
    m_metricTimer->setInterval(1000);
    connect(m_metricTimer, &QTimer::timeout, this, &MainWindow::requestMetrics);
    m_metricTimer->start();

    QTimer::singleShot(0, this, [this] {
        m_sidebar->selectFirstServer();
    });
}

QToolBar *MainWindow::createWindowToolbar()
{
    auto *toolbar = new QToolBar(this);
    toolbar->setObjectName(QStringLiteral("windowControlsToolbar"));
    toolbar->setAllowedAreas(Qt::TopToolBarArea);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setIconSize(QSize(20, 20));

    m_sidebarToggleButton = new QToolButton;
    m_sidebarToggleButton->setObjectName(QStringLiteral("sidebarToggleButton"));
    m_sidebarToggleButton->setIcon(QIcon(QStringLiteral(":/assets/sidebar-collapse.svg")));
    m_sidebarToggleButton->setIconSize(QSize(20, 20));
    m_sidebarToggleButton->setFixedSize(30, 28);
    m_sidebarToggleButton->setToolTip(QStringLiteral("隐藏主机列表"));
    m_sidebarToggleButton->setAccessibleName(QStringLiteral("隐藏主机列表"));

    m_monitorToggleButton = new QToolButton;
    m_monitorToggleButton->setObjectName(QStringLiteral("monitorToggleButton"));
    m_monitorToggleButton->setIcon(QIcon(QStringLiteral(":/assets/monitor-collapse.svg")));
    m_monitorToggleButton->setIconSize(QSize(20, 20));
    m_monitorToggleButton->setFixedSize(30, 28);
    m_monitorToggleButton->setToolTip(QStringLiteral("隐藏实时监控栏"));
    m_monitorToggleButton->setAccessibleName(QStringLiteral("隐藏实时监控栏"));

    toolbar->addWidget(m_sidebarToggleButton);
    toolbar->addWidget(m_monitorToggleButton);
    return toolbar;
}

void MainWindow::setSidebarVisible(bool visible)
{
    if (!m_sidebar) return;
    m_sidebar->setVisible(visible);
    const auto action = visible ? QStringLiteral("隐藏主机列表") : QStringLiteral("显示主机列表");
    if (m_sidebarToggleButton) {
        m_sidebarToggleButton->setIcon(QIcon(visible
                ? QStringLiteral(":/assets/sidebar-collapse.svg")
                : QStringLiteral(":/assets/sidebar-expand.svg")));
        m_sidebarToggleButton->setToolTip(action);
        m_sidebarToggleButton->setAccessibleName(action);
    }
#ifdef Q_OS_MACOS
    if (m_nativeTitleBarControls) updateMacTitleBarControls(this, visible, m_monitorRail && m_monitorRail->isVisible());
#endif
}

void MainWindow::setMonitorVisible(bool visible)
{
    if (!m_monitorRail) return;
    m_monitorRail->setVisible(visible);
    const auto action = visible ? QStringLiteral("隐藏实时监控栏") : QStringLiteral("显示实时监控栏");
    if (m_monitorToggleButton) {
        m_monitorToggleButton->setIcon(QIcon(visible
                ? QStringLiteral(":/assets/monitor-collapse.svg")
                : QStringLiteral(":/assets/monitor-expand.svg")));
        m_monitorToggleButton->setToolTip(action);
        m_monitorToggleButton->setAccessibleName(action);
    }
#ifdef Q_OS_MACOS
    if (m_nativeTitleBarControls) updateMacTitleBarControls(this, m_sidebar && m_sidebar->isVisible(), visible);
#endif
}

QWidget *MainWindow::createWorkspace()
{
    auto *workspace = new QWidget;
    auto *layout = new QVBoxLayout(workspace);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *mainSplitter = new QSplitter(Qt::Horizontal);
    mainSplitter->setObjectName(QStringLiteral("mainWorkspaceSplitter"));
    mainSplitter->setChildrenCollapsible(false);
    mainSplitter->addWidget(createMetricStrip());

    auto *operations = new QWidget;
    operations->setObjectName(QStringLiteral("operationsWorkspace"));
    auto *operationsLayout = new QVBoxLayout(operations);
    operationsLayout->setContentsMargins(0, 0, 0, 0);
    operationsLayout->setSpacing(0);

    auto *terminalFileSplitter = new QSplitter(Qt::Vertical);
    terminalFileSplitter->setObjectName(QStringLiteral("terminalFileSplitter"));
    terminalFileSplitter->setChildrenCollapsible(false);
    terminalFileSplitter->addWidget(createTerminalWorkspace());
    terminalFileSplitter->addWidget(createFileWorkspace());
    terminalFileSplitter->setStretchFactor(0, 65);
    terminalFileSplitter->setStretchFactor(1, 35);
    terminalFileSplitter->setSizes({560, 300});
    operationsLayout->addWidget(terminalFileSplitter, 1);

    mainSplitter->addWidget(operations);
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);
    mainSplitter->setSizes({252, 944});
    layout->addWidget(mainSplitter, 1);
    return workspace;
}

QWidget *MainWindow::createMetricStrip()
{
    auto *rail = new QWidget;
    m_monitorRail = rail;
    rail->setObjectName(QStringLiteral("monitorRail"));
    rail->setMinimumWidth(236);
    rail->setMaximumWidth(360);
    auto *railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(0, 0, 0, 0);
    railLayout->setSpacing(0);

    auto *identity = new QFrame;
    identity->setObjectName(QStringLiteral("monitorIdentity"));
    auto *identityLayout = new QVBoxLayout(identity);
    identityLayout->setContentsMargins(13, 10, 12, 9);
    identityLayout->setSpacing(5);
    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(5);
    m_serverMeta = new QLabel(QStringLiteral("未选择主机"));
    m_serverMeta->setObjectName(QStringLiteral("serverAddress"));
    auto *copyAddress = new QToolButton;
    copyAddress->setObjectName(QStringLiteral("copyHostAddressButton"));
    copyAddress->setIcon(QIcon(QStringLiteral(":/assets/copy.svg")));
    copyAddress->setIconSize(QSize(15, 15));
    copyAddress->setFixedSize(27, 27);
    copyAddress->setToolTip(QStringLiteral("复制 IP 地址"));
    copyAddress->setAccessibleName(QStringLiteral("复制 IP 地址"));
    m_onlineBadge = new QLabel(QStringLiteral("○ 待连接"));
    m_onlineBadge->setObjectName(QStringLiteral("onlineBadge"));
    titleRow->addWidget(m_serverMeta);
    titleRow->addWidget(copyAddress);
    titleRow->addStretch();
    titleRow->addWidget(m_onlineBadge);
    identityLayout->addLayout(titleRow);
    railLayout->addWidget(identity);
    connect(copyAddress, &QToolButton::clicked, this, [this] {
        if (!m_currentServer.host.isEmpty()) QApplication::clipboard()->setText(m_currentServer.host);
    });

    auto *monitorHeading = new QWidget;
    monitorHeading->setObjectName(QStringLiteral("monitorHeading"));
    auto *headingLayout = new QHBoxLayout(monitorHeading);
    headingLayout->setContentsMargins(12, 8, 10, 7);
    auto *heading = new QLabel(QStringLiteral("实时监控"));
    heading->setStyleSheet(QStringLiteral("font-size:15px;font-weight:650;"));
    auto *sampleHint = new QLabel(QStringLiteral("1 秒"));
    sampleHint->setObjectName(QStringLiteral("mutedLabel"));
    headingLayout->addWidget(heading);
    headingLayout->addStretch();
    headingLayout->addWidget(sampleHint);
    railLayout->addWidget(monitorHeading);

    auto *scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("monitorScrollArea"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 2, 10, 10);
    layout->setSpacing(8);

    m_cpuCard = new MetricCard(QStringLiteral("CPU 使用率"), QColor(QStringLiteral("#006EFF")));
    m_memoryCard = new MetricCard(QStringLiteral("内存使用率"), QColor(QStringLiteral("#8B5CF6")));
    m_loadCard = new MetricCard(QStringLiteral("系统负载"), QColor(QStringLiteral("#00A870")));
    m_diskCard = new MetricCard(QStringLiteral("磁盘使用率"), QColor(QStringLiteral("#ED7B2F")));
    for (auto *card : {m_cpuCard, m_memoryCard, m_loadCard, m_diskCard}) {
        layout->addWidget(card);
    }

    auto *alert = new QFrame;
    alert->setObjectName(QStringLiteral("alertCard"));
    auto *alertLayout = new QVBoxLayout(alert);
    alertLayout->setContentsMargins(13, 11, 13, 9);
    m_alertTitle = new QLabel(QStringLiteral("磁盘空间状态"));
    m_alertTitle->setObjectName(QStringLiteral("alertTitle"));
    m_alertText = new QLabel(QStringLiteral("等待磁盘采样数据…"));
    m_alertText->setObjectName(QStringLiteral("alertText"));
    m_alertText->setWordWrap(true);
    alertLayout->addWidget(m_alertTitle);
    alertLayout->addWidget(m_alertText);
    layout->addWidget(alert);

    auto *trendToggle = new QToolButton;
    trendToggle->setObjectName(QStringLiteral("monitorTrendToggle"));
    trendToggle->setText(QStringLiteral("展开实时趋势"));
    trendToggle->setCheckable(true);
    trendToggle->setArrowType(Qt::RightArrow);
    trendToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    trendToggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *monitorDetails = createMonitorWorkspace();
    monitorDetails->setVisible(false);
    connect(trendToggle, &QToolButton::toggled, this, [trendToggle, monitorDetails](bool expanded) {
        trendToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        trendToggle->setText(expanded ? QStringLiteral("收起实时趋势") : QStringLiteral("展开实时趋势"));
        monitorDetails->setVisible(expanded);
    });
    layout->addWidget(trendToggle);
    layout->addWidget(monitorDetails);
    layout->addStretch();
    scroll->setWidget(content);
    railLayout->addWidget(scroll, 1);

    resetMetrics(QStringLiteral("等待 SSH 连接"));
    return rail;
}

QWidget *MainWindow::createTerminalWorkspace()
{
    auto *container = new QWidget;
    container->setObjectName(QStringLiteral("terminalWorkspacePane"));
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(8, 8, 8, 4);
    layout->setSpacing(0);
    m_terminalWorkspace = new TerminalWorkspace(m_repository, m_credentialStore);
    layout->addWidget(m_terminalWorkspace);
    connect(m_terminalWorkspace, &TerminalWorkspace::activeSessionChanged,
        this, &MainWindow::activateTerminalSession);
    connect(m_terminalWorkspace, &TerminalWorkspace::sessionClosed,
        this, &MainWindow::releaseSession);
    connect(m_terminalWorkspace, &TerminalWorkspace::sessionConnectionChanged,
        this, [this](const QString &serverId, bool connected, const QString &message) {
            updateConnectionPresentation(serverId, connected, message);
        });
    connect(m_terminalWorkspace, &TerminalWorkspace::commandSubmitted, this,
        [this](const QString &serverId, const QString &command) {
            QTimer::singleShot(180, this, [this, serverId, command] {
                if (m_filePanel && serverId == m_currentServer.id) {
                    m_filePanel->syncDirectoryFromTerminalCommand(command);
                }
            });
        });
    return container;
}

QWidget *MainWindow::createMonitorWorkspace()
{
    auto *widget = new QWidget;
    widget->setObjectName(QStringLiteral("monitorDetails"));
    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(9);

    auto *toolbar = new QWidget;
    toolbar->setObjectName(QStringLiteral("monitorToolbar"));
    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(8, 6, 8, 6);
    m_historyRange = new QComboBox;
    m_historyRange->setObjectName(QStringLiteral("historyRange"));
    m_historyRange->addItem(QStringLiteral("最近 1 分钟"), 60);
    m_historyRange->addItem(QStringLiteral("最近 15 分钟"), 900);
    m_historyRange->addItem(QStringLiteral("最近 60 分钟"), 3600);
    m_historyRange->setCurrentIndex(1);
    m_historyRange->setFixedWidth(118);
    auto *thresholdButton = new QPushButton(QStringLiteral("告警阈值"));
    thresholdButton->setObjectName(QStringLiteral("monitorThresholdButton"));
    toolbarLayout->addWidget(thresholdButton);
    toolbarLayout->addWidget(m_historyRange);
    layout->addWidget(toolbar);

    m_cpuTrend = new TrendChart(QStringLiteral("CPU 使用率"), TrendMetric::Cpu, QColor(QStringLiteral("#006EFF")));
    m_memoryTrend = new TrendChart(QStringLiteral("内存使用率"), TrendMetric::Memory, QColor(QStringLiteral("#8B5CF6")));
    m_loadTrend = new TrendChart(QStringLiteral("系统负载 / CPU 容量"), TrendMetric::Load, QColor(QStringLiteral("#00A870")));
    m_diskTrend = new TrendChart(QStringLiteral("根磁盘使用率"), TrendMetric::Disk, QColor(QStringLiteral("#ED7B2F")));
    m_cpuTrend->setObjectName(QStringLiteral("cpuTrendChart"));
    m_memoryTrend->setObjectName(QStringLiteral("memoryTrendChart"));
    m_loadTrend->setObjectName(QStringLiteral("loadTrendChart"));
    m_diskTrend->setObjectName(QStringLiteral("diskTrendChart"));
    for (auto *chart : {m_cpuTrend, m_memoryTrend, m_loadTrend, m_diskTrend}) {
        chart->setMinimumHeight(170);
        chart->setMaximumHeight(210);
        layout->addWidget(chart);
    }

    auto *alerts = new QWidget;
    alerts->setObjectName(QStringLiteral("monitorAlerts"));
    auto *alertsLayout = new QVBoxLayout(alerts);
    alertsLayout->setContentsMargins(9, 6, 9, 7);
    alertsLayout->setSpacing(4);
    auto *alertsTitle = new QLabel(QStringLiteral("最近告警事件"));
    alertsTitle->setStyleSheet(QStringLiteral("font-weight:650;"));
    m_alertList = new QListWidget;
    m_alertList->setObjectName(QStringLiteral("monitorAlertList"));
    m_alertList->setMaximumHeight(74);
    alertsLayout->addWidget(alertsTitle);
    alertsLayout->addWidget(m_alertList);
    layout->addWidget(alerts);

    connect(m_historyRange, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_historyWindowSeconds = m_historyRange->itemData(index).toInt();
        refreshTrendCharts();
    });
    connect(thresholdButton, &QPushButton::clicked, this, &MainWindow::configureMonitoringThresholds);
    return widget;
}

QWidget *MainWindow::createFileWorkspace()
{
    auto *widget = new QWidget;
    widget->setObjectName(QStringLiteral("fileWorkspacePane"));
    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(8, 4, 8, 8);
    m_fileWorkspaceStack = new QStackedWidget;
    m_fileWorkspaceStack->setObjectName(QStringLiteral("fileWorkspaceStack"));
    m_filePlaceholder = new QLabel(QStringLiteral("选择 SSH 会话后显示文件管理"));
    auto *placeholderLabel = qobject_cast<QLabel *>(m_filePlaceholder);
    placeholderLabel->setAlignment(Qt::AlignCenter);
    placeholderLabel->setStyleSheet(QStringLiteral("color:#8B9AAF;background:#FBFCFD;"));
    m_fileWorkspaceStack->addWidget(m_filePlaceholder);
    layout->addWidget(m_fileWorkspaceStack);
    return widget;
}

QWidget *MainWindow::createStatusBar()
{
    auto *bar = new QFrame;
    bar->setObjectName(QStringLiteral("statusBar"));
    bar->setFixedHeight(24);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(12, 0, 12, 0);
    auto *safe = new QLabel(QStringLiteral("● 已验证主机指纹"));
    safe->setStyleSheet(QStringLiteral("color:#008858;font-size:10px;"));
    m_sampleStatus = new QLabel(QStringLiteral("采样等待中    传输任务 0"));
    m_sampleStatus->setObjectName(QStringLiteral("mutedLabel"));
    auto *version = new QLabel(QStringLiteral("玄壳 v%1 · C++20 / Qt 6").arg(QString::fromLatin1(NOXSHELL_APP_VERSION)));
    version->setObjectName(QStringLiteral("mutedLabel"));
    layout->addWidget(safe);
    layout->addSpacing(15);
    layout->addWidget(m_sampleStatus);
    layout->addStretch();
    layout->addWidget(version);
    return bar;
}

void MainWindow::selectServer(const ServerProfile &profile)
{
    if (profile.id.isEmpty()) return;
    m_currentServer = profile;
    m_serverDeletionInFlight = false;
    m_hasMetricSample = false;
    m_metricHistory.clear();
    for (const auto &point : m_repository->loadMetricHistory(profile.id, QDateTime::currentDateTime().addSecs(-3600))) {
        m_metricHistory.appendPoint(point);
    }
    m_thresholds = m_repository->loadMonitoringThresholds(profile.id);
    m_activeAlerts.clear();
    refreshAlertList();
    refreshTrendCharts();
    resetMetrics(QStringLiteral("等待连接 · 双击主机或右键连接"));
    m_serverMeta->setText(QStringLiteral("%1:%2").arg(profile.host).arg(profile.port));
    const bool connectedNow = m_terminalWorkspace && m_terminalWorkspace->hasConnectedSession(profile.id);
    if (connectedNow) {
        m_currentServer.state = ServerState::Online;
        m_sidebar->setServerState(profile.id, ServerState::Online);
        setConnectionBadge(QStringLiteral("● 在线"), QStringLiteral("#008858"), QStringLiteral("#E8F8F2"));
    } else {
        m_currentServer.state = ServerState::Offline;
        m_sidebar->setServerState(profile.id, ServerState::Offline);
        setConnectionBadge(QStringLiteral("○ 离线"), QStringLiteral("#738297"), QStringLiteral("#EEF2F6"));
    }
}

void MainWindow::activateTerminalSession(const ServerProfile &profile, SshSession *session)
{
    if (profile.id.isEmpty() || !session) return;
    if (m_sidebar) m_sidebar->selectServerById(profile.id);
    selectServer(profile);
    bindSession(session);
    m_session = session;
    showFilePanel(profile, session);
    if (session->isConnected()) requestMetrics();
}

void MainWindow::bindSession(SshSession *session)
{
    if (!session || m_boundSessions.contains(session)) return;
    m_boundSessions.insert(session);
    connect(session, &SshSession::metricSampleReceived, this, [this, session](const MetricSample &sample) {
        if (session == m_session) displayMetrics(sample);
    });
    connect(session, &SshSession::metricsCollectionFailed, this, [this, session](const QString &message) {
        if (session != m_session) return;
        if (!m_hasMetricSample) resetMetrics(QStringLiteral("采样失败"));
        if (m_sampleStatus) m_sampleStatus->setText(QStringLiteral("采样失败 · %1").arg(message));
    });
    connect(session, &SshSession::connectionChanged, this, [this, session](bool connected, const QString &) {
        if (connected && session == m_session) requestMetrics();
    });
}

void MainWindow::releaseSession(SshSession *session)
{
    if (!session) return;
    m_boundSessions.remove(session);
    disconnect(session, nullptr, this, nullptr);
    if (auto *panel = m_filePanels.take(session)) {
        if (m_fileWorkspaceStack) m_fileWorkspaceStack->removeWidget(panel);
        panel->deleteLater();
    }
    if (m_session == session) {
        m_session = nullptr;
        m_filePanel = nullptr;
        if (m_fileWorkspaceStack && m_filePlaceholder) m_fileWorkspaceStack->setCurrentWidget(m_filePlaceholder);
    }
}

void MainWindow::showFilePanel(const ServerProfile &profile, SshSession *session)
{
    if (!m_fileWorkspaceStack || !session) return;
    auto *panel = m_filePanels.value(session);
    if (!panel) {
        panel = new FilePanel(session);
        panel->setMinimumHeight(220);
        panel->setServer(profile);
        m_filePanels.insert(session, panel);
        m_fileWorkspaceStack->addWidget(panel);
    } else {
        panel->updateServer(profile);
    }
    m_filePanel = panel;
    m_fileWorkspaceStack->setCurrentWidget(panel);
}

void MainWindow::connectToServer(const ServerProfile &profile)
{
    if (profile.id.isEmpty()) return;
    selectServer(profile);
    if (m_terminalWorkspace) m_terminalWorkspace->openOrActivate(profile);
}

void MainWindow::selectAndEditServer(const ServerProfile &profile)
{
    editServer(profile);
}

void MainWindow::selectAndDeleteServer(const ServerProfile &profile)
{
    deleteServer(profile);
}

void MainWindow::duplicateServer(const ServerProfile &profile)
{
    if (profile.id.isEmpty()) return;
    auto duplicate = profile;
    const auto sourceCredentialRef = duplicate.credentialRef;
    duplicate.id.clear();
    duplicate.credentialRef.clear();
    duplicate.name = QStringLiteral("%1 副本").arg(profile.name);
    duplicate.state = ServerState::Offline;
    if (duplicate.authentication != AuthenticationMethod::SshAgent && !sourceCredentialRef.isEmpty()) {
        const auto secret = m_credentialStore->load(sourceCredentialRef);
        duplicate.password = secret.password;
        duplicate.keyPassphrase = secret.keyPassphrase;
    }
    if (persistProfile(duplicate, false)) m_sidebar->addServer(duplicate);
}

bool MainWindow::persistProfile(ServerProfile &profile, bool preserveEmptySecret)
{
    if (profile.id.isEmpty()) profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (profile.credentialRef.isEmpty()) profile.credentialRef = QStringLiteral("server/%1").arg(profile.id);

    CredentialSecret secret;
    if (preserveEmptySecret && profile.authentication != AuthenticationMethod::SshAgent) {
        secret = m_credentialStore->load(profile.credentialRef);
        if (!m_credentialStore->lastError().isEmpty() && secret.password.isEmpty() && secret.keyPassphrase.isEmpty()) {
            // Missing credentials are allowed when switching from Agent to a new password/key entered below.
        }
    }
    if (!profile.password.isEmpty()) secret.password = profile.password;
    if (!profile.keyPassphrase.isEmpty()) secret.keyPassphrase = profile.keyPassphrase;
    if (profile.authentication == AuthenticationMethod::SshAgent) {
        if (preserveEmptySecret && !profile.credentialRef.isEmpty()) m_credentialStore->remove(profile.credentialRef);
    } else if (!m_credentialStore->save(profile.credentialRef, secret)) {
        QMessageBox::critical(this, QStringLiteral("保存凭据失败"), m_credentialStore->lastError());
        return false;
    }
    profile.password.clear();
    profile.keyPassphrase.clear();
    if (!m_repository->saveServer(profile)) {
        QMessageBox::critical(this, QStringLiteral("保存服务器失败"), m_repository->lastError());
        return false;
    }
    return true;
}

void MainWindow::addServer()
{
    ServerDialog dialog(this);
    dialog.setConnectionServices(m_repository, m_credentialStore);
    if (dialog.exec() != QDialog::Accepted) return;
    auto profile = dialog.profile();
    if (persistProfile(profile, false)) m_sidebar->addServer(profile);
}

void MainWindow::editServer(const ServerProfile &sourceProfile)
{
    if (sourceProfile.id.isEmpty()) return;
    auto editable = sourceProfile;
    editable.expectedFingerprint = m_repository->knownHostFingerprint(editable.host, editable.port);
    ServerDialog dialog(editable, this);
    dialog.setConnectionServices(m_repository, m_credentialStore);
    if (dialog.exec() != QDialog::Accepted) return;
    auto profile = dialog.profile();
    const auto runtimeState = sourceProfile.state;
    profile.state = ServerState::Offline;
    profile.connectionMode = sourceProfile.connectionMode;
    if (!persistProfile(profile, true)) return;
    profile.state = runtimeState;
    m_sidebar->updateServer(profile);
    if (m_terminalWorkspace) m_terminalWorkspace->updateServer(profile);
}

void MainWindow::deleteServer(const ServerProfile &removedProfile)
{
    if (removedProfile.id.isEmpty() || m_serverDeletionInFlight) return;
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("删除服务器"),
        QStringLiteral("确定删除“%1”（%2:%3）吗？服务器配置和关联凭据会被移除，此操作不可撤销。")
            .arg(removedProfile.name, removedProfile.host).arg(removedProfile.port),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    m_serverDeletionInFlight = true;
    if (m_terminalWorkspace) m_terminalWorkspace->closeServer(removedProfile.id);
    if (!m_repository->deleteServer(removedProfile.id)) {
        m_serverDeletionInFlight = false;
        QMessageBox::critical(this, QStringLiteral("删除失败"), m_repository->lastError());
        return;
    }
    if (!removedProfile.credentialRef.isEmpty()) m_credentialStore->remove(removedProfile.credentialRef);
    m_sidebar->removeServer(removedProfile.id);
    if (m_currentServer.id == removedProfile.id) {
        m_currentServer = {};
        m_serverMeta->setText(QStringLiteral("未选择主机"));
        setConnectionBadge(m_sidebar->servers().isEmpty() ? QStringLiteral("○ 无主机") : QStringLiteral("○ 待连接"),
            QStringLiteral("#738297"), QStringLiteral("#EEF2F6"));
        resetMetrics(QStringLiteral("等待选择主机"));
        m_session = nullptr;
        m_filePanel = nullptr;
        if (m_fileWorkspaceStack && m_filePlaceholder) m_fileWorkspaceStack->setCurrentWidget(m_filePlaceholder);
    }
    m_serverDeletionInFlight = false;
}

void MainWindow::setConnectionBadge(const QString &text, const QString &foreground, const QString &background)
{
    if (!m_onlineBadge) return;
    m_onlineBadge->setText(text);
    m_onlineBadge->setStyleSheet(QStringLiteral("color:%1;background:%2;border-radius:3px;padding:2px 8px;")
            .arg(foreground, background));
}

void MainWindow::updateConnectionPresentation(const QString &serverId, bool connected, const QString &message)
{
    if (serverId.isEmpty()) return;
    const bool terminalConnected = m_terminalWorkspace && m_terminalWorkspace->hasConnectedSession(serverId);
    const bool anyConnected = connected || terminalConnected;

    if (anyConnected) {
        m_sidebar->setServerState(serverId, ServerState::Online);
        if (m_currentServer.id == serverId) {
            m_currentServer.state = ServerState::Online;
            setConnectionBadge(QStringLiteral("● 在线"), QStringLiteral("#008858"), QStringLiteral("#E8F8F2"));
        }
        return;
    }
    if (isConnectingMessage(message)) {
        if (m_currentServer.id == serverId) {
            setConnectionBadge(QStringLiteral("◌ 连接中"), QStringLiteral("#006EFF"), QStringLiteral("#E8F3FF"));
        }
        return;
    }

    m_sidebar->setServerState(serverId, ServerState::Offline);
    if (m_currentServer.id == serverId) {
        m_currentServer.state = ServerState::Offline;
        setConnectionBadge(QStringLiteral("○ 离线"), QStringLiteral("#738297"), QStringLiteral("#EEF2F6"));
    }
}

void MainWindow::requestMetrics()
{
    if (!m_session || !m_session->isConnected()) return;
    if (m_sampleStatus) m_sampleStatus->setText(QStringLiteral("正在采样…    传输任务 0"));
    m_session->requestMetrics();
}

void MainWindow::displayMetrics(const MetricSample &sample)
{
    m_hasMetricSample = true;
    m_metricHistory.append(sample);
    m_repository->saveMetricSample(m_currentServer.id, sample);
    evaluateMonitoringAlerts(sample);
    refreshTrendCharts();
    const int cpuProgress = qBound(0, qRound(sample.cpuPercent), 100);
    if (sample.cpuReady) {
        m_cpuCard->setValue(QStringLiteral("%1%").arg(sample.cpuPercent, 0, 'f', 1),
            QStringLiteral("内核态 %1%").arg(sample.kernelPercent, 0, 'f', 1), cpuProgress);
    } else {
        m_cpuCard->setValue(QStringLiteral("--"), QStringLiteral("已建立差分基线"), 0);
    }

    const int memoryProgress = qBound(0, qRound(sample.memoryPercent), 100);
    m_memoryCard->setValue(QStringLiteral("%1%").arg(sample.memoryPercent, 0, 'f', 1),
        QStringLiteral("%1 / %2 GiB").arg(formatGib(sample.memoryUsedBytes), formatGib(sample.memoryTotalBytes)), memoryProgress);

    const int loadProgress = qBound(0, qRound(sample.load1 * 100.0 / qMax(1, sample.cpuCoreCount)), 100);
    m_loadCard->setValue(QString::number(sample.load1, 'f', 2),
        QStringLiteral("1m / %1 核 · 5m %2").arg(sample.cpuCoreCount).arg(sample.load5, 0, 'f', 2), loadProgress);

    const auto &disk = sample.primaryDisk;
    if (disk.totalBytes > 0) {
        m_diskCard->setValue(QStringLiteral("%1%").arg(disk.usagePercent),
            QStringLiteral("%1 · %2 GiB").arg(disk.fileSystem, formatGib(disk.usedBytes)), disk.usagePercent);
        const bool warning = disk.usagePercent >= m_thresholds.diskPercent;
        m_alertTitle->setText(warning ? QStringLiteral("⚠  磁盘空间预警") : QStringLiteral("✓  磁盘空间正常"));
        m_alertText->setText(warning
            ? QStringLiteral("%1 已使用 %2%，可用 %3 GiB\n建议检查日志和临时文件。")
                  .arg(disk.mountPoint).arg(disk.usagePercent).arg(formatGib(disk.availableBytes))
            : QStringLiteral("%1 已使用 %2%，可用 %3 GiB\n磁盘空间处于正常范围。")
                  .arg(disk.mountPoint).arg(disk.usagePercent).arg(formatGib(disk.availableBytes)));
    } else {
        m_diskCard->setValue(QStringLiteral("--"), QStringLiteral("未发现持久化磁盘"), 0);
        m_alertTitle->setText(QStringLiteral("磁盘空间状态"));
        m_alertText->setText(QStringLiteral("未发现可展示的持久化磁盘。"));
    }

    if (m_sampleStatus) {
        m_sampleStatus->setText(QStringLiteral("采样 %1    周期 1.0 s")
                                    .arg(sample.capturedAt.toString(QStringLiteral("HH:mm:ss"))));
    }
}

void MainWindow::resetMetrics(const QString &detail)
{
    if (!m_cpuCard) return;
    for (auto *card : {m_cpuCard, m_memoryCard, m_loadCard, m_diskCard}) {
        card->setValue(QStringLiteral("--"), detail, 0);
    }
    if (m_alertTitle) m_alertTitle->setText(QStringLiteral("磁盘空间状态"));
    if (m_alertText) m_alertText->setText(QStringLiteral("等待磁盘采样数据…"));
    if (m_sampleStatus) m_sampleStatus->setText(detail + QStringLiteral("    周期 1.0 s"));
}

void MainWindow::refreshTrendCharts()
{
    if (!m_cpuTrend) return;
    const auto points = m_metricHistory.points(m_historyWindowSeconds);
    for (auto *chart : {m_cpuTrend, m_memoryTrend, m_loadTrend, m_diskTrend}) {
        chart->setSeries(points, m_historyWindowSeconds);
    }
}

void MainWindow::configureMonitoringThresholds()
{
    if (m_currentServer.id.isEmpty()) return;
    MonitoringThresholdDialog dialog(m_thresholds, this);
    if (dialog.exec() != QDialog::Accepted) return;
    const auto thresholds = dialog.thresholds();
    if (!m_repository->saveMonitoringThresholds(m_currentServer.id, thresholds)) {
        QMessageBox::critical(this, QStringLiteral("保存阈值失败"), m_repository->lastError());
        return;
    }
    m_thresholds = thresholds;
    m_activeAlerts.clear();
}

void MainWindow::evaluateMonitoringAlerts(const MetricSample &sample)
{
    if (m_currentServer.id.isEmpty()) return;
    struct Check { QString key; QString label; double value; double threshold; bool valid; };
    const QVector<Check> checks{
        {QStringLiteral("cpu"), QStringLiteral("CPU"), sample.cpuPercent, m_thresholds.cpuPercent, sample.cpuReady},
        {QStringLiteral("memory"), QStringLiteral("内存"), sample.memoryPercent, m_thresholds.memoryPercent, true},
        {QStringLiteral("load"), QStringLiteral("负载"), sample.load1 * 100.0 / qMax(1, sample.cpuCoreCount), m_thresholds.loadPercent, true},
        {QStringLiteral("disk"), QStringLiteral("磁盘"), static_cast<double>(sample.primaryDisk.usagePercent),
            m_thresholds.diskPercent, sample.primaryDisk.totalBytes > 0},
    };
    bool recorded = false;
    for (const auto &check : checks) {
        if (!check.valid) continue;
        if (check.value >= check.threshold && !m_activeAlerts.contains(check.key)) {
            MonitoringAlert alert;
            alert.serverId = m_currentServer.id;
            alert.metric = check.label;
            alert.value = check.value;
            alert.threshold = check.threshold;
            alert.createdAt = sample.capturedAt;
            recorded = m_repository->recordMonitoringAlert(alert) || recorded;
            m_activeAlerts.insert(check.key);
        } else if (check.value <= check.threshold - 3.0) {
            m_activeAlerts.remove(check.key);
        }
    }
    if (recorded) refreshAlertList();
}

void MainWindow::refreshAlertList()
{
    if (!m_alertList) return;
    m_alertList->clear();
    if (m_currentServer.id.isEmpty()) return;
    const auto alerts = m_repository->loadMonitoringAlerts(m_currentServer.id, 20);
    if (alerts.isEmpty()) {
        m_alertList->addItem(QStringLiteral("暂无告警，当前指标均在阈值范围内。"));
        return;
    }
    for (const auto &alert : alerts) {
        m_alertList->addItem(QStringLiteral("%1   %2达到 %3%（阈值 %4%）")
            .arg(alert.createdAt.toString(QStringLiteral("MM-dd HH:mm:ss")), alert.metric)
            .arg(alert.value, 0, 'f', 1).arg(alert.threshold, 0, 'f', 1));
    }
}

} // namespace noxshell::ui
