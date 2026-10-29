#include "MainWindow.h"
#include "SettingsPage.h"
#include "../core/AppSettings.h"
#include <QShortcut>

#include <QPointer>

#include "../core/SshSession.h"
#include "../core/CredentialStore.h"
#include "../core/ServerRepository.h"
#include "../core/RdpLauncher.h"
#include "HostSidebar.h"
#include "MetricCard.h"
#ifdef Q_OS_MACOS
#include "MacTitleBarControls.h"
#endif
#include "ServerDialog.h"
#include "RdpDialog.h"
#include "SystemDetailPanel.h"
#include "TerminalSettingsDialog.h"
#include "TerminalView.h"
#include "TerminalWorkspace.h"

#include <QApplication>
#include <QActionGroup>
#include <QClipboard>
#include <QCloseEvent>
#include <QDebug>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyleHints>
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
QStringList monitorEndpoint(const ServerProfile &profile)
{
    // Cache only non-secret routing identity, never passwords or key material.
    return {profile.id,profile.host,QString::number(profile.port),profile.user,
        QString::number(static_cast<int>(profile.connectionMode))};
}

class HoverMenuToolButton final : public QToolButton {
public:
    using QToolButton::QToolButton;

    void setupHoverMenu(QMenu *menu)
    {
        setMenu(menu);
        // A hover menu must leave the button clickable, so show it without
        // the mouse grab that QMenu::popup / QToolButton::showMenu normally use.
        menu->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
        menu->setAttribute(Qt::WA_ShowWithoutActivating);
        menu->installEventFilter(this);
        connect(menu, &QMenu::triggered, menu, &QWidget::hide);
        connect(this, &QToolButton::clicked, menu, &QWidget::hide);
    }

protected:
    void enterEvent(QEnterEvent *event) override
    {
        QToolButton::enterEvent(event);
        QTimer::singleShot(200, this, [this] {
            if (underMouse() && !isDown() && menu() && !menu()->isVisible()) {
                menu()->move(mapToGlobal(QPoint(0, height())));
                menu()->show();
            }
        });
    }

    void leaveEvent(QEvent *event) override
    {
        QToolButton::leaveEvent(event);
        scheduleHide();
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == menu() && event->type() == QEvent::Leave) scheduleHide();
        return QToolButton::eventFilter(watched, event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (menu()) menu()->hide();
        QToolButton::mousePressEvent(event);
    }

private:
    void scheduleHide()
    {
        QTimer::singleShot(250, this, [this] {
            if (menu() && !underMouse() && !menu()->underMouse()) menu()->hide();
        });
    }
};

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

MainWindow::MainWindow(QString databasePath, QWidget *parent, CredentialStore *credentialStore)
    : QMainWindow(parent)
    , m_repository(new ServerRepository(std::move(databasePath), true, this))
    , m_credentialStore(credentialStore ? credentialStore : new CredentialStore(this))
{
    m_themeMode = storedThemeMode();
    applyApplicationTheme(m_themeMode);
    setWindowTitle(QStringLiteral("玄壳 · 远程连接管理"));
#ifdef Q_OS_WIN
    // Windows 使用应用内标题栏，把三枚工作区工具按钮与最小化、最大化、
    // 关闭按钮合并为一行；macOS 继续使用原生标题栏附件。
    setWindowFlag(Qt::FramelessWindowHint, true);
#endif
    setMinimumSize(1180, 720);
    resize(1440, 900);
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive || !m_hiddenByClose || m_quitInProgress) return;
        m_hiddenByClose = false;
        setWindowState(windowState() & ~Qt::WindowMinimized);
        show();
        raise();
        activateWindow();
    });

    auto terminalAppearance = TerminalView::defaultAppearance();
    const QSettings settings;
    terminalAppearance.fontFamily = settings.value(
        QStringLiteral("terminal/fontFamily"), terminalAppearance.fontFamily).toString();
    terminalAppearance.pointSize = settings.value(
        QStringLiteral("terminal/fontSize"), terminalAppearance.pointSize).toInt();
    terminalAppearance.lineSpacing = settings.value(
        QStringLiteral("terminal/lineSpacing"), terminalAppearance.lineSpacing).toDouble();
    terminalAppearance.autoEnglishInput = settings.value(
        QStringLiteral("terminal/autoEnglishInput"), true).toBool();
    TerminalView::setDefaultAppearance(terminalAppearance);
    auto *settingsShortcut = new QShortcut(this); settingsShortcut->setContext(Qt::WindowShortcut);
    const auto updateSettingsKey = [settingsShortcut] {settingsShortcut->setKey(QKeySequence(QSettings().value("shortcuts/settings","Ctrl+,").toString()));};
    connect(settingsShortcut,&QShortcut::activated,this,&MainWindow::showTerminalSettings);
    connect(AppSettings::instance(),&AppSettings::changed,this,[this,updateSettingsKey](const QString &key){
        updateSettingsKey();
        if(key=="ui/themeMode")setThemeMode(themeModeFromSetting(AppSettings::value(key).toString()));
        if (key.isEmpty() || key.startsWith("terminal/")) {
            auto appearance = TerminalView::defaultAppearance();
            appearance.fontFamily=AppSettings::value("terminal/fontFamily").toString();
            appearance.pointSize=AppSettings::value("terminal/fontSize").toInt();
            appearance.lineSpacing=AppSettings::value("terminal/lineSpacing").toDouble();
            appearance.autoEnglishInput=AppSettings::value("terminal/autoEnglishInput").toBool();
            TerminalView::setDefaultAppearance(appearance);
            for(auto *terminal:findChildren<TerminalView*>())terminal->setAppearance(appearance);
        }
    }); updateSettingsKey();
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
    bodyLayout->addWidget(createWorkspace(), 1);
    rootLayout->addWidget(body, 1);
    rootLayout->addWidget(createStatusBar());
    setCentralWidget(root);
    setMonitorVisible(false);
    if (m_titlebarTabsHost && m_terminalWorkspace) {
        auto *tabs = m_terminalWorkspace->tabToolbar();
        tabs->setParent(m_titlebarTabsHost);
        m_titlebarTabsHost->layout()->addWidget(tabs);
    }

    if (m_monitorToggleButton) {
        connect(m_monitorToggleButton, &QToolButton::clicked, this, [this] {
            setMonitorVisible(m_monitorRail->isHidden());
        });
    }
#ifdef Q_OS_MACOS
    if (m_nativeTitleBarControls) {
        m_terminalWorkspace->setNativeTitlebarTabs(true);
        connect(m_terminalWorkspace, &TerminalWorkspace::tabsChanged, this, [this] {
            updateMacTitleBarSessionTabs(this, m_terminalWorkspace->sessionTabs());
        });
        QTimer::singleShot(0, this, [this] {
            const auto install = [this] {
                return installMacTitleBarControls(this,
                    [this] { addServer(); },
                    [this] { showCredentialManager(); },
                    [this] { showServerManager(); },
                    [this] { setMonitorVisible(m_monitorRail->isHidden()); },
                    [this] { showTerminalSettings(); },
                    [this](int mode) { setThemeMode(static_cast<ThemeMode>(mode)); },
                    static_cast<int>(m_themeMode));
            };
            if (install()) {
                updateMacTitleBarSessionTabs(this, m_terminalWorkspace->sessionTabs());
                updateMacTitleBarControls(this, m_monitorRail && m_monitorRail->isVisible(), static_cast<int>(m_themeMode));
                qInfo().noquote() << QStringLiteral("macOS 原生标题栏控件已安装");
            } else {
                QTimer::singleShot(100, this, [this] {
                    const bool installed = installMacTitleBarControls(this,
                        [this] { addServer(); },
                        [this] { showCredentialManager(); },
                        [this] { showServerManager(); },
                        [this] { setMonitorVisible(m_monitorRail->isHidden()); },
                        [this] { showTerminalSettings(); },
                        [this](int mode) { setThemeMode(static_cast<ThemeMode>(mode)); },
                        static_cast<int>(m_themeMode));
                    if (installed) {
                        updateMacTitleBarSessionTabs(this, m_terminalWorkspace->sessionTabs());
                        updateMacTitleBarControls(this, m_monitorRail && m_monitorRail->isVisible(), static_cast<int>(m_themeMode));
                        qInfo().noquote() << QStringLiteral("macOS 原生标题栏控件已安装");
                    }
                    else qWarning().noquote() << QStringLiteral("macOS 原生标题栏控件安装失败");
                });
            }
        });
    }
#endif

    connect(m_sidebar, &HostSidebar::serverConnectRequested, this, &MainWindow::connectToServer);
    connect(m_sidebar, &HostSidebar::serverEditRequested, this, &MainWindow::selectAndEditServer);
    connect(m_sidebar, &HostSidebar::serverDuplicateRequested, this, &MainWindow::duplicateServer);
    connect(m_sidebar, &HostSidebar::serverDeleteRequested, this, &MainWindow::selectAndDeleteServer);
    connect(m_sidebar, &HostSidebar::addServerRequested, this, &MainWindow::addServer);
    connect(m_sidebar, &HostSidebar::addServerInGroupRequested, this, &MainWindow::addServerInGroup);
    connect(m_sidebar, &HostSidebar::addRdpServerRequested, this, &MainWindow::addRdpServer);
    connect(m_sidebar, &HostSidebar::addRdpServerInGroupRequested, this, &MainWindow::addRdpServerInGroup);
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
    m_metricTimer->setTimerType(Qt::PreciseTimer);
    m_metricTimer->setInterval(1000);
    connect(m_metricTimer, &QTimer::timeout, this, &MainWindow::requestMetrics);
    m_metricTimer->start();

    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
        [this](Qt::ColorScheme) {
            if (m_themeMode != ThemeMode::System) return;
            applyApplicationTheme(m_themeMode);
            updateThemePresentation();
        });

    QTimer::singleShot(0, this, [this] {
        m_sidebar->selectFirstServer();
    });
}

MainWindow::~MainWindow()
{
    // QWidget deletes child pages after derived members have been destroyed.
    // Disconnect their cache-cleanup callbacks while the maps are still alive.
    for(const auto &connection:m_monitorCleanupConnections)QObject::disconnect(connection);
}

QToolBar *MainWindow::createWindowToolbar()
{
    auto *toolbar = new WindowControlsToolbar(this);
    toolbar->setObjectName(QStringLiteral("windowControlsToolbar"));
    toolbar->setAllowedAreas(Qt::TopToolBarArea);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setIconSize(QSize(20, 20));
    toolbar->setFixedHeight(36);

    m_newConnectionButton = new HoverMenuToolButton;
    m_newConnectionButton->setObjectName(QStringLiteral("newConnectionButton"));
    m_newConnectionButton->setIcon(QIcon(QStringLiteral(":/assets/add.svg")));
    m_newConnectionButton->setIconSize(QSize(18, 18));
    m_newConnectionButton->setFixedSize(30, 28);
    m_newConnectionButton->setToolTip(QStringLiteral("新建连接；停留以打开管理菜单"));
    m_newConnectionButton->setAccessibleName(QStringLiteral("新建连接"));
    m_newConnectionButton->setPopupMode(QToolButton::DelayedPopup);
    auto *connectionMenu = new QMenu(m_newConnectionButton);
    connectionMenu->setObjectName(QStringLiteral("connectionManagementMenu"));
    auto *credentialAction = connectionMenu->addAction(QStringLiteral("凭据管理"));
    credentialAction->setObjectName(QStringLiteral("credentialManagerAction"));
    auto *serverAction = connectionMenu->addAction(QStringLiteral("服务器管理"));
    serverAction->setObjectName(QStringLiteral("serverManagerAction"));
    static_cast<HoverMenuToolButton *>(m_newConnectionButton)->setupHoverMenu(connectionMenu);
    connect(m_newConnectionButton, &QToolButton::clicked, this, &MainWindow::addServer);
    connect(credentialAction, &QAction::triggered, this, &MainWindow::showCredentialManager);
    connect(serverAction, &QAction::triggered, this, &MainWindow::showServerManager);

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
    m_settingsButton->setToolTip(QStringLiteral("终端设置：外观与输入法"));
    m_settingsButton->setAccessibleName(QStringLiteral("终端设置"));

    m_themeModeButton = new QToolButton;
    m_themeModeButton->setObjectName(QStringLiteral("themeModeButton"));
    m_themeModeButton->setFixedSize(30, 28);
    m_themeModeButton->setPopupMode(QToolButton::InstantPopup);
    auto *themeMenu = new QMenu(m_themeModeButton);
    themeMenu->setObjectName(QStringLiteral("themeModeMenu"));
    auto *themeGroup = new QActionGroup(themeMenu);
    themeGroup->setExclusive(true);
    const auto addThemeAction = [themeMenu, themeGroup](const QString &text, const QString &name, ThemeMode mode) {
        auto *action = themeMenu->addAction(text);
        action->setObjectName(name);
        action->setCheckable(true);
        action->setData(static_cast<int>(mode));
        themeGroup->addAction(action);
        return action;
    };
    m_systemThemeAction = addThemeAction(QStringLiteral("跟随系统"),
        QStringLiteral("themeSystemAction"), ThemeMode::System);
    m_lightThemeAction = addThemeAction(QStringLiteral("亮色模式"),
        QStringLiteral("themeLightAction"), ThemeMode::Light);
    m_darkThemeAction = addThemeAction(QStringLiteral("暗色模式"),
        QStringLiteral("themeDarkAction"), ThemeMode::Dark);
    m_themeModeButton->setMenu(themeMenu);
    connect(themeGroup, &QActionGroup::triggered, this, [this](QAction *action) {
        setThemeMode(static_cast<ThemeMode>(action->data().toInt()));
    });

    toolbar->addWidget(m_newConnectionButton);
    toolbar->addWidget(m_monitorToggleButton);
    toolbar->addWidget(m_settingsButton);

    m_titlebarTabsHost = new QWidget;
    m_titlebarTabsHost->setObjectName(QStringLiteral("titlebarTabsHost"));
    m_titlebarTabsHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *tabsLayout = new QHBoxLayout(m_titlebarTabsHost);
    tabsLayout->setContentsMargins(0, 0, 0, 0);
    tabsLayout->setSpacing(0);
    toolbar->addWidget(m_titlebarTabsHost);
    toolbar->addWidget(m_themeModeButton);

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
    updateThemePresentation();
    return toolbar;
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (m_metricTimer && (event->type() == QEvent::ActivationChange || event->type() == QEvent::WindowStateChange)) {
        requestMetrics();
    }
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
    auto *page = new SettingsPage(m_credentialStore);
    connect(page, &SettingsPage::themeModeRequested, this, [this](const QString &mode) {setThemeMode(themeModeFromSetting(mode));});
    m_terminalWorkspace->showSettings(page);
}

void MainWindow::showCredentialManager()
{
    if (m_terminalWorkspace) m_terminalWorkspace->showCredentialManager();
}

void MainWindow::showServerManager()
{
    if (m_terminalWorkspace) m_terminalWorkspace->showServerManager();
}

void MainWindow::setThemeMode(ThemeMode mode, bool persist)
{
    m_themeMode = mode;
    if (persist) {
        QSettings settings;
        settings.setValue(QStringLiteral("ui/themeMode"), themeModeSettingValue(mode));
    }
    applyApplicationTheme(mode);
    updateThemePresentation();
}

void MainWindow::updateThemePresentation()
{
    const bool dark = isDarkTheme(m_themeMode);
    const QString modeName = m_themeMode == ThemeMode::System
        ? QStringLiteral("跟随系统")
        : m_themeMode == ThemeMode::Light ? QStringLiteral("亮色模式") : QStringLiteral("暗色模式");
    if (m_themeModeButton) {
        m_themeModeButton->setText(m_themeMode == ThemeMode::System
                ? QStringLiteral("◐") : dark ? QStringLiteral("☾") : QStringLiteral("☀"));
        m_themeModeButton->setToolTip(QStringLiteral("界面外观：%1").arg(modeName));
        m_themeModeButton->setAccessibleName(m_themeModeButton->toolTip());
    }
    if (m_systemThemeAction) m_systemThemeAction->setChecked(m_themeMode == ThemeMode::System);
    if (m_lightThemeAction) m_lightThemeAction->setChecked(m_themeMode == ThemeMode::Light);
    if (m_darkThemeAction) m_darkThemeAction->setChecked(m_themeMode == ThemeMode::Dark);
    if (m_onlineBadge && m_onlineBadge->property("badgeForeground").isValid()) {
        setConnectionBadge(m_onlineBadge->text(),
            m_onlineBadge->property("badgeForeground").toString(),
            m_onlineBadge->property("badgeBackground").toString());
    }
#ifdef Q_OS_MACOS
    if (m_nativeTitleBarControls) {
        updateMacTitleBarControls(this, m_monitorRail && m_monitorRail->isVisible(), static_cast<int>(m_themeMode));
    }
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
    if (m_nativeTitleBarControls) updateMacTitleBarControls(this, visible, static_cast<int>(m_themeMode));
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

    operationsLayout->addWidget(createTerminalWorkspace(), 1);

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
    identityLayout->setContentsMargins(10, 10, 10, 9);
    identityLayout->setSpacing(5);
    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(2);
    m_serverMeta = new QLabel(QStringLiteral("未选择主机"));
    m_serverMeta->setObjectName(QStringLiteral("serverAddress"));
    auto *copyAddress = new QToolButton;
    copyAddress->setObjectName(QStringLiteral("copyHostAddressButton"));
    copyAddress->setIcon(QIcon(QStringLiteral(":/assets/copy.svg")));
    copyAddress->setIconSize(QSize(15, 15));
    copyAddress->setFixedSize(20, 27);
    copyAddress->setToolTip(QStringLiteral("复制 IP 地址"));
    copyAddress->setAccessibleName(QStringLiteral("复制 IP 地址"));
    m_onlineBadge = new QLabel(QStringLiteral("○ 待连接"));
    m_onlineBadge->setObjectName(QStringLiteral("onlineBadge"));
    titleRow->addWidget(m_serverMeta);
    titleRow->addWidget(copyAddress);
    titleRow->addStretch();
    m_serverLatency = new QLabel;
    m_serverLatency->setObjectName(QStringLiteral("serverLatency"));
    m_serverLatency->setAccessibleName(QStringLiteral("服务器 TCP 延迟"));
    m_serverLatency->setToolTip(QStringLiteral("当前顶部标签的 TCP 连接探测耗时，包含代理协商，并非 ICMP 延迟。\n每个标签共用一个任务，仅当前标签每 10 秒检测一次。"));
    m_serverLatency->hide();
    titleRow->addWidget(m_serverLatency);
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
    content->setObjectName(QStringLiteral("monitorScrollContent"));
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 7, 10, 10);
    layout->setSpacing(10);

    m_cpuCard = new MetricCard(QStringLiteral("CPU"), QColor(QStringLiteral("#006EFF")));
    m_memoryCard = new MetricCard(QStringLiteral("内存"), QColor(QStringLiteral("#8B5CF6")));
    m_loadCard = new MetricCard(QStringLiteral("负载"), QColor(QStringLiteral("#00A870")));
    auto *summary = new QFrame;
    summary->setObjectName(QStringLiteral("monitorMetricSummary"));
    // Core details use a floating panel. Summary rows keep their fixed heights
    // so hovering cannot change the monitor scroll range or move other cards.
    summary->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    auto *summaryLayout = new QVBoxLayout(summary);
    summaryLayout->setContentsMargins(0, 0, 0, 0);
    summaryLayout->setSpacing(0);
    summaryLayout->setSizeConstraint(QLayout::SetMinimumSize);

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
    m_terminalWorkspace->setServerManager(m_sidebar);
    connect(m_terminalWorkspace, &TerminalWorkspace::savedCredentialsChanged, this, [this] {
        QHash<QString, ServerProfile> runtime;
        for (const auto &profile : m_sidebar->servers()) runtime.insert(profile.id, profile);
        auto saved = m_repository->loadServers();
        bool changed = false;
        for (auto &profile : saved) {
            const auto previous = runtime.constFind(profile.id);
            if (previous == runtime.cend()) continue;
            profile.state = previous->state;
            if (previous->user == profile.user && previous->credentialRef == profile.credentialRef) continue;
            changed = true;
            m_terminalWorkspace->updateServer(profile);
        }
        if (changed) m_sidebar->refreshServers(saved);
    });
    layout->addWidget(m_terminalWorkspace);
    connect(m_terminalWorkspace, &TerminalWorkspace::monitorSessionChanged,
        this, &MainWindow::activateMonitorSession);
    connect(m_terminalWorkspace, &TerminalWorkspace::latencyChanged, this,
        [this](QWidget *page, const QString &text) {
            if (page && page != m_monitorPage) return;
            m_serverLatency->setText(text);
            m_serverLatency->setVisible(!text.isEmpty());
        });
    connect(m_terminalWorkspace, &TerminalWorkspace::sessionClosed,
        this, &MainWindow::releaseSession);
    connect(m_terminalWorkspace, &TerminalWorkspace::sessionCountChanged, this, [this](int count) {
        if (count != 0) return;
        // Closing the final tab has no replacement monitor source to publish.
        // Drop its snapshot as well as its channel before returning home, so
        // delayed metrics cannot repopulate the now-hidden rail.
        m_session.clear();
        m_monitorPage.clear();
        m_currentServer = {};
        m_hasMetricSample = false;
        m_monitorStates.clear();
        m_monitorCacheOrder.clear();
        resetMetrics(QStringLiteral("没有打开的 SSH 会话"));
        m_serverMeta->clear();
        m_serverMeta->setToolTip({});
        m_serverLatency->clear();
        m_serverLatency->hide();
        setConnectionBadge(QStringLiteral("○ 离线"), QStringLiteral("#738297"), QStringLiteral("#EEF2F6"));
        if (m_sampleStatus) m_sampleStatus->setText(QStringLiteral("采样等待中    传输任务 0"));
        setMonitorVisible(false);
    });
    connect(m_terminalWorkspace, &TerminalWorkspace::sessionConnectionChanged,
        this, [this](const QString &serverId, bool connected, const QString &message) {
            updateConnectionPresentation(serverId, connected, message);
        });
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
    m_rdpCredentialButton = new QPushButton;
    m_rdpCredentialButton->setObjectName(QStringLiteral("rdpEnterPasswordButton"));
    m_rdpCredentialButton->hide();
    layout->addWidget(m_rdpCredentialButton);
    connect(m_rdpCredentialButton, &QPushButton::clicked, this, [this] {
        if (m_pendingCredentialProfile.id.isEmpty()) return;
        const auto profile = m_pendingCredentialProfile;
        // Reuse the existing remote-password editor, never a system unlock UI.
        editServer(profile);
    });
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
    if (profile.connectionMode == ConnectionMode::Rdp) {
        m_session = nullptr;
        resetMetrics(QStringLiteral("Windows 远程桌面由系统客户端运行"));
        setConnectionBadge(QStringLiteral("RDP 远程桌面"), QStringLiteral("#6F42C1"), QStringLiteral("#F1EAFE"));
        return;
    }
    const bool connectedNow = m_terminalWorkspace && m_terminalWorkspace->hasConnectedSession(profile.id);
    if (connectedNow) {
        m_currentServer.state = ServerState::Online;
        m_sidebar->setServerState(profile.id, ServerState::Online);
        setConnectionBadge(QStringLiteral("● 在线"), QStringLiteral("#008858"), QStringLiteral("#E8F8F2"));
    } else if (m_terminalWorkspace && m_terminalWorkspace->hasConnectingSession(profile.id)) {
        setConnectionBadge(QStringLiteral("◌ 连接中"), QStringLiteral("#006EFF"), QStringLiteral("#E8F3FF"));
    } else {
        m_currentServer.state = ServerState::Offline;
        m_sidebar->setServerState(profile.id, ServerState::Offline);
        setConnectionBadge(QStringLiteral("○ 离线"), QStringLiteral("#738297"), QStringLiteral("#EEF2F6"));
    }
}

void MainWindow::cacheMonitorState()
{
    if(m_monitorPage && m_hasMetricSample && m_systemDetailPanel)
        m_monitorStates[m_monitorPage]={monitorEndpoint(m_currentServer),m_systemDetailPanel->viewState()};
}

void MainWindow::activateMonitorSession(QWidget *tabPage, const ServerProfile &profile, SshSession *session)
{
    if (profile.id.isEmpty() || !session || !tabPage) return;
    if(m_monitorPage==tabPage && m_session==session){
        m_currentServer=profile;
        m_serverMeta->setText(profile.host);
        updateConnectionPresentation(profile.id,session->isConnected(),session->isConnecting()?QStringLiteral("正在连接"):QString{});
        return;
    }
    cacheMonitorState();
    if(!m_monitorCleanupConnections.contains(tabPage)){
        m_monitorCleanupConnections.insert(tabPage,connect(tabPage,&QObject::destroyed,this,[this,tabPage]{
            m_monitorStates.remove(tabPage);m_monitorCacheOrder.removeAll(tabPage);
            m_monitorCleanupConnections.remove(tabPage);
        }));
    }
    m_monitorCacheOrder.removeAll(tabPage);m_monitorCacheOrder.append(tabPage);
    while(m_monitorCacheOrder.size()>32)m_monitorStates.remove(m_monitorCacheOrder.takeFirst());
    m_monitorPage=tabPage;
    if (m_sidebar) m_sidebar->selectServerById(profile.id);
    selectServer(profile);
    bindSession(session);
    m_session = session;
    m_serverMeta->setToolTip(QStringLiteral("当前顶部标签的固定监控目标：%1@%2:%3\n切换内部 Shell / Agent 或分屏不会清空图表。").arg(profile.user,profile.host).arg(profile.port));
    updateConnectionPresentation(profile.id, session->isConnected(), session->isConnecting() ? QStringLiteral("正在连接") : QString{});
    const auto state=m_monitorStates.value(tabPage);
    const bool sameEndpoint=state.endpoint==monitorEndpoint(profile);
    if(sameEndpoint && state.view.sample.capturedAt.isValid()){
        m_systemDetailPanel->restoreViewState(state.view);
        displayMetrics(state.view.sample);
    }
    if (session->isConnected()) {
        if (!m_hasMetricSample)if(const auto cached=session->lastMetricSample())displayMetrics(*cached);
        requestMetrics();
    }
}

void MainWindow::bindSession(SshSession *session)
{
    if (!session || m_boundSessions.contains(session)) return;
    m_boundSessions.insert(session);
    connect(session, &SshSession::credentialReferenceChanged, this, [this](const ServerProfile &saved) {
        m_sidebar->updateServer(saved);
        if (m_currentServer.id == saved.id) m_currentServer.credentialRef = saved.credentialRef;
    });
    connect(session, &SshSession::credentialSaveNotice, this, [this](const QString &message) {
        if (m_sampleStatus) m_sampleStatus->setText(message);
    });
    connect(session, &SshSession::metricSampleReceived, this, [this, session](const MetricSample &sample) {
        if (session == m_session) displayMetrics(sample);
    });
    connect(session, &SshSession::metricsCollectionFailed, this, [this, session](const QString &message) {
        if (session != m_session) return;
        if (!m_hasMetricSample) resetMetrics(QStringLiteral("采样失败"));
        if (m_sampleStatus) m_sampleStatus->setText(QStringLiteral("采样失败 · %1").arg(message));
    });
    connect(session, &SshSession::connectionChanged, this, [this, session](bool connected, const QString &) {
        if (!connected || session != m_session) return;
        // The terminal is the primary interactive surface. Give its login
        // banner/prompt one short frame window before starting the first
        // blocking metrics channel on the shared SSH transport.
        QTimer::singleShot(120, this, [this, session] {
            if (session == m_session && session->isConnected()) requestMetrics();
        });
    });
}

void MainWindow::releaseSession(SshSession *session)
{
    if (!session) return;
    m_boundSessions.remove(session);
    disconnect(session, nullptr, this, nullptr);
    if (m_session == session) {
        m_session = nullptr;
    }
}

void MainWindow::connectToServer(const ServerProfile &profile)
{
    if (profile.id.isEmpty() || m_readingRdpCredentials) return;
    m_rdpCredentialButton->hide();
    m_pendingCredentialProfile = {};
    if (profile.connectionMode == ConnectionMode::Rdp) {
        cacheMonitorState();m_monitorPage.clear();
        selectServer(profile);
        auto launchProfile = profile;
        if (launchProfile.password.isEmpty() && !profile.credentialRef.isEmpty()) {
            m_readingRdpCredentials = true;
            const QPointer<MainWindow> lifetime(this);
            const auto secret = m_credentialStore->load(profile.credentialRef);
            if (!lifetime) return;
            m_readingRdpCredentials = false;
            if (!m_credentialStore->lastError().isEmpty()) {
                m_sampleStatus->setText(QStringLiteral("RDP 凭据读取未完成；尚未启动远程桌面客户端"));
                m_pendingCredentialProfile = profile;
                m_pendingCredentialProfile.password.clear();
                m_pendingCredentialProfile.keyPassphrase.clear();
                m_rdpCredentialButton->setText(QStringLiteral("重新填写 %1 的远程桌面密码").arg(profile.name));
                m_rdpCredentialButton->show();
                return;
            }
            launchProfile.password = secret.password;
        }
#ifdef Q_OS_MACOS
        const auto copiedPassword = launchProfile.password;
        if (!copiedPassword.isEmpty()) QApplication::clipboard()->setText(copiedPassword);
#endif
        QString error;
        if (!RdpLauncher::launch(launchProfile, &error)) {
#ifdef Q_OS_MACOS
            if (!copiedPassword.isEmpty() && QApplication::clipboard()->text() == copiedPassword) {
                QApplication::clipboard()->clear();
            }
#endif
            QMessageBox::warning(this, QStringLiteral("无法打开远程桌面"), error);
            setConnectionBadge(QStringLiteral("RDP 启动失败"), QStringLiteral("#B42318"), QStringLiteral("#FEECEB"));
            return;
        }
        setConnectionBadge(QStringLiteral("↗ 已打开 RDP"), QStringLiteral("#6F42C1"), QStringLiteral("#F1EAFE"));
#ifdef Q_OS_MACOS
        if (!copiedPassword.isEmpty()) {
            if (m_sampleStatus) m_sampleStatus->setText(QStringLiteral("Windows App 已打开 · 密码已复制，60 秒后清除"));
            QTimer::singleShot(60000, this, [copiedPassword] {
                if (QApplication::clipboard()->text() == copiedPassword) QApplication::clipboard()->clear();
            });
        } else
#endif
        if (m_sampleStatus) m_sampleStatus->setText(QStringLiteral("远程桌面已交给系统客户端"));
        return;
    }
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
    if (sourceCredentialRef.startsWith(QLatin1String("managed/"))) {
        duplicate.credentialRef = sourceCredentialRef;
        duplicate.password.clear(); duplicate.keyPassphrase.clear();
    } else if (duplicate.connectionMode != ConnectionMode::Rdp
        && duplicate.authentication != AuthenticationMethod::SshAgent && !sourceCredentialRef.isEmpty()) {
        const auto secret = m_credentialStore->load(sourceCredentialRef);
        duplicate.password = secret.password;
        duplicate.keyPassphrase = secret.keyPassphrase;
    }
    if (persistProfile(duplicate, false)) m_sidebar->addServer(duplicate);
}

bool MainWindow::persistProfile(ServerProfile &profile, bool preserveEmptySecret, const ServerProfile *originalProfile)
{
    if (profile.id.isEmpty()) profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (profile.authentication == AuthenticationMethod::Password && profile.password.isEmpty()
        && profile.credentialRef.startsWith(QLatin1String("managed/"))) {
        bool found = false;
        for (const auto &entry : m_repository->loadSavedCredentials()) {
            if (entry.credentialRef != profile.credentialRef) continue;
            profile.user = entry.user;
            found = true;
            break;
        }
        if (!found || !m_repository->saveServer(profile)) {
            QMessageBox::critical(this, QStringLiteral("保存连接失败"),
                found ? m_repository->lastError() : QStringLiteral("所选账密已发生变化，请重新选择。"));
            return false;
        }
        return true;
    }
    if (profile.connectionMode == ConnectionMode::Rdp) {
        CredentialSecret secret;
        if (!profile.password.isEmpty()) secret.password = profile.password;
        if (!secret.password.isEmpty()) {
            profile.credentialRef = QStringLiteral("rdp/%1/%2").arg(profile.id, QUuid::createUuid().toString(QUuid::WithoutBraces));
            if (!m_credentialStore->save(profile.credentialRef, secret)) {
                QMessageBox::critical(this, QStringLiteral("保存 RDP 密码失败"), m_credentialStore->lastError());
                return false;
            }
        }
        profile.password.clear();
        profile.keyPassphrase.clear();
        profile.privateKeyPath.clear();
        profile.publicKeyPath.clear();
        if (!m_repository->saveServer(profile)) {
            QMessageBox::critical(this, QStringLiteral("保存远程桌面失败"), m_repository->lastError());
            return false;
        }
        return true;
    }
    if (profile.credentialRef.isEmpty()) profile.credentialRef = QStringLiteral("server/%1").arg(profile.id);

    CredentialSecret secret;
    const bool sameAuthentication = originalProfile && originalProfile->authentication == profile.authentication;
    const bool samePrivateKey = profile.authentication != AuthenticationMethod::PrivateKey
        || (originalProfile && originalProfile->privateKeyPath == profile.privateKeyPath);
    const bool canPreserveSecret = preserveEmptySecret && originalProfile && sameAuthentication && samePrivateKey
        && profile.credentialRef == originalProfile->credentialRef
        && !originalProfile->credentialRef.isEmpty();
    if (canPreserveSecret && profile.authentication != AuthenticationMethod::SshAgent
        && profile.password.isEmpty() && profile.keyPassphrase.isEmpty()) {
        // Renaming/moving/editing an address does not require decrypting or
        // rewriting an unchanged credential. Preserve the original reference.
        profile.credentialRef = originalProfile->credentialRef;
        if (!m_repository->saveServer(profile)) {
            QMessageBox::critical(this, QStringLiteral("保存服务器失败"), m_repository->lastError());
            return false;
        }
        return true;
    }
    if (!profile.password.isEmpty()) secret.password = profile.password;
    if (!profile.keyPassphrase.isEmpty()) secret.keyPassphrase = profile.keyPassphrase;
    if (profile.authentication == AuthenticationMethod::Password && secret.password.isEmpty()) {
        QMessageBox::critical(this, QStringLiteral("保存凭据失败"), QStringLiteral("未找到可用的 SSH 密码，请重新输入后保存。"));
        return false;
    }
    if (profile.authentication == AuthenticationMethod::SshAgent) {
        profile.credentialRef.clear();
    } else {
        // A new entry belongs to this application identity. Do not update or
        // remove an old entry whose ACL may belong to another build/tool.
        profile.credentialRef = QStringLiteral("server/%1/%2").arg(profile.id, QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (!m_credentialStore->save(profile.credentialRef, secret)) {
            QMessageBox::critical(this, QStringLiteral("保存凭据失败"), m_credentialStore->lastError());
            return false;
        }
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
    addServerInGroup(QString{});
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

void MainWindow::addRdpServer()
{
    addRdpServerInGroup(QString{});
}

void MainWindow::addRdpServerInGroup(const QString &group)
{
    RdpDialog dialog(this);
    dialog.setConnectionRepository(m_repository);
    dialog.setAvailableGroups(m_repository->loadServerGroups());
    dialog.setInitialGroup(group);
    if (dialog.exec() != QDialog::Accepted) return;
    auto profile = dialog.profile();
    if (persistProfile(profile, false)) m_sidebar->addServer(profile);
}

void MainWindow::editServer(const ServerProfile &sourceProfile)
{
    if (sourceProfile.id.isEmpty()) return;
    if (sourceProfile.connectionMode == ConnectionMode::Rdp) {
        RdpDialog dialog(sourceProfile, this);
        dialog.setConnectionRepository(m_repository);
        dialog.setAvailableGroups(m_repository->loadServerGroups());
        if (dialog.exec() != QDialog::Accepted) return;
        auto profile = dialog.profile();
        if (!persistProfile(profile, true, &sourceProfile)) return;
        m_sidebar->updateServer(profile);
        return;
    }
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
    if (!persistProfile(profile, true, &sourceProfile)) return;
    profile.state = runtimeState;
    m_sidebar->updateServer(profile);
    if (m_terminalWorkspace) m_terminalWorkspace->updateServer(profile);
}

void MainWindow::deleteServer(const ServerProfile &removedProfile)
{
    if (removedProfile.id.isEmpty() || m_serverDeletionInFlight) return;
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("删除远程连接"),
        QStringLiteral("确定删除“%1”（%2:%3）吗？连接配置和关联凭据会被移除，此操作不可撤销。")
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
    if (!removedProfile.credentialRef.isEmpty() && !removedProfile.credentialRef.startsWith(QLatin1String("managed/")))
        m_credentialStore->remove(removedProfile.credentialRef);
    m_sidebar->removeServer(removedProfile.id);
    if (m_currentServer.id == removedProfile.id) {
        m_currentServer = {};
        m_serverMeta->setText(QStringLiteral("未选择主机"));
        setConnectionBadge(m_sidebar->servers().isEmpty() ? QStringLiteral("○ 无主机") : QStringLiteral("○ 待连接"),
            QStringLiteral("#738297"), QStringLiteral("#EEF2F6"));
        resetMetrics(QStringLiteral("等待选择主机"));
        m_session = nullptr;
    }
    m_serverDeletionInFlight = false;
}

void MainWindow::setConnectionBadge(const QString &text, const QString &foreground, const QString &background)
{
    if (!m_onlineBadge) return;
    m_onlineBadge->setText(text);
    m_onlineBadge->setProperty("badgeForeground", foreground);
    m_onlineBadge->setProperty("badgeBackground", background);
    QString themedForeground = foreground;
    QString themedBackground = background;
    if (isApplicationDarkTheme()) {
        if (text.contains(QStringLiteral("在线"))) {
            themedForeground = QStringLiteral("#55D6A9");
            themedBackground = QStringLiteral("#15372F");
        } else if (text.contains(QStringLiteral("连接中"))) {
            themedForeground = QStringLiteral("#70B7FF");
            themedBackground = QStringLiteral("#16324A");
        } else {
            themedForeground = QStringLiteral("#9AABBD");
            themedBackground = QStringLiteral("#252F3A");
        }
    }
    m_onlineBadge->setStyleSheet(QStringLiteral("color:%1;background:%2;border-radius:3px;padding:2px 4px;")
            .arg(themedForeground, themedBackground));
}

void MainWindow::updateConnectionPresentation(const QString &serverId, bool connected, const QString &message)
{
    if (serverId.isEmpty()) return;
    const bool terminalConnected = m_terminalWorkspace && m_terminalWorkspace->hasConnectedSession(serverId);
    const bool anyConnected = connected || terminalConnected;

    m_sidebar->setServerState(serverId, anyConnected ? ServerState::Online : ServerState::Offline);
    if (m_currentServer.id != serverId) return;
    // The list aggregates all panes. The monitor is pinned to its top-level
    // tab's source channel, not to whichever pane currently has keyboard focus.
    const bool focused = m_session && m_session->profile().id == serverId;
    const bool currentConnected = focused ? m_session->isConnected() : anyConnected;
    const bool currentConnecting = focused ? m_session->isConnecting() : isConnectingMessage(message);
    if (currentConnected) {
        m_currentServer.state = ServerState::Online;
        setConnectionBadge(QStringLiteral("● 在线"), QStringLiteral("#008858"), QStringLiteral("#E8F8F2"));
    } else if (currentConnecting) {
        setConnectionBadge(QStringLiteral("◌ 连接中"), QStringLiteral("#006EFF"), QStringLiteral("#E8F3FF"));
    } else {
        m_currentServer.state = ServerState::Offline;
        setConnectionBadge(QStringLiteral("○ 离线"), QStringLiteral("#738297"), QStringLiteral("#EEF2F6"));
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_quitInProgress) {
        event->accept();
        return;
    }
    event->ignore();
    const auto behavior = AppSettings::value("general/closeBehavior").toString();
    if (behavior == "quit") { QMetaObject::invokeMethod(qApp, &QCoreApplication::quit, Qt::QueuedConnection); return; }
#ifdef Q_OS_MACOS
    m_hiddenByClose = true;
    hide(); // Dock activation restores this same window and its live sessions.
#else
    showMinimized();
#endif
}

bool MainWindow::confirmApplicationQuit()
{
    if (!AppSettings::value("general/confirmClose").toBool()) return true;
    QMessageBox dialog(QMessageBox::Question, QStringLiteral("确认退出玄壳"),
        QStringLiteral("退出将断开所有 SSH 会话，并停止进行中的传输。\n确认退出程序？"),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    dialog.setObjectName(QStringLiteral("applicationQuitConfirmation"));
    dialog.button(QMessageBox::Yes)->setText(QStringLiteral("退出程序"));
    dialog.button(QMessageBox::Cancel)->setText(QStringLiteral("取消"));
    dialog.setDefaultButton(QMessageBox::Cancel);
    dialog.setEscapeButton(QMessageBox::Cancel);
    return dialog.exec() == QMessageBox::Yes;
}

void MainWindow::requestMetrics()
{
    if (!AppSettings::value("general/serverStatus").toBool()) return;
    // Keep background/minimized windows inexpensive without disconnecting SSH.
    const int interval = !isVisible() || isMinimized() || !isActiveWindow() || !m_monitorRail->isVisible() ? 5000 : 1000;
    if (m_metricTimer->interval() != interval) m_metricTimer->setInterval(interval);
    if (!m_session || !m_session->isConnected()) return;
    if (m_session->terminalFullScreen()) {
        m_sampleStatus->setText(QStringLiteral("终端全屏操作 · 暂缓采样"));
        return;
    }
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
    m_cpuCard->setCoreValues(sample.cpuCorePercents);

    const int memoryProgress = qBound(0, qRound(sample.memoryPercent), 100);
    m_memoryCard->setValue(QStringLiteral("%1%").arg(sample.memoryPercent, 0, 'f', 1),
        QStringLiteral("%1 / %2 GiB").arg(formatGib(sample.memoryUsedBytes), formatGib(sample.memoryTotalBytes)), memoryProgress);

    const int loadProgress = qBound(0, qRound(sample.load1 * 100.0 / qMax(1, sample.cpuCoreCount)), 100);
    m_loadCard->setValue(QString::number(sample.load1, 'f', 2),
        QStringLiteral("1m / %1 核 · 5m %2").arg(sample.cpuCoreCount).arg(sample.load5, 0, 'f', 2), loadProgress);

    if (m_sampleStatus) {
        m_sampleStatus->setText(QStringLiteral("采样 %1    基础 %2 s · 进程 5 s · 本地磁盘 30 s")
            .arg(sample.capturedAt.toString(QStringLiteral("HH:mm:ss")))
            .arg(m_metricTimer->interval() / 1000));
    }
}

void MainWindow::resetMetrics(const QString &detail)
{
    if (!m_cpuCard) return;
    for (auto *card : {m_cpuCard, m_memoryCard, m_loadCard}) {
        card->setValue(QStringLiteral("--"), detail, 0);
    }
    m_cpuCard->setCoreValues({});
    if (m_uptimeValue) m_uptimeValue->setText(QStringLiteral("--"));
    if (m_systemDetailPanel) m_systemDetailPanel->reset(detail);
    if (m_sampleStatus) m_sampleStatus->setText(detail + QStringLiteral("    周期 1.0 s"));
}

} // namespace noxshell::ui
