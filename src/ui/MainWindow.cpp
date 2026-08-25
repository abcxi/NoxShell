#include "MainWindow.h"

#include "../core/SshSession.h"
#include "../core/CredentialStore.h"
#include "../core/ServerRepository.h"
#include "FilePanel.h"
#include "HostSidebar.h"
#include "MetricCard.h"
#ifdef Q_OS_MACOS
#include "MacTitleBarControls.h"
#endif
#include "ServerDialog.h"
#include "SystemDetailPanel.h"
#include "TerminalSettingsDialog.h"
#include "TerminalView.h"
#include "TerminalWorkspace.h"

#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <QWindow>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <windowsx.h>
#endif

#ifndef NOXSHELL_APP_VERSION
#define NOXSHELL_APP_VERSION "0.0.0"
#endif

namespace noxshell::ui {

namespace {
class WindowControlsToolbar final : public QToolBar {
public:
    explicit WindowControlsToolbar(QWidget *parent = nullptr)
        : QToolBar(parent)
    {
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            if (auto *handle = window()->windowHandle()) handle->startSystemMove();
            event->accept();
            return;
        }
        QToolBar::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            window()->isMaximized() ? window()->showNormal() : window()->showMaximized();
            event->accept();
            return;
        }
        QToolBar::mouseDoubleClickEvent(event);
    }
};

QString formatGib(quint64 bytes)
{
    constexpr double bytesPerGib = 1024.0 * 1024.0 * 1024.0;
    return QString::number(static_cast<double>(bytes) / bytesPerGib, 'f', 1);
}

QString formatUptime(quint64 seconds)
{
    const auto days = seconds / 86400;
    const auto hours = (seconds % 86400) / 3600;
    const auto minutes = (seconds % 3600) / 60;
    if (days > 0) return QStringLiteral("%1 天 %2 小时").arg(days).arg(hours);
    if (hours > 0) return QStringLiteral("%1 小时 %2 分").arg(hours).arg(minutes);
    return QStringLiteral("%1 分钟").arg(minutes);
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
#ifdef Q_OS_WIN
    // Windows 使用应用内标题栏，把三枚工作区工具按钮与最小化、最大化、
    // 关闭按钮合并为一行；macOS 继续使用原生标题栏附件。
    setWindowFlag(Qt::FramelessWindowHint, true);
#endif
    setMinimumSize(1180, 720);
    resize(1440, 900);

    auto terminalAppearance = TerminalView::defaultAppearance();
    const QSettings settings;
    terminalAppearance.fontFamily = settings.value(
        QStringLiteral("terminal/fontFamily"), terminalAppearance.fontFamily).toString();
    terminalAppearance.pointSize = settings.value(
        QStringLiteral("terminal/fontSize"), terminalAppearance.pointSize).toInt();
    terminalAppearance.lineSpacing = settings.value(
        QStringLiteral("terminal/lineSpacing"), terminalAppearance.lineSpacing).toDouble();
    TerminalView::setDefaultAppearance(terminalAppearance);
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
    m_sidebar = new HostSidebar(std::move(servers), m_repository->loadServerGroups());
    bodyLayout->addWidget(m_sidebar);
    bodyLayout->addWidget(createWorkspace(), 1);
    rootLayout->addWidget(body, 1);
    rootLayout->addWidget(createStatusBar());
    setCentralWidget(root);

    // 主机列表只在选择新会话时按需展开，启动时为终端和文件区
    // 保留更多可用空间。
    setSidebarVisible(false);

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
                    [this] { setMonitorVisible(m_monitorRail->isHidden()); },
                    [this] { showTerminalSettings(); });
            };
            if (install()) {
                updateMacTitleBarControls(this, m_sidebar && m_sidebar->isVisible(),
                    m_monitorRail && m_monitorRail->isVisible());
                qInfo().noquote() << QStringLiteral("macOS 原生标题栏双栏开关已安装");
            } else {
                QTimer::singleShot(100, this, [this] {
                    const bool installed = installMacTitleBarControls(this,
                        [this] { setSidebarVisible(m_sidebar->isHidden()); },
                        [this] { setMonitorVisible(m_monitorRail->isHidden()); },
                        [this] { showTerminalSettings(); });
                    if (installed) {
                        updateMacTitleBarControls(this, m_sidebar && m_sidebar->isVisible(),
                            m_monitorRail && m_monitorRail->isVisible());
                        qInfo().noquote() << QStringLiteral("macOS 原生标题栏双栏开关已安装");
                    }
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
    connect(m_sidebar, &HostSidebar::addServerInGroupRequested, this, &MainWindow::addServerInGroup);
    connect(m_sidebar, &HostSidebar::serverGroupChanged, this, [this](const ServerProfile &updated) {
        auto profile = updated;
        if (!m_repository->saveServer(profile)) {
            QMessageBox::critical(this, QStringLiteral("移动主机失败"), m_repository->lastError());
            for (const auto &saved : m_repository->loadServers()) {
                if (saved.id == profile.id) {
                    m_sidebar->updateServer(saved);
                    break;
                }
            }
            return;
        }
        if (m_terminalWorkspace) m_terminalWorkspace->updateServer(profile);
    });
    connect(m_sidebar, &HostSidebar::groupCreateRequested, this, [this](const QString &name) {
        if (!m_repository->saveServerGroup(name)) {
            QMessageBox::critical(this, QStringLiteral("新建分组失败"), m_repository->lastError());
            return;
        }
        m_sidebar->addGroup(name);
    });
    connect(m_sidebar, &HostSidebar::groupRenameRequested, this,
        [this](const QString &oldName, const QString &newName) {
            if (!m_repository->renameServerGroup(oldName, newName)) {
                QMessageBox::critical(this, QStringLiteral("重命名分组失败"), m_repository->lastError());
                return;
            }
            m_sidebar->renameGroup(oldName, newName);
            if (m_terminalWorkspace) {
                for (const auto &server : m_sidebar->servers()) m_terminalWorkspace->updateServer(server);
            }
        });
    connect(m_sidebar, &HostSidebar::groupDeleteRequested, this, [this](const QString &name) {
        if (!m_repository->deleteServerGroup(name)) {
            QMessageBox::critical(this, QStringLiteral("删除分组失败"), m_repository->lastError());
            return;
        }
        m_sidebar->removeGroup(name);
        if (m_terminalWorkspace) {
            for (const auto &server : m_sidebar->servers()) m_terminalWorkspace->updateServer(server);
        }
    });
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
    auto *toolbar = new WindowControlsToolbar(this);
    toolbar->setObjectName(QStringLiteral("windowControlsToolbar"));
    toolbar->setAllowedAreas(Qt::TopToolBarArea);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setIconSize(QSize(20, 20));
    toolbar->setFixedHeight(34);

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

    m_settingsButton = new QToolButton;
    m_settingsButton->setObjectName(QStringLiteral("terminalSettingsButton"));
    m_settingsButton->setIcon(QIcon(QStringLiteral(":/assets/settings.svg")));
    m_settingsButton->setIconSize(QSize(19, 19));
    m_settingsButton->setFixedSize(30, 28);
    m_settingsButton->setToolTip(QStringLiteral("终端显示设置"));
    m_settingsButton->setAccessibleName(QStringLiteral("终端显示设置"));

    toolbar->addWidget(m_sidebarToggleButton);
    toolbar->addWidget(m_monitorToggleButton);
    toolbar->addWidget(m_settingsButton);

    auto *leftSpacer = new QWidget;
    leftSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(leftSpacer);
    auto *title = new QLabel(QStringLiteral("玄壳 · SSH 远程管理"));
    title->setObjectName(QStringLiteral("windowToolbarTitle"));
    title->setAlignment(Qt::AlignCenter);
    toolbar->addWidget(title);
    auto *rightSpacer = new QWidget;
    rightSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(rightSpacer);

#ifdef Q_OS_WIN
    const auto systemButton = [toolbar](const QString &text, const QString &name, const QString &toolTip) {
        auto *button = new QToolButton;
        button->setObjectName(name);
        button->setText(text);
        button->setToolTip(toolTip);
        button->setAccessibleName(toolTip);
        button->setFixedSize(44, 32);
        toolbar->addWidget(button);
        return button;
    };
    auto *minimizeButton = systemButton(QStringLiteral("—"),
        QStringLiteral("windowMinimizeButton"), QStringLiteral("最小化"));
    m_maximizeWindowButton = systemButton(QStringLiteral("□"),
        QStringLiteral("windowMaximizeButton"), QStringLiteral("最大化"));
    auto *closeButton = systemButton(QStringLiteral("×"),
        QStringLiteral("windowCloseButton"), QStringLiteral("关闭"));
    connect(minimizeButton, &QToolButton::clicked, this, &QWidget::showMinimized);
    connect(m_maximizeWindowButton, &QToolButton::clicked, this, [this] {
        isMaximized() ? showNormal() : showMaximized();
    });
    connect(closeButton, &QToolButton::clicked, this, &QWidget::close);
#endif
    connect(m_settingsButton, &QToolButton::clicked, this, &MainWindow::showTerminalSettings);
    return toolbar;
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() != QEvent::WindowStateChange || !m_maximizeWindowButton) return;
    const bool maximized = isMaximized();
    m_maximizeWindowButton->setText(maximized ? QStringLiteral("❐") : QStringLiteral("□"));
    m_maximizeWindowButton->setToolTip(maximized ? QStringLiteral("还原") : QStringLiteral("最大化"));
    m_maximizeWindowButton->setAccessibleName(m_maximizeWindowButton->toolTip());
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    Q_UNUSED(eventType)
    const auto *nativeMessage = static_cast<MSG *>(message);
    if (nativeMessage && nativeMessage->message == WM_NCHITTEST && !isMaximized()) {
        const auto local = mapFromGlobal(QPoint(
            GET_X_LPARAM(nativeMessage->lParam), GET_Y_LPARAM(nativeMessage->lParam)));
        constexpr int border = 6;
        const bool left = local.x() >= 0 && local.x() < border;
        const bool right = local.x() < width() && local.x() >= width() - border;
        const bool top = local.y() >= 0 && local.y() < border;
        const bool bottom = local.y() < height() && local.y() >= height() - border;
        if (top && left) *result = HTTOPLEFT;
        else if (top && right) *result = HTTOPRIGHT;
        else if (bottom && left) *result = HTBOTTOMLEFT;
        else if (bottom && right) *result = HTBOTTOMRIGHT;
        else if (left) *result = HTLEFT;
        else if (right) *result = HTRIGHT;
        else if (top) *result = HTTOP;
        else if (bottom) *result = HTBOTTOM;
        else return QMainWindow::nativeEvent(eventType, message, result);
        return true;
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::showTerminalSettings()
{
    const auto previous = TerminalView::defaultAppearance();
    TerminalSettingsDialog dialog(previous, this);
    const auto applyAppearance = [this](const TerminalAppearance &appearance) {
        TerminalView::setDefaultAppearance(appearance);
        const auto terminals = findChildren<TerminalView *>();
        for (auto *terminal : terminals) terminal->setAppearance(appearance);
    };
    connect(&dialog, &TerminalSettingsDialog::appearancePreviewRequested, this,
        [applyAppearance](const QString &family, int size, double spacing) {
            applyAppearance({family, size, spacing});
        });

    if (dialog.exec() == QDialog::Accepted) {
        const auto selected = dialog.appearance();
        applyAppearance(selected);
        QSettings settings;
        settings.setValue(QStringLiteral("terminal/fontFamily"), selected.fontFamily);
        settings.setValue(QStringLiteral("terminal/fontSize"), selected.pointSize);
        settings.setValue(QStringLiteral("terminal/lineSpacing"), selected.lineSpacing);
    } else {
        applyAppearance(previous);
    }
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

void MainWindow::setFileWorkspaceVisible(bool visible)
{
    m_fileWorkspaceVisible = visible;
    if (m_fileWorkspacePane) m_fileWorkspacePane->setVisible(visible);
    if (visible && m_terminalFileSplitter) {
        const int available = qMax(400, m_terminalFileSplitter->height());
        m_terminalFileSplitter->setSizes({available * 65 / 100, available * 35 / 100});
    }
    if (m_terminalWorkspace) m_terminalWorkspace->setFileWorkspaceVisible(visible);
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
    m_terminalFileSplitter = terminalFileSplitter;
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

    auto *scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("monitorScrollArea"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 7, 10, 10);
    layout->setSpacing(8);

    m_cpuCard = new MetricCard(QStringLiteral("CPU"), QColor(QStringLiteral("#006EFF")));
    m_memoryCard = new MetricCard(QStringLiteral("内存"), QColor(QStringLiteral("#8B5CF6")));
    m_loadCard = new MetricCard(QStringLiteral("负载"), QColor(QStringLiteral("#00A870")));
    auto *summary = new QFrame;
    summary->setObjectName(QStringLiteral("monitorMetricSummary"));
    summary->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *summaryLayout = new QVBoxLayout(summary);
    summaryLayout->setContentsMargins(0, 0, 0, 0);
    summaryLayout->setSpacing(0);

    auto *systemInfo = new QFrame;
    systemInfo->setObjectName(QStringLiteral("monitorSystemSummary"));
    auto *systemInfoLayout = new QVBoxLayout(systemInfo);
    systemInfoLayout->setContentsMargins(11, 7, 10, 7);
    systemInfoLayout->setSpacing(4);
    auto *systemTitle = new QLabel(QStringLiteral("系统信息"));
    systemTitle->setObjectName(QStringLiteral("monitorSystemTitle"));
    auto *uptimeRow = new QHBoxLayout;
    uptimeRow->setContentsMargins(0, 0, 0, 0);
    auto *uptimeTitle = new QLabel(QStringLiteral("运行时长"));
    uptimeTitle->setObjectName(QStringLiteral("monitorSystemCaption"));
    m_uptimeValue = new QLabel(QStringLiteral("--"));
    m_uptimeValue->setObjectName(QStringLiteral("monitorUptimeValue"));
    uptimeRow->addWidget(uptimeTitle);
    uptimeRow->addStretch();
    uptimeRow->addWidget(m_uptimeValue);
    systemInfoLayout->addWidget(systemTitle);
    systemInfoLayout->addLayout(uptimeRow);
    summaryLayout->addWidget(systemInfo);

    for (auto *card : {m_cpuCard, m_memoryCard, m_loadCard}) {
        summaryLayout->addWidget(card);
    }
    m_loadCard->setProperty("lastRow", true);
    layout->addWidget(summary);

    auto *monitorDetails = createMonitorWorkspace();
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
    connect(m_terminalWorkspace, &TerminalWorkspace::fileWorkspaceToggleRequested, this, [this] {
        setFileWorkspaceVisible(!m_fileWorkspaceVisible);
    });
    connect(m_terminalWorkspace, &TerminalWorkspace::hostSidebarVisibilityRequested,
        this, &MainWindow::setSidebarVisible);
    return container;
}

QWidget *MainWindow::createMonitorWorkspace()
{
    auto *widget = new QWidget;
    widget->setObjectName(QStringLiteral("monitorDetails"));
    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_systemDetailPanel = new SystemDetailPanel;
    layout->addWidget(m_systemDetailPanel);
    return widget;
}

QWidget *MainWindow::createFileWorkspace()
{
    auto *widget = new QWidget;
    m_fileWorkspacePane = widget;
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
    resetMetrics(QStringLiteral("等待连接 · 双击主机或右键连接"));
    m_serverMeta->setText(profile.host);
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
    addServerInGroup(QStringLiteral("我的主机"));
}

void MainWindow::addServerInGroup(const QString &group)
{
    ServerDialog dialog(this);
    dialog.setAvailableGroups(m_repository->loadServerGroups());
    dialog.setInitialGroup(group);
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
    dialog.setAvailableGroups(m_repository->loadServerGroups());
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
    if (m_systemDetailPanel) m_systemDetailPanel->setSample(sample);
    if (m_uptimeValue)
        m_uptimeValue->setText(sample.uptimeSeconds > 0 ? formatUptime(sample.uptimeSeconds) : QStringLiteral("--"));
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

    if (m_sampleStatus) {
        m_sampleStatus->setText(QStringLiteral("采样 %1    周期 1.0 s")
                                    .arg(sample.capturedAt.toString(QStringLiteral("HH:mm:ss"))));
    }
}

void MainWindow::resetMetrics(const QString &detail)
{
    if (!m_cpuCard) return;
    for (auto *card : {m_cpuCard, m_memoryCard, m_loadCard}) {
        card->setValue(QStringLiteral("--"), detail, 0);
    }
    if (m_uptimeValue) m_uptimeValue->setText(QStringLiteral("--"));
    if (m_systemDetailPanel) m_systemDetailPanel->reset(detail);
    if (m_sampleStatus) m_sampleStatus->setText(detail + QStringLiteral("    周期 1.0 s"));
}

} // namespace noxshell::ui
