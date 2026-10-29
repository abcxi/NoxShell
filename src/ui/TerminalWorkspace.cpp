#include "TerminalWorkspace.h"
#include "../core/AppSettings.h"
#include "AgentPanel.h"
#include <QSettings>
#include "../core/TcpLatencyProbe.h"
#include <QShortcut>

#include "../core/CredentialStore.h"
#include "../core/ServerRepository.h"
#include "../core/SshSession.h"
#include "TerminalPanel.h"
#include "TerminalView.h"
#include "FilePanel.h"
#include "RemoteFileEditor.h"
#include "CredentialManagerPanel.h"

#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QSplitter>
#include <QHeaderView>
#include <QGuiApplication>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QProxyStyle>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace noxshell::ui {

namespace {
QStringList monitorEndpoint(const ServerProfile &profile)
{
    return {profile.id,profile.host,QString::number(profile.port),profile.user,
        QString::number(static_cast<int>(profile.connectionMode))};
}

// Reparenting splitter children briefly gives them a minimum-size geometry.
// Only resize their terminal grids after the completed layout is available;
// otherwise those transient widths unnecessarily wrap shell output and reach
// the remote PTY. Ordinary window resizes and IME updates remain immediate.
class TerminalGridResizePause final {
public:
    TerminalGridResizePause(QObject *context, QWidget *page) : m_context(context)
    {
        for (auto *view : page->findChildren<TerminalView *>()) add(view);
    }

    void add(TerminalView *view)
    {
        view->suspendGridResize();
        m_views.append(view);
    }

    ~TerminalGridResizePause()
    {
        QTimer::singleShot(0, m_context, [views = m_views] {
            for (const auto &view : views) if (view) view->resumeGridResize();
        });
    }

private:
    QObject *m_context;
    QList<QPointer<TerminalView>> m_views;
};

class LeftAlignedTabStyle final : public QProxyStyle {
public:
    [[nodiscard]] int styleHint(StyleHint hint, const QStyleOption *option = nullptr,
        const QWidget *widget = nullptr, QStyleHintReturn *returnData = nullptr) const override
    {
        if (hint == QStyle::SH_TabBar_Alignment) return Qt::AlignLeft;
        if (hint == QStyle::SH_TabBar_CloseButtonPosition) return QTabBar::RightSide;
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
};

class SessionTabBar final : public QTabBar {
protected:
    void paintEvent(QPaintEvent *event) override
    {
        QTabBar::paintEvent(event);
        const auto selected = property("multiSelectedTabs").toList();
        if (selected.size() < 2) return;
        QPainter painter(this);
        painter.setPen(QPen(QColor(QStringLiteral("#1684FF")), 1.5));
        painter.setBrush(Qt::NoBrush);
        for (const auto &index : selected) painter.drawRoundedRect(tabRect(index.toInt()).adjusted(2, 2, -2, -2), 4, 4);
    }
};

class PaneIdentityLabel final : public QLabel {
public:
    explicit PaneIdentityLabel(QString text) : QLabel(text), m_fullText(std::move(text))
    {
        setToolTip(m_fullText); setTextFormat(Qt::PlainText);
        setMinimumWidth(0); setMaximumWidth(240);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }
    QSize sizeHint() const override { return {qMin(240, fontMetrics().horizontalAdvance(m_fullText)), fontMetrics().height()}; }
    QSize minimumSizeHint() const override { return {0, fontMetrics().height()}; }
protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        setText(fontMetrics().elidedText(m_fullText, Qt::ElideMiddle, contentsRect().width()));
    }
private:
    QString m_fullText;
};

class TerminalTabToolbar final : public QWidget {
public:
    explicit TerminalTabToolbar(QWidget *parent = nullptr)
        : QWidget(parent)
    {
    }

    void setControls(QTabBar *tabs, QToolButton *addButton)
    {
        m_tabs = tabs;
        m_addButton = addButton;
        m_tabs->installEventFilter(this);
        scheduleButtonPosition();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_tabs && (event->type() == QEvent::LayoutRequest
                || event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
            scheduleButtonPosition();
        }
        return QWidget::eventFilter(watched, event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        scheduleButtonPosition();
    }

private:
    void scheduleButtonPosition()
    {
        QTimer::singleShot(0, this, [this] { positionButton(); });
    }

    void positionButton()
    {
        if (!m_tabs || !m_addButton) return;
        if (layout()) layout()->activate();
        int x = m_tabs->geometry().left() + 2;
        if (m_tabs->count() > 0) {
            x = m_tabs->geometry().left() + m_tabs->tabRect(m_tabs->count() - 1).right() + 5;
        }
        x = std::clamp(x, 2, std::max(2, width() - m_addButton->width() - 4));
        m_addButton->move(x, std::max(0, (height() - m_addButton->height()) / 2));
        m_addButton->raise();
    }

    QTabBar *m_tabs{};
    QToolButton *m_addButton{};
};

QString compactTabTitle(const QString &title)
{
    constexpr int maximumCharacters = 10;
    if (title.size() <= maximumCharacters) return title;
    return title.left(maximumCharacters - 1) + QChar(0x2026);
}

enum class TabConnectionPhase {
    Disconnected,
    Connecting,
    Connected,
};

bool isConnectionProgress(const QString &message)
{
    return message.startsWith(QStringLiteral("正在"))
        || message.startsWith(QStringLiteral("TCP 连接"))
        || message.startsWith(QStringLiteral("等待确认"));
}

QIcon tabConnectionIcon(TabConnectionPhase phase)
{
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF dot(3.0, 3.0, 8.0, 8.0);
    if (phase == TabConnectionPhase::Connected) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#00A870")));
        painter.drawEllipse(dot);
    } else if (phase == TabConnectionPhase::Connecting) {
        QPen pen(QColor(QStringLiteral("#2F88FF")), 2.0);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(dot, 35 * 16, 270 * 16);
    } else {
        painter.setPen(QPen(QColor(QStringLiteral("#8293A6")), 1.4));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(dot);
    }
    return QIcon(pixmap);
}

QString phaseText(TabConnectionPhase phase)
{
    switch (phase) {
    case TabConnectionPhase::Connecting:
        return QStringLiteral("连接中");
    case TabConnectionPhase::Connected:
        return QStringLiteral("连接成功");
    case TabConnectionPhase::Disconnected:
        return QStringLiteral("未连接");
    }
    return {};
}
} // namespace

TerminalWorkspace::TerminalWorkspace(ServerRepository *repository, CredentialStore *credentialStore, QWidget *parent)
    : QWidget(parent)
    , m_repository(repository)
    , m_credentialStore(credentialStore)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_viewStack = new QStackedWidget;
    m_viewStack->setObjectName(QStringLiteral("terminalWorkspaceViewStack"));

    m_emptyPage = new QWidget;
    m_emptyPage->setObjectName(QStringLiteral("terminalRecentPage"));
    auto *emptyLayout = new QVBoxLayout(m_emptyPage);
    emptyLayout->setContentsMargins(22, 14, 22, 20);
    m_homeTabs = new QTabWidget;
    m_homeTabs->setObjectName(QStringLiteral("connectionHomeTabs"));
    m_homeTabs->setDocumentMode(true);
    m_homeTabs->tabBar()->setObjectName(QStringLiteral("connectionHomeTabBar"));
    m_homeTabs->tabBar()->setExpanding(false);
    m_homeTabs->tabBar()->setDrawBase(false);
    auto *historyPage = new QWidget;
    auto *historyLayout = new QVBoxLayout(historyPage);
    historyLayout->setContentsMargins(0, 16, 0, 0);
    historyLayout->setSpacing(12);
    auto *recentHint = new QLabel(QStringLiteral("双击最近登录记录可打开或返回 SSH 会话"));
    recentHint->setObjectName(QStringLiteral("recentLoginHint"));
    m_recentLogins = new QTreeWidget;
    m_recentLogins->setObjectName(QStringLiteral("recentLoginList"));
    m_recentLogins->setColumnCount(4);
    m_recentLogins->setHeaderLabels({QStringLiteral("主机"), QStringLiteral("服务器地址"),
        QStringLiteral("用户"), QStringLiteral("登录时间")});
    m_recentLogins->setRootIsDecorated(false);
    m_recentLogins->setAlternatingRowColors(true);
    m_recentLogins->setSelectionMode(QAbstractItemView::SingleSelection);
    m_recentLogins->setUniformRowHeights(true);
    m_recentLogins->header()->setStretchLastSection(false);
    m_recentLogins->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_recentLogins->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_recentLogins->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_recentLogins->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_recentEmptyLabel = new QLabel(QStringLiteral("暂无成功登录记录\n在“服务器管理”中添加或双击服务器即可连接"));
    m_recentEmptyLabel->setObjectName(QStringLiteral("recentLoginEmpty"));
    m_recentEmptyLabel->setAlignment(Qt::AlignCenter);
    historyLayout->addWidget(recentHint);
    historyLayout->addWidget(m_recentLogins, 1);
    historyLayout->addWidget(m_recentEmptyLabel, 1);
    m_serverManagerPage = new QWidget;
    auto *managerLayout = new QVBoxLayout(m_serverManagerPage);
    managerLayout->setContentsMargins(0, 16, 0, 0);
    m_homeTabs->addTab(historyPage, QStringLiteral("访问历史"));
    m_homeTabs->addTab(m_serverManagerPage, QStringLiteral("服务器管理"));
    auto *credentialsPage = new CredentialManagerPanel(repository, credentialStore, m_homeTabs);
    m_homeTabs->addTab(credentialsPage, QStringLiteral("密码管理"));
    connect(credentialsPage, &CredentialManagerPanel::credentialsChanged, this, &TerminalWorkspace::savedCredentialsChanged);
    emptyLayout->addWidget(m_homeTabs);
    connect(m_homeTabs, &QTabWidget::currentChanged, this, [this, credentialsPage](int index) {
        if (index == 0) refreshRecentLogins();
        if (index == 2) credentialsPage->refresh();
    });

    m_sessionsPage = new QWidget;
    m_sessionsPage->setObjectName(QStringLiteral("terminalSessionsPage"));
    auto *sessionsLayout = new QVBoxLayout(m_sessionsPage);
    sessionsLayout->setContentsMargins(0, 0, 0, 0);
    sessionsLayout->setSpacing(0);

    auto *toolbar = new TerminalTabToolbar;
    m_tabToolbar = toolbar;
    toolbar->setObjectName(QStringLiteral("terminalTabToolbar"));
    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(0, 1, 34, 1);
    toolbarLayout->setSpacing(6);
    m_tabs = new SessionTabBar;
    m_tabs->setObjectName(QStringLiteral("terminalSessionTabs"));
    auto *leftAlignedStyle = new LeftAlignedTabStyle;
    leftAlignedStyle->setParent(m_tabs);
    m_tabs->setStyle(leftAlignedStyle);
    // Cocoa's native tab style can report a near-zero height for a standalone
    // QTabBar. Keep the session strip explicit so its labels do not collapse
    // while the surrounding toolbar remains visible as an empty row.
    m_tabs->setFixedHeight(32);
    m_tabs->setExpanding(false);
    m_tabs->setTabsClosable(true);
    m_tabs->setElideMode(Qt::ElideRight);
    m_tabs->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tabs->installEventFilter(this);
    m_tabs->setToolTip(QStringLiteral("Cmd/Ctrl 点选、Shift 连续多选；右键合并选中的会话。"));
    m_tabMenu = new QMenu(this);
    m_tabMenu->setObjectName(QStringLiteral("terminalTabMenu"));
    m_connectAction = m_tabMenu->addAction(QStringLiteral("连接"));
    m_disconnectAction = m_tabMenu->addAction(QStringLiteral("断开连接"));
    m_connectAction->setObjectName(QStringLiteral("terminalConnectAction"));
    m_disconnectAction->setObjectName(QStringLiteral("terminalDisconnectAction"));
    m_tabMenu->addSeparator();
    auto *clearAction = m_tabMenu->addAction(QStringLiteral("清屏"));
    clearAction->setObjectName(QStringLiteral("terminalClearAction"));
    auto *duplicateAction = m_tabMenu->addAction(QStringLiteral("复制会话"));
    duplicateAction->setObjectName(QStringLiteral("terminalDuplicateAction"));
    m_mergeAction = m_tabMenu->addAction(QStringLiteral("合并选中标签（自动布局）"));
    m_mergeAction->setObjectName(QStringLiteral("terminalMergeSelectedAction"));
    m_unmergeAction = m_tabMenu->addAction(QStringLiteral("解除合并，恢复独立标签"));
    m_unmergeAction->setObjectName(QStringLiteral("terminalUnmergeAction"));
    const QStringList arrangements{QStringLiteral("分屏排列：自动网格"), QStringLiteral("分屏排列：全部左右"), QStringLiteral("分屏排列：全部上下")};
    for (int mode = 0; mode < arrangements.size(); ++mode) {
        auto *action = m_tabMenu->addAction(arrangements[mode]);
        action->setObjectName(QStringLiteral("terminalArrangeAction%1").arg(mode));
        connect(action, &QAction::triggered, this, [this, mode] { arrangeActivePanes(mode); });
    }
    connect(m_mergeAction, &QAction::triggered, this, &TerminalWorkspace::mergeSelectedSessions);
    connect(m_unmergeAction, &QAction::triggered, this, &TerminalWorkspace::unmergeActiveSession);
    m_tabMenu->addSeparator();
    auto *closeCurrentAction = m_tabMenu->addAction(QStringLiteral("关闭当前"));
    m_closeOthersAction = m_tabMenu->addAction(QStringLiteral("关闭其他"));
    auto *closeAllAction = m_tabMenu->addAction(QStringLiteral("关闭全部"));
    closeCurrentAction->setObjectName(QStringLiteral("terminalCloseCurrentAction"));
    m_closeOthersAction->setObjectName(QStringLiteral("terminalCloseOthersAction"));
    closeAllAction->setObjectName(QStringLiteral("terminalCloseAllAction"));
    // 标签栏保持完整可用宽度，具体标签通过样式提示从左侧开始排列。
    // 不能压缩 QTabBar 本身：macOS 原生样式会因此停止绘制标签。
    toolbarLayout->addWidget(m_tabs, 1);
    auto *newTabButton = new QToolButton(toolbar);
    newTabButton->setObjectName(QStringLiteral("terminalNewTabButton"));
    newTabButton->setText(QStringLiteral("+"));
    newTabButton->setToolTip(QStringLiteral("服务器管理 / 新建连接"));
    newTabButton->setAccessibleName(QStringLiteral("打开服务器管理"));
    newTabButton->setFixedSize(28, 28);
    toolbar->setControls(m_tabs, newTabButton);

    m_stack = new QStackedWidget;
    m_stack->setObjectName(QStringLiteral("terminalSessionStack"));
    sessionsLayout->addWidget(m_stack, 1);
    m_viewStack->addWidget(m_emptyPage);
    m_viewStack->addWidget(m_sessionsPage);
    connect(m_viewStack, &QStackedWidget::currentChanged, this, [this] {
        if (isHomePageVisible()) restoreExpandedFileWorkspace();
        emit homePageVisibilityChanged(isHomePageVisible());
        updateLatencyProbes();
    });
    connect(AppSettings::instance(), &AppSettings::changed, this, [this](const QString &key) {
        if (key == QStringLiteral("general/showLatency") || key.startsWith(QStringLiteral("proxy/")))
            updateLatencyProbes(true);
    });
    layout->addWidget(toolbar);
    layout->addWidget(m_viewStack, 1);

    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) {
        restoreExpandedFileWorkspace();
        if (index >= 0 && index < m_stack->count()) m_stack->setCurrentIndex(index);
        if (index >= 0 && index < m_stack->count()) m_viewStack->setCurrentWidget(m_sessionsPage);
        publishActiveSession();
        persistState();
    });
    connect(m_tabs, &QTabBar::tabBarClicked, this, [this](int index) {
        if (index < 0 || index >= m_stack->count()) return;
        m_tabs->setCurrentIndex(index);
        m_stack->setCurrentIndex(index);
        m_viewStack->setCurrentWidget(m_sessionsPage);
        publishActiveSession();
    });
    connect(newTabButton, &QToolButton::clicked, this, &TerminalWorkspace::showServerManager);
    connect(m_tabs, &QTabBar::tabCloseRequested, this, &TerminalWorkspace::closeSession);
    connect(m_tabs, &QTabBar::customContextMenuRequested, this,
        [this](const QPoint &position) {
        const int index = m_tabs->tabAt(position);
        if (!prepareTabContextMenu(index)) return;
        // The minimal/offscreen platform plugins used by headless CI do not
        // support popup activation or keyboard grabs. The action state above
        // remains fully testable without asking those plugins to show a menu.
        const QString platformName = QGuiApplication::platformName();
        if (platformName != QStringLiteral("minimal")
            && platformName != QStringLiteral("offscreen")) {
            m_tabMenu->popup(m_tabs->mapToGlobal(position));
        }
    });
    connect(m_connectAction, &QAction::triggered, this, [this] {
        if (m_tabContextIndex < 0 || m_tabContextIndex >= m_stack->count()) return;
        auto *page = activePane(m_stack->widget(m_tabContextIndex));
        if (!page) return;
        const auto next = page->property("nextConnectionProfile");
        const auto profile = (next.isValid() ? next : page->property("serverProfile")).value<ServerProfile>();
        if (profile.id.isEmpty()) return;
        page->setProperty("serverProfile", QVariant::fromValue(profile));
        page->setProperty("nextConnectionProfile", {});
        page->setProperty("serverConfigurationCurrent", true);
        updateTabPresentation(m_stack->widget(m_tabContextIndex));
        if (auto *panel = page->findChild<TerminalPanel *>()) panel->connectToServer(profile);
        publishActiveSession();
    });
    connect(m_disconnectAction, &QAction::triggered, this, [this] {
        if (m_tabContextIndex < 0 || m_tabContextIndex >= m_stack->count()) return;
        if (auto *session = (activePane(m_stack->widget(m_tabContextIndex)) ? activePane(m_stack->widget(m_tabContextIndex))->findChild<SshSession *>() : nullptr)) {
            session->disconnectFromHost();
        }
    });
    connect(clearAction, &QAction::triggered, this, [this] {
        if (m_tabContextIndex < 0 || m_tabContextIndex >= m_stack->count()) return;
        if (auto *panel = (activePane(m_stack->widget(m_tabContextIndex)) ? activePane(m_stack->widget(m_tabContextIndex))->findChild<TerminalPanel *>() : nullptr)) {
            panel->clearTerminal();
        }
    });
    connect(duplicateAction, &QAction::triggered, this, [this] { duplicateSessionAt(m_tabContextIndex); });
    connect(closeCurrentAction, &QAction::triggered, this, [this] { closeSession(m_tabContextIndex); });
    connect(m_closeOthersAction, &QAction::triggered, this, [this] { closeOtherSessions(m_tabContextIndex); });
    connect(closeAllAction, &QAction::triggered, this, &TerminalWorkspace::closeAllSessions);
    connect(m_recentLogins, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (!item || !m_repository) return;
        const auto serverId = item->data(0, Qt::UserRole).toString();
        const auto servers = m_repository->loadServers();
        const auto found = std::find_if(servers.cbegin(), servers.cend(), [&serverId](const ServerProfile &profile) {
            return profile.id == serverId;
        });
        if (found != servers.cend()) {
            openOrActivate(*found, true);
        }
    });

    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *focused) {
        for (auto *widget = focused; widget; widget = widget->parentWidget()) {
            if (widget->objectName() == QStringLiteral("terminalSplitPane") && isAncestorOf(widget)) {
                activatePane(widget);
                break;
            }
        }
    });
    connect(AppSettings::instance(), &AppSettings::changed, this, [this](const QString &key) {
        if (key.isEmpty() || key.startsWith("tabs/")) for (int i=0;i<m_stack->count();++i) updateTabPresentation(m_stack->widget(i));
    });
    if(m_repository && AppSettings::value("general/openTerminal").toBool()) {
        const auto state=m_repository->loadTerminalState();
        const auto servers=m_repository->loadServers();
        for(const auto &id:state.serverIds) {
            const auto found=std::find_if(servers.cbegin(),servers.cend(),[&id](const auto &p){return p.id==id;});
            if(found!=servers.cend() && found->connectionMode!=ConnectionMode::Rdp)addSession(*found,false,false,false);
        }
        if(m_tabs->count()) {
            const QSignalBlocker blocker(m_tabs);
            const int index=qBound(0,state.currentIndex,m_tabs->count()-1);
            m_tabs->setCurrentIndex(index);m_stack->setCurrentIndex(index);
            m_viewStack->setCurrentWidget(m_sessionsPage);publishActiveSession();
        }
    }
    refreshRecentLogins();
    updateWorkspaceState();
}

void TerminalWorkspace::showSettings(QWidget *page)
{
    restoreExpandedFileWorkspace();
    int index = -1;
    for (int i=0;i<m_stack->count();++i) if (m_stack->widget(i)->property("settingsTab").toBool()) {index=i;break;}
    if (index < 0) {
        page->setProperty("settingsTab",true);
        const QSignalBlocker blocker(m_tabs);
        index=m_tabs->addTab(QStringLiteral("设置"));
        m_tabs->setTabData(index,QStringLiteral("__settings__"));
        m_stack->addWidget(page); installCloseButton(index);
    } else if (page != m_stack->widget(index)) page->deleteLater();
    m_tabs->setCurrentIndex(index); m_stack->setCurrentIndex(index); updateWorkspaceState(); emit tabsChanged();
}

void TerminalWorkspace::setServerManager(QWidget *manager)
{
    if (manager) m_serverManagerPage->layout()->addWidget(manager);
}

void TerminalWorkspace::showServerManager()
{
    // Only switch the view. Existing SSH channels, monitor sampling and SFTP
    // state belong to the session tabs and must remain untouched.
    m_homeTabs->setCurrentIndex(1);
    m_viewStack->setCurrentWidget(m_emptyPage);
}

void TerminalWorkspace::showCredentialManager()
{
    // Credential records live on the home page beside server management.
    // Keeping this navigation separate from session tabs preserves active SSH work.
    m_homeTabs->setCurrentIndex(2);
    m_viewStack->setCurrentWidget(m_emptyPage);
}

bool TerminalWorkspace::isHomePageVisible() const
{
    return m_viewStack->currentWidget() == m_emptyPage;
}

bool TerminalWorkspace::prepareTabContextMenu(int index)
{
    if (index < 0 || index >= m_tabs->count() || index >= m_stack->count()) return false;
    if (!activePane(m_stack->widget(index))) return false;
    m_tabContextIndex = index;
    if (!m_selectedPages.contains(m_stack->widget(index))) selectSessionTab(index);
    m_tabs->setCurrentIndex(index);
    const auto phase = static_cast<TabConnectionPhase>(
        activePane(m_stack->widget(index))->property("terminalConnectionPhase").toInt());
    m_connectAction->setEnabled(phase == TabConnectionPhase::Disconnected);
    m_disconnectAction->setEnabled(phase != TabConnectionPhase::Disconnected);
    m_closeOthersAction->setEnabled(m_tabs->count() > 1);
    int count = 0;
    for (const auto &page : m_selectedPages) if (page && m_stack->indexOf(page) >= 0) count += panes(page).size();
    m_mergeAction->setEnabled(m_selectedPages.size() > 1 && count <= 8);
    m_mergeAction->setToolTip(QStringLiteral("合并已有连接，不重连；最多 8 个分屏。Cmd/Ctrl 点选或 Shift 多选标签。"));
    m_unmergeAction->setEnabled(panes(m_stack->widget(index)).size() > 1);
    for (int mode = 0; mode < 3; ++mode)
        findChild<QAction *>(QStringLiteral("terminalArrangeAction%1").arg(mode))->setEnabled(panes(m_stack->widget(index)).size() > 1);
    return true;
}

void TerminalWorkspace::selectSessionTab(int index, int selectionMode)
{
    if (index < 0 || index >= m_stack->count()) return;
    auto *page = m_stack->widget(index);
    if (!activePane(page)) { m_selectedPages.clear(); m_selectionAnchor.clear(); }
    else if (selectionMode == 2 && m_selectionAnchor && m_stack->indexOf(m_selectionAnchor) >= 0) {
        const int anchor = m_stack->indexOf(m_selectionAnchor);
        m_selectedPages.clear();
        for (int i = qMin(anchor, index); i <= qMax(anchor, index); ++i)
            if (activePane(m_stack->widget(i))) m_selectedPages.append(m_stack->widget(i));
    } else if (selectionMode == 1) {
        if (m_selectedPages.contains(page)) m_selectedPages.removeAll(page);
        else m_selectedPages.append(page);
        m_selectionAnchor = page;
    } else {
        m_selectedPages = {page}; m_selectionAnchor = page;
    }
    m_tabs->setCurrentIndex(index);
    m_stack->setCurrentIndex(index); m_viewStack->setCurrentWidget(m_sessionsPage);
    updateTabSelection();
    publishActiveSession();
}

void TerminalWorkspace::updateTabSelection()
{
    QVariantList indices;
    for (auto it = m_selectedPages.begin(); it != m_selectedPages.end();) {
        if (!*it || m_stack->indexOf(*it) < 0) it = m_selectedPages.erase(it);
        else { indices.append(m_stack->indexOf(*it)); ++it; }
    }
    m_tabs->setProperty("multiSelectedTabs", indices);
    m_tabs->update(); emit tabsChanged();
}

int TerminalWorkspace::sessionCount() const
{
    int count = 0;
    for (int i = 0; i < m_stack->count(); ++i) if (!panes(m_stack->widget(i)).isEmpty()) ++count;
    return count;
}

bool TerminalWorkspace::hasConnectedSession(const QString &serverId) const
{
    for (int index = 0; index < m_stack->count(); ++index) {
        for (auto *pane : panes(m_stack->widget(index))) {
            if (pane->property("serverProfile").value<ServerProfile>().id != serverId) continue;
            if (auto *session = pane->findChild<SshSession *>(); session && session->isConnected()) return true;
        }
    }
    return false;
}

bool TerminalWorkspace::hasConnectingSession(const QString &serverId) const
{
    for (int index = 0; index < m_stack->count(); ++index) {
        for (auto *pane : panes(m_stack->widget(index))) {
            if (pane->property("serverProfile").value<ServerProfile>().id != serverId) continue;
            if (auto *session = pane->findChild<SshSession *>(); session && session->isConnecting()) return true;
        }
    }
    return false;
}

bool TerminalWorkspace::activateExisting(const QString &serverId)
{
    QWidget *fallback = nullptr;
    for (int index = m_stack->count() - 1; index >= 0; --index) {
        for (auto *pane : panes(m_stack->widget(index))) {
            if (pane->property("serverProfile").value<ServerProfile>().id != serverId) continue;
            if (!fallback) fallback = pane;
            if (pane->property("serverConfigurationCurrent").toBool()) { fallback = pane; break; }
        }
        if (fallback && fallback->property("serverConfigurationCurrent").toBool()) break;
    }
    if (!fallback) return false;
    const int index = m_stack->indexOf(fallback->property("terminalGroupPage").value<QWidget *>());
    selectSessionTab(index); activatePane(fallback); publishActiveSession();
    return true;
}

void TerminalWorkspace::openOrActivate(const ServerProfile &profile, bool connectNow)
{
    for (int index = 0; index < m_tabs->count(); ++index) {
        for (auto *page : panes(m_stack->widget(index))) {
        if (page->property("serverProfile").value<ServerProfile>().id != profile.id) continue;
        if (!page->property("serverConfigurationCurrent").toBool()) continue;
        m_viewStack->setCurrentWidget(m_sessionsPage);
        selectSessionTab(index); activatePane(page); publishActiveSession();
        if (connectNow) {
            if (auto *panel = page->findChild<TerminalPanel *>()) panel->connectToServer(profile);
        }
        return;
        }
    }
    addSession(profile, true, true, connectNow);
}

void TerminalWorkspace::duplicateSession(const ServerProfile &profile)
{
    addSession(profile, true, true, true);
}

void TerminalWorkspace::duplicateSessionAt(int index)
{
    if (index < 0 || index >= m_stack->count() || !activePane(m_stack->widget(index))) return;
    if (index < 0 || index >= m_stack->count()) return;
    const auto *page = activePane(m_stack->widget(index));
    const auto next = page->property("nextConnectionProfile");
    const auto profile = (next.isValid() ? next : page->property("serverProfile")).value<ServerProfile>();
    if (!profile.id.isEmpty()) duplicateSession(profile);
}

void TerminalWorkspace::updateServer(const ServerProfile &profile)
{
    for (int index = 0; index < m_tabs->count(); ++index) {
        // A running terminal is an immutable connection snapshot. Editing the
        // saved host must not rewrite, clear or reconnect that existing SSH
        // channel. Keep the next-connection configuration separate so a tab
        // reconnect/duplicate cannot accidentally reuse the previous password.
        for (auto *pane : panes(m_stack->widget(index))) {
            if (pane->property("serverProfile").value<ServerProfile>().id != profile.id) continue;
            auto next = profile;
            next.password.clear();
            next.keyPassphrase.clear();
            pane->setProperty("nextConnectionProfile", QVariant::fromValue(next));
            pane->setProperty("serverConfigurationCurrent", false);
        }
    }
}

void TerminalWorkspace::closeServer(const QString &serverId)
{
    QList<QPointer<QWidget>> matching;
    for (int index = m_tabs->count() - 1; index >= 0; --index) {
        for (auto *pane : panes(m_stack->widget(index)))
            if (pane->property("serverProfile").value<ServerProfile>().id == serverId) matching.append(pane);
    }
    // Deleting one host from a merged tab must not close unrelated hosts.
    for (const auto &pane : matching) if (pane
        && m_stack->indexOf(pane->property("terminalGroupPage").value<QWidget *>()) >= 0) removePane(pane);
}

void TerminalWorkspace::setFileWorkspaceVisible(bool visible)
{
    m_fileWorkspaceVisible = visible;
    if (auto *pane = currentPane()) setPaneFileWorkspaceVisible(pane, visible);
}

void TerminalWorkspace::setPaneFileWorkspaceVisible(QWidget *pane, bool visible)
{
    if (m_expandedFilePane == pane && !visible) restoreExpandedFileWorkspace();
    auto *files = pane->findChild<QWidget *>(QStringLiteral("fileWorkspacePane"));
    auto *splitter = pane->findChild<QSplitter *>(QStringLiteral("terminalFileSplitter"));
    if (!files || !splitter) return;
    pane->setProperty("fileWorkspaceVisible", visible);
    pane->findChild<TerminalPanel *>()->setFileWorkspaceVisible(visible);
    if (files->isHidden() == !visible) return;
    TerminalGridResizePause resizePause(this, pane);
    if (!visible) {
        const auto sizes = splitter->sizes();
        if (sizes.size() == 2 && sizes.at(0) > 0 && sizes.at(1) > 0)
            pane->setProperty("terminalFileSplitSizes", QVariant::fromValue(sizes));
        files->hide();
    } else {
        files->show();
        auto sizes = pane->property("terminalFileSplitSizes").value<QList<int>>();
        if (sizes.size() != 2) {
            const int available = qMax(300, splitter->height());
            sizes = {available * 65 / 100, available * 35 / 100};
        }
        splitter->setSizes(sizes);
    }
}

void TerminalWorkspace::setPaneFileWorkspaceExpanded(QWidget *pane, bool expanded)
{
    if (!expanded) {
        if (m_expandedFilePane == pane) restoreExpandedFileWorkspace();
        return;
    }
    if (m_expandedFilePane == pane) return;
    restoreExpandedFileWorkspace();
    auto *group = pane ? pane->property("terminalGroupPage").value<QWidget *>() : nullptr;
    if (!group || m_stack->currentWidget() != group) return;
    setPaneFileWorkspaceVisible(pane, true);
    activatePane(pane);
    m_expandedFilePane = pane;
    // Keep the widgets in their own pane. No editor, session, or splitter is
    // reparented, and hidden terminals keep their original remote PTY size.
    for (auto *view : group->findChildren<TerminalView *>()) {
        view->suspendGridResize();
        m_expansionSuspendedViews.append(view);
    }
    for (auto *splitter : group->findChildren<QSplitter *>()) {
        if (splitter->objectName() == QStringLiteral("terminalPaneSplitter")
            || splitter->objectName() == QStringLiteral("paneEditorSplitter")
            || splitter->objectName() == QStringLiteral("terminalFileSplitter"))
            m_expansionSplitStates.append({splitter, splitter->saveState()});
    }
    const auto hideForExpansion = [this](QWidget *widget) {
        if (!widget || widget->isHidden()) return;
        m_expansionHiddenWidgets.append(widget);
        widget->hide();
    };
    for (auto *branch = pane; branch && branch != group; branch = branch->parentWidget()) {
        if (auto *splitter = qobject_cast<QSplitter *>(branch->parentWidget())) {
            for (int index = 0; index < splitter->count(); ++index)
                if (auto *sibling = splitter->widget(index); sibling != branch) hideForExpansion(sibling);
        }
    }
    hideForExpansion(pane->findChild<QStackedWidget *>(QStringLiteral("terminalModeStack")));
    // Maximise the browser only; the full-height editor remains its right-hand
    // sibling. Restore both horizontal and vertical ratios afterwards.
    hideForExpansion(pane->findChild<QFrame *>(QStringLiteral("terminalPaneHeader")));
    pane->findChild<FilePanel *>()->setExpanded(true);
}

void TerminalWorkspace::restoreExpandedFileWorkspace()
{
    if (!m_expandedFilePane) return;
    if (auto *files = m_expandedFilePane->findChild<FilePanel *>()) files->setExpanded(false);
    for (const auto &widget : m_expansionHiddenWidgets) if (widget) widget->show();
    for (const auto &[splitter, state] : m_expansionSplitStates) if (splitter) splitter->restoreState(state);
    // Let layouts settle before restoring terminal grid updates; otherwise a
    // transient narrow geometry could unnecessarily reflow the remote shell.
    QTimer::singleShot(0, this, [views = m_expansionSuspendedViews] {
        for (const auto &view : views) if (view) view->resumeGridResize();
    });
    m_expandedFilePane.clear();
    m_expansionHiddenWidgets.clear();
    m_expansionSplitStates.clear();
    m_expansionSuspendedViews.clear();
}

bool TerminalWorkspace::fileWorkspaceVisible() const
{
    if (auto *pane = currentPane()) return pane->property("fileWorkspaceVisible").toBool();
    return m_fileWorkspaceVisible;
}

QList<QWidget *> TerminalWorkspace::panes(QWidget *page) const
{
    QList<QWidget *> result;
    if (!page) return result;
    // QWidget::raise() changes QObject child order. Use the splitter's visual
    // order instead, so focus changes cannot shuffle merge/unmerge or restore.
    const std::function<void(QWidget *)> visit = [&](QWidget *widget) {
        if (!widget) return;
        if (widget->objectName() == QStringLiteral("terminalSplitPane")) { result.append(widget); return; }
        if (auto *split = qobject_cast<QSplitter *>(widget);
            split && split->objectName() == QStringLiteral("terminalPaneSplitter"))
            for (int i = 0; i < split->count(); ++i) visit(split->widget(i));
    };
    if (page->objectName() == QStringLiteral("terminalSplitPane")) result.append(page);
    else if (page->layout()) for (int i = 0; i < page->layout()->count(); ++i) visit(page->layout()->itemAt(i)->widget());
    return result;
}

QWidget *TerminalWorkspace::activePane(QWidget *page) const
{
    if (!page) return nullptr;
    if (auto *pane = page->property("activeTerminalPane").value<QWidget *>()) return pane;
    const auto children = panes(page);
    return children.isEmpty() ? nullptr : children.first();
}

QWidget *TerminalWorkspace::currentPane() const
{
    return activePane(m_stack->currentWidget());
}

int TerminalWorkspace::activePaneCount() const
{
    return panes(m_stack->currentWidget()).size();
}

void TerminalWorkspace::setNativeTitlebarTabs(bool enabled)
{
    m_nativeTitlebarTabs = enabled;
    m_tabToolbar->setVisible(!enabled && m_tabs->count() > 0);
}

bool TerminalWorkspace::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_tabs && event->type() == QEvent::MouseButtonPress) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton) {
            const int index = m_tabs->tabAt(mouse->position().toPoint());
            if (index >= 0) {
                const auto modifiers = mouse->modifiers();
                selectSessionTab(index, modifiers.testFlag(Qt::ShiftModifier) ? 2
                    : (modifiers.testFlag(Qt::ControlModifier) || modifiers.testFlag(Qt::MetaModifier)) ? 1 : 0);
                return true;
            }
        }
    }
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::FocusIn) {
        for (auto *widget = qobject_cast<QWidget *>(watched); widget; widget = widget->parentWidget()) {
            if (widget->objectName() == QStringLiteral("terminalSplitPane") && isAncestorOf(widget)) {
                activatePane(widget);
                break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TerminalWorkspace::activatePane(QWidget *pane, bool focus)
{
    if (!pane) return;
    auto *page = pane->property("terminalGroupPage").value<QWidget *>();
    if (!page || m_stack->currentWidget() != page) return;
    const bool changed = activePane(page) != pane;
    page->setProperty("activeTerminalPane", QVariant::fromValue(pane));
    page->setProperty("serverProfile", pane->property("serverProfile"));
    for (auto *candidate : panes(page)) {
        auto *panel = candidate->findChild<TerminalPanel *>();
        auto *header = candidate->findChild<QFrame *>(QStringLiteral("terminalPaneHeader"));
        // Repeated clicks inside the same pane must not restyle its entire
        // terminal/editor tree. Only the focus transition changes this frame.
        for (QWidget *target : {candidate, static_cast<QWidget *>(panel), static_cast<QWidget *>(header)}) {
            const auto previous = target->property("activeSplitPane");
            if (previous.isValid() && previous.toBool() == (candidate == pane)) continue;
            target->setProperty("activeSplitPane", candidate == pane);
            target->style()->unpolish(target);
            target->style()->polish(target);
            target->update();
        }
    }
    updateTabPresentation(page);
    if (changed) { publishActiveSession(); persistState(); }
    if (focus) {
        auto *modes = pane->findChild<QStackedWidget *>(QStringLiteral("terminalModeStack"));
        auto *terminal = modes && modes->currentWidget() ? modes->currentWidget()->findChild<TerminalView *>() : nullptr;
        if (terminal && terminal->isEnabled() && terminal->isVisible()) terminal->setFocus();
        else pane->setFocus();
    }
}

void TerminalWorkspace::updateTabPresentation(QWidget *page)
{
    const int index = m_stack->indexOf(page);
    auto *pane = activePane(page);
    if (index < 0 || !pane) return;
    const auto profile = pane->property("serverProfile").value<ServerProfile>();
    page->setProperty("serverProfile", pane->property("serverProfile"));
    page->setProperty("terminalConnectionPhase", pane->property("terminalConnectionPhase"));
    page->setProperty("serverConfigurationCurrent", pane->property("serverConfigurationCurrent"));
    const int count = panes(page).size();
    const auto phase = static_cast<TabConnectionPhase>(pane->property("terminalConnectionPhase").toInt());
    QString title = profile.name;
    if (count > 1) title = QStringLiteral("[%1] %2").arg(count).arg(title);
    if (AppSettings::value("tabs/showUser").toBool()) title += " · " + profile.user;
    if (AppSettings::value("tabs/showHost").toBool()) title += " · " + profile.host;
    const int maxLength = qBound(8, AppSettings::value("tabs/maxTitleLength").toInt(), 60);
    m_tabs->setTabText(index, title.size() > maxLength ? title.left(maxLength - 1) + QChar(0x2026) : title);
    m_tabs->setTabIcon(index, tabConnectionIcon(phase));
    m_tabs->setTabData(index, profile.id);
    m_tabs->setTabToolTip(index, profile.name + QStringLiteral(" · ") + QStringLiteral("%1 · %2@%3:%4%5%6")
        .arg(phaseText(phase), profile.user, profile.host).arg(profile.port)
        .arg(count > 1 ? QStringLiteral(" · %1 个分屏").arg(count) : QString{})
        .arg(pane->property("connectionMessage").toString().isEmpty() ? QString{}
            : QStringLiteral(" · %1").arg(pane->property("connectionMessage").toString()))
        + QStringLiteral("\nCmd/Ctrl 点选、Shift 多选；右键合并（最多 8 个分屏）"));
    emit tabsChanged();
}

QWidget *TerminalWorkspace::createPane(QWidget *groupPage, const ServerProfile &profile, bool fileVisible)
{
    auto *page = new QFrame(groupPage);
    page->setObjectName(QStringLiteral("terminalSplitPane"));
    page->setFocusPolicy(Qt::ClickFocus);
    page->setMinimumSize(220, 140);
    page->setProperty("terminalGroupPage", QVariant::fromValue(groupPage));
    page->setProperty("fileWorkspaceVisible", fileVisible);
    page->setProperty("serverProfile", QVariant::fromValue(profile));
    page->setProperty("serverConfigurationCurrent", true);
    page->setProperty("terminalConnectionPhase", static_cast<int>(TabConnectionPhase::Disconnected));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    auto *session = new SshSession(m_repository, m_credentialStore, page);
    session->setTransferPersistenceEnabled(false);
    auto *panel = new TerminalPanel(session, m_repository);
    panel->setFileWorkspaceVisible(fileVisible);
    auto *commandColumn = new QWidget;
    commandColumn->setObjectName(QStringLiteral("terminalCommandColumn"));
    auto *commandLayout = new QVBoxLayout(commandColumn);
    commandLayout->setContentsMargins(0, 0, 0, 0);
    commandLayout->setSpacing(0);
    auto *terminalFileSplitter = new QSplitter(Qt::Vertical);
    terminalFileSplitter->setObjectName(QStringLiteral("terminalFileSplitter"));
    terminalFileSplitter->setChildrenCollapsible(false);
    terminalFileSplitter->setHandleWidth(5);
    terminalFileSplitter->addWidget(panel);
    auto *fileWorkspace = new QWidget;
    fileWorkspace->setObjectName(QStringLiteral("fileWorkspacePane"));
    auto *fileLayout = new QVBoxLayout(fileWorkspace);
    fileLayout->setContentsMargins(0, 0, 0, 0);
    fileLayout->setSpacing(0);
    auto *files = new FilePanel(session);
    files->setMinimumHeight(150);
    files->setServer(profile);
    fileLayout->addWidget(files);
    connect(files, &FilePanel::expansionRequested, this, [this, page](bool expanded) {
        setPaneFileWorkspaceExpanded(page, expanded);
    });
    connect(files, &FilePanel::closeWorkspaceRequested, this, [this, page] {
        setPaneFileWorkspaceVisible(page, false);
    });
    terminalFileSplitter->addWidget(fileWorkspace);
    fileWorkspace->setVisible(fileVisible);
    connect(terminalFileSplitter, &QSplitter::splitterMoved, this, [page, terminalFileSplitter, fileWorkspace] {
        if (!fileWorkspace->isHidden())
            page->setProperty("terminalFileSplitSizes", QVariant::fromValue(terminalFileSplitter->sizes()));
    });
    // This belongs to the isolated pane, NOT to the bottom file drawer. The
    // terminal and browser stay stacked on the left; document tabs span the
    // full available height on the right, even while the drawer is hidden.
    auto *paneEditorSplitter = new QSplitter(Qt::Horizontal);
    paneEditorSplitter->setObjectName(QStringLiteral("paneEditorSplitter"));
    paneEditorSplitter->setChildrenCollapsible(false);
    paneEditorSplitter->setHandleWidth(5);
    paneEditorSplitter->addWidget(commandColumn);
    paneEditorSplitter->setStretchFactor(0, 1);
    connect(paneEditorSplitter, &QSplitter::splitterMoved, page, [page, paneEditorSplitter] {
        if (paneEditorSplitter->count() == 2)
            page->setProperty("paneEditorSplitSizes", QVariant::fromValue(paneEditorSplitter->sizes()));
    });
    connect(files, &FilePanel::fileOpenRequested, page, [this, page, session, paneEditorSplitter](const QString &path) {
        TerminalGridResizePause resizePause(this, page);
        auto *editor = paneEditorSplitter->findChild<RemoteFileEditor *>(QString{}, Qt::FindDirectChildrenOnly);
        if (editor) {
            editor->openFile(path);
            return;
        }
        const auto profile = page->property("serverProfile").value<ServerProfile>();
        editor = new RemoteFileEditor(session, profile.name, path, paneEditorSplitter);
        paneEditorSplitter->addWidget(editor);
        paneEditorSplitter->setStretchFactor(0, 1);
        paneEditorSplitter->setStretchFactor(1, 1);
        editor->show();
        auto sizes = page->property("paneEditorSplitSizes").value<QList<int>>();
        if (sizes.size() != 2) {
            const int available = qMax(600, paneEditorSplitter->width());
            sizes = {available / 2, available / 2};
        }
        paneEditorSplitter->setSizes(sizes);
    });
    auto *header = new QFrame;
    header->setObjectName(QStringLiteral("terminalPaneHeader"));
    header->setFixedHeight(28);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(8, 2, 4, 2);
    headerLayout->setSpacing(4);
    auto *identity = new PaneIdentityLabel(QStringLiteral("%1@%2:%3").arg(profile.user, profile.host).arg(profile.port));
    identity->setObjectName(QStringLiteral("terminalPaneIdentity"));
    headerLayout->addWidget(identity);
    auto *modes = new QTabBar; modes->setObjectName("terminalModeTabs");
    modes->setExpanding(false); modes->setDrawBase(false); modes->setDocumentMode(true);
    modes->setUsesScrollButtons(false); modes->setFixedSize(134, 22); modes->setIconSize(QSize(12, 12));
    modes->addTab(QIcon(QStringLiteral(":/assets/terminal-shell.svg")), QStringLiteral("Shell"));
    modes->addTab(QIcon(QStringLiteral(":/assets/agent-mode.svg")), QStringLiteral("Agent"));
    modes->setTabToolTip(0, QStringLiteral("Shell · 交互式终端"));
    modes->setTabToolTip(1, QStringLiteral("Agent · 独立智能体会话（点击后按需启动）"));
    headerLayout->addWidget(modes);
    headerLayout->addStretch(1);
    auto *modeStack = new QStackedWidget; modeStack->setObjectName("terminalModeStack");
    modeStack->addWidget(panel);
    terminalFileSplitter->insertWidget(0, modeStack);
    // Wrapping the panel reparents it out of the splitter. Apply its initial
    // proportions afterwards, otherwise the browser inherits the freed space.
    terminalFileSplitter->setStretchFactor(0, 65);
    terminalFileSplitter->setStretchFactor(1, 35);
    terminalFileSplitter->setSizes({560, 300});
    connect(modes, &QTabBar::currentChanged, page, [this, page, modeStack, session, profile](int index) {
        if (index == 1 && modeStack->count() == 1) {
            auto *agent = new AgentPanel(session, m_repository, m_credentialStore, modeStack);
            agent->setObjectName("paneAgentPanel");
            modeStack->addWidget(agent);
            connect(agent, &AgentPanel::statusChanged, this, [this, profile](const QString &state) {
                if (!AppSettings::value("agent/notifications").toBool()) return;
                auto *notice = window()->findChild<QLabel *>("agentNotificationBanner");
                if (!notice) { notice = new QLabel(window()); notice->setObjectName("agentNotificationBanner"); notice->setAlignment(Qt::AlignCenter); notice->setStyleSheet("background:#168DFF;color:white;border-radius:16px;padding:8px 20px;"); }
                notice->setText(profile.name + " · Agent · " + state); notice->adjustSize();
                notice->move((window()->width()-notice->width())/2, 10); notice->show(); notice->raise();
                const auto serial = notice->property("noticeSerial").toInt()+1; notice->setProperty("noticeSerial",serial);
                QTimer::singleShot(5000,notice,[notice,serial]{if(notice->property("noticeSerial").toInt()==serial)notice->hide();});
            });
        }
        modeStack->setCurrentIndex(index); activatePane(page);
    });
    auto *modeShortcut = new QShortcut(page); modeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    const auto updateModeShortcut = [modeShortcut] { modeShortcut->setKey(QKeySequence(QSettings().value("shortcuts/agent","Ctrl+Shift+A").toString())); };
    connect(modeShortcut, &QShortcut::activated, modes, [modes]{modes->setCurrentIndex(1-modes->currentIndex());});
    connect(AppSettings::instance(), &AppSettings::changed, page, [updateModeShortcut](const QString &){updateModeShortcut();}); updateModeShortcut();
    auto *more = new QToolButton;
    more->setObjectName(QStringLiteral("terminalPaneMoreButton"));
    more->setText(QStringLiteral("⋮"));
    more->setToolTip(QStringLiteral("分屏与会话操作"));
    more->setFixedSize(22, 22);
    more->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(more);
    auto *splitVertical = menu->addAction(QStringLiteral("垂直切分（左右）"));
    auto *splitHorizontal = menu->addAction(QStringLiteral("水平切分（上下）"));
    splitVertical->setObjectName(QStringLiteral("terminalSplitVerticalAction"));
    splitHorizontal->setObjectName(QStringLiteral("terminalSplitHorizontalAction"));
#ifdef Q_OS_MACOS
    splitVertical->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    splitHorizontal->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));
#else
    splitVertical->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));
    splitHorizontal->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
#endif
    const auto updateSplitKeys = [splitVertical, splitHorizontal] {
#ifdef Q_OS_MACOS
        const QString vertical = "Ctrl+D", horizontal = "Ctrl+Shift+D";
#else
        const QString vertical = "Ctrl+Shift+D", horizontal = "Ctrl+Shift+E";
#endif
        splitVertical->setShortcut(QKeySequence(QSettings().value("shortcuts/splitVertical",vertical).toString()));
        splitHorizontal->setShortcut(QKeySequence(QSettings().value("shortcuts/splitHorizontal",horizontal).toString()));
    };
    connect(AppSettings::instance(), &AppSettings::changed, page, [updateSplitKeys](const QString &){updateSplitKeys();}); updateSplitKeys();
    for (auto *action : {splitVertical, splitHorizontal}) {
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        page->addAction(action);
    }
    connect(splitVertical, &QAction::triggered, this, [this, page] {
        activatePane(page); splitActivePane(Qt::Horizontal);
    });
    connect(splitHorizontal, &QAction::triggered, this, [this, page] {
        activatePane(page); splitActivePane(Qt::Vertical);
    });
    menu->addSeparator();
    auto *fileAction = menu->addAction(QStringLiteral("显示 / 隐藏文件管理"));
    page->addAction(fileAction); fileAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    const auto updateFileKey = [fileAction] {fileAction->setShortcut(QKeySequence(QSettings().value("shortcuts/files","Ctrl+Shift+F").toString()));};
    connect(AppSettings::instance(), &AppSettings::changed, page, [updateFileKey](const QString &){updateFileKey();}); updateFileKey();
    connect(fileAction, &QAction::triggered, this, [this, page] {
        activatePane(page);
        setPaneFileWorkspaceVisible(page, !page->property("fileWorkspaceVisible").toBool());
    });
    auto *disconnectAction = menu->addAction(QStringLiteral("断开当前分屏"));
    connect(disconnectAction, &QAction::triggered, session, &SshSession::disconnectFromHost);
    more->setMenu(menu);
    headerLayout->addWidget(more);
    auto *close = new QToolButton;
    close->setObjectName(QStringLiteral("terminalPaneCloseButton"));
    close->setText(QStringLiteral("×"));
    close->setToolTip(QStringLiteral("关闭当前分屏"));
    close->setFixedSize(22, 22);
    connect(close, &QToolButton::clicked, this, [this, page] { closePane(page); });
    headerLayout->addWidget(close);
    // The session controls belong to the command column, not a shared row
    // above the editor. The editor can then use the entire pane height.
    commandLayout->addWidget(header);
    commandLayout->addWidget(terminalFileSplitter, 1);
    layout->addWidget(paneEditorSplitter, 1);
    connect(panel, &TerminalPanel::commandSubmitted, this, [this, page, session, files](const QString &command) {
        const auto profile = page->property("serverProfile").value<ServerProfile>();
        if (!profile.id.isEmpty()) emit commandSubmitted(profile.id, command);
        emit sessionCommandSubmitted(session, command);
        QTimer::singleShot(180, files, [files, command] { files->syncDirectoryFromTerminalCommand(command); });
    });
    connect(panel, &TerminalPanel::fileWorkspaceToggleRequested, this, [this, page] {
        activatePane(page);
        setPaneFileWorkspaceVisible(page, !page->property("fileWorkspaceVisible").toBool());
    });
    connect(session, &SshSession::credentialReferenceChanged, this, [page, files](const ServerProfile &saved) {
        auto profile = page->property("serverProfile").value<ServerProfile>();
        profile.credentialRef = saved.credentialRef;
        page->setProperty("serverProfile", QVariant::fromValue(profile));
        files->updateServer(profile);
    });
    connect(session, &SshSession::connectionChanged, this, [this, page](bool connected, const QString &message) {
        const auto profile = page->property("serverProfile").value<ServerProfile>();
        const auto phase = connected ? TabConnectionPhase::Connected
                                     : (isConnectionProgress(message) ? TabConnectionPhase::Connecting
                                                                      : TabConnectionPhase::Disconnected);
        page->setProperty("terminalConnectionPhase", static_cast<int>(phase));
        page->setProperty("connectionMessage", message);
        updateTabPresentation(page->property("terminalGroupPage").value<QWidget *>());
        if (connected && m_repository) {
            m_repository->recordSuccessfulLogin(profile.id);
            refreshRecentLogins();
        }
        if (!profile.id.isEmpty()) emit sessionConnectionChanged(profile.id, connected, message);
        if(page->property("terminalGroupPage").value<QWidget *>()==m_stack->currentWidget())publishMonitorSession();
    });
    connect(session, &SshSession::hostKeyVerificationRequired, this,
        [this, session](const QString &target, const QString &fingerprint, const QString &algorithm) {
            const auto answer = QMessageBox::warning(this, QStringLiteral("确认未知主机指纹"),
                QStringLiteral("首次连接到 %1。\n\n算法：%2\nSHA-256 指纹：\n%3\n\n仅在已通过可信渠道核对后接受。")
                    .arg(target, algorithm, fingerprint),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            session->approveHostKey(answer == QMessageBox::Yes);
        });

    for (auto *widget : page->findChildren<QWidget *>()) widget->installEventFilter(this);
    page->installEventFilter(this);
    panel->setServer(profile);
    return page;
}

void TerminalWorkspace::addSession(const ServerProfile &profile, bool activate, bool persist, bool connectNow)
{
    auto *page = new QWidget;
    page->setProperty("serverProfile", QVariant::fromValue(profile));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *root = new QSplitter(Qt::Horizontal);
    root->setObjectName(QStringLiteral("terminalPaneSplitter"));
    root->setChildrenCollapsible(false);
    root->setHandleWidth(5);
    layout->addWidget(root);
    auto *pane = createPane(page, profile, true);
    root->addWidget(pane);
    page->setProperty("activeTerminalPane", QVariant::fromValue(pane));
    const int previousIndex = m_tabs->currentIndex();
    int index = -1;
    {
        const QSignalBlocker blocker(m_tabs);
        index = m_tabs->addTab(compactTabTitle(profile.name));
        m_tabs->setTabData(index, profile.id);
        m_tabs->setTabIcon(index, tabConnectionIcon(TabConnectionPhase::Disconnected));
        m_tabs->setTabToolTip(index, QStringLiteral("%1 · 未连接 · %2@%3:%4")
                .arg(profile.name, profile.user, profile.host).arg(profile.port));
        installCloseButton(index);
        m_stack->addWidget(page);
        if (activate) m_tabs->setCurrentIndex(index);
    }
    if (activate || previousIndex < 0) {
        m_stack->setCurrentIndex(m_tabs->currentIndex());
        m_selectedPages = {page}; m_selectionAnchor = page; updateTabSelection();
        publishActiveSession();
    }
    updateWorkspaceState();
    emit sessionCountChanged(sessionCount());
    emit tabsChanged();
    if (persist) persistState();
    // Finish exposing the tab before a native credential dialog can block.
    if (connectNow) pane->findChild<TerminalPanel *>()->connectToServer(profile);
    updateTabPresentation(page);
}

void TerminalWorkspace::layoutPanes(QWidget *page, const QList<QWidget *> &items, int mode)
{
    // The pane (and all its SSH/editor/Agent children) is moved as one unit.
    // Never destroy or recreate a session while changing presentation.
    for (auto *pane : items) { pane->setParent(page); pane->setProperty("terminalGroupPage", QVariant::fromValue(page)); }
    while (auto *item = page->layout()->takeAt(0)) { delete item->widget(); delete item; }
    auto *root = new QSplitter(mode == 2 || (mode == 0 && items.size() > 2) ? Qt::Vertical : Qt::Horizontal);
    root->setObjectName(QStringLiteral("terminalPaneSplitter"));
    root->setChildrenCollapsible(false); root->setHandleWidth(5); page->layout()->addWidget(root);
    const auto add = [](QSplitter *split, QWidget *pane) { split->addWidget(pane); pane->show(); };
    if (mode != 0 || items.size() <= 2) {
        for (auto *pane : items) add(root, pane);
    } else {
        for (int i = 0; i < items.size(); i += 2) {
            if (i + 1 == items.size()) { add(root, items[i]); break; }
            auto *row = new QSplitter(Qt::Horizontal);
            row->setObjectName(QStringLiteral("terminalPaneSplitter")); row->setChildrenCollapsible(false); row->setHandleWidth(5);
            root->addWidget(row); add(row, items[i]); add(row, items[i + 1]); row->setSizes({500, 500});
        }
    }
    root->setSizes(QList<int>(root->count(), 500));
    page->setProperty("paneLayoutMode", mode);
}

void TerminalWorkspace::mergeSelectedSessions()
{
    if (m_confirmingClose) return;
    QList<QWidget *> pages, items;
    for (int i = 0; i < m_stack->count(); ++i) {
        auto *page = m_stack->widget(i);
        if (m_selectedPages.contains(page) && !panes(page).isEmpty()) { pages.append(page); items.append(panes(page)); }
    }
    if (pages.size() < 2 || items.size() > 8) return;
    restoreExpandedFileWorkspace();
    auto *target = pages.first();
    auto *focused = currentPane();
    if (!items.contains(focused)) focused = items.first();
    TerminalGridResizePause pause(this, target);
    for (auto *page : pages.mid(1)) for (auto *view : page->findChildren<TerminalView *>()) pause.add(view);
    {
        const QSignalBlocker blocker(m_tabs);
        // Detach before deleting the now-empty source pages or splitter trees.
        for (auto *pane : items) pane->setParent(target);
        for (auto *page : pages.mid(1)) {
            const int index = m_stack->indexOf(page);
            m_tabs->removeTab(index); m_stack->removeWidget(page); page->deleteLater();
        }
        layoutPanes(target, items, 0);
        target->setProperty("activeTerminalPane", QVariant::fromValue(focused));
        m_tabs->setCurrentIndex(m_stack->indexOf(target)); m_stack->setCurrentWidget(target);
    }
    for (auto *pane : items) setPaneFileWorkspaceVisible(pane, false);
    m_selectedPages = {target}; m_selectionAnchor = target;
    updateTabSelection(); activatePane(focused); publishActiveSession(); updateWorkspaceState();
    emit sessionCountChanged(sessionCount()); emit splitLayoutChanged(items.size()); persistState();
}

QWidget *TerminalWorkspace::appendPaneTab(QWidget *pane)
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    layoutPanes(page, {pane}, 0); page->setProperty("activeTerminalPane", QVariant::fromValue(pane));
    const int index = m_tabs->addTab(QString{}); m_stack->addWidget(page); installCloseButton(index);
    updateTabPresentation(page); return page;
}

void TerminalWorkspace::unmergeActiveSession()
{
    auto *page = m_stack->currentWidget(); const auto items = panes(page);
    if (items.size() <= 1 || m_confirmingClose) return;
    restoreExpandedFileWorkspace();
    auto *focused = activePane(page); QWidget *focusPage = page;
    TerminalGridResizePause pause(this, page);
    {
        const QSignalBlocker blocker(m_tabs);
        for (auto *pane : items) pane->setParent(page);
        for (auto *pane : items.mid(1)) {
            auto *created = appendPaneTab(pane); if (pane == focused) focusPage = created;
        }
        layoutPanes(page, {items.first()}, 0); page->setProperty("activeTerminalPane", QVariant::fromValue(items.first()));
        m_tabs->setCurrentIndex(m_stack->indexOf(focusPage)); m_stack->setCurrentWidget(focusPage);
    }
    m_selectedPages = {focusPage}; m_selectionAnchor = focusPage;
    updateTabSelection(); updateTabPresentation(page); publishActiveSession(); updateWorkspaceState();
    emit sessionCountChanged(sessionCount()); emit splitLayoutChanged(1); persistState();
}

void TerminalWorkspace::arrangeActivePanes(int mode)
{
    auto *page = m_stack->currentWidget(); const auto items = panes(page);
    if (items.size() <= 1 || m_confirmingClose || mode < 0 || mode > 2) return;
    restoreExpandedFileWorkspace(); TerminalGridResizePause pause(this, page);
    layoutPanes(page, items, mode); activatePane(activePane(page));
}

void TerminalWorkspace::splitActivePane(Qt::Orientation orientation)
{
    restoreExpandedFileWorkspace();
    auto *page = m_stack->currentWidget();
    auto *source = activePane(page);
    if (!source || isHomePageVisible() || m_confirmingClose || panes(page).size() >= 8) return;
    auto *parent = qobject_cast<QSplitter *>(source->parentWidget());
    if (!parent) return;
    TerminalGridResizePause resizePause(this, page);
    const auto next = source->property("nextConnectionProfile");
    const auto profile = (next.isValid() ? next : source->property("serverProfile")).value<ServerProfile>();
    const int index = parent->indexOf(source);
    auto *splitter = new QSplitter(orientation);
    splitter->setObjectName(QStringLiteral("terminalPaneSplitter"));
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(5);
    parent->replaceWidget(index, splitter);
    splitter->addWidget(source);
    auto *created = createPane(page, profile, false);
    resizePause.add(created->findChild<TerminalView *>());
    splitter->addWidget(created);
    splitter->setSizes({500, 500});
    // Every newly split workspace starts with more room for its terminals.
    // Each pane can opt into its own file drawer afterwards.
    for (auto *pane : panes(page)) {
        setPaneFileWorkspaceVisible(pane, false);
    }
    activatePane(created, true);
    emit splitLayoutChanged(panes(page).size());
    created->findChild<TerminalPanel *>()->connectToServer(profile);
}

void TerminalWorkspace::closePane(QWidget *pane)
{
    auto *page = pane ? pane->property("terminalGroupPage").value<QWidget *>() : nullptr;
    if (!page) return;
    if (panes(page).size() <= 1) closeSession(m_stack->indexOf(page));
    else requestCloseSessions({pane}, QStringLiteral("确认关闭分屏"));
}

void TerminalWorkspace::removePane(QWidget *pane)
{
    restoreExpandedFileWorkspace();
    auto *page = pane->property("terminalGroupPage").value<QWidget *>();
    auto *parent = qobject_cast<QSplitter *>(pane->parentWidget());
    if (!page || !parent) return;
    if (panes(page).size() <= 1) { removeSession(m_stack->indexOf(page)); return; }
    TerminalGridResizePause resizePause(this, page);
    const bool wasActive = activePane(page) == pane;
    pane->setParent(nullptr);
    if (auto *session = pane->findChild<SshSession *>()) {
        session->disconnectFromHost();
        emit sessionClosed(session);
    }
    pane->deleteLater();
    // Flatten emptied branches while preserving the surviving terminal widget
    // and its live SSH channel; closing a pane must never reconnect siblings.
    while (parent && parent->count() == 1) {
        auto *outer = qobject_cast<QSplitter *>(parent->parentWidget());
        if (!outer) break;
        const int index = outer->indexOf(parent);
        auto *survivor = parent->widget(0);
        survivor->setParent(nullptr);
        outer->replaceWidget(index, survivor);
        parent->deleteLater();
        parent = outer;
    }
    if (wasActive) {
        const auto remaining = panes(page);
        page->setProperty("activeTerminalPane", QVariant::fromValue(remaining.first()));
        activatePane(remaining.first(), true);
        publishActiveSession();
    }
    updateTabPresentation(page);
    emit splitLayoutChanged(panes(page).size());
    publishMonitorSession();
}

void TerminalWorkspace::installCloseButton(int index)
{
    if (index < 0 || index >= m_tabs->count()) return;
    auto *nativeButton = m_tabs->tabButton(index, QTabBar::RightSide);
    auto *container = new QWidget(m_tabs);
    container->setObjectName(QStringLiteral("terminalTabCloseContainer"));
    container->setFixedSize(27, 22);
    auto *containerLayout = new QHBoxLayout(container);
    containerLayout->setContentsMargins(2, 0, 6, 0);
    containerLayout->setSpacing(0);
    auto *closeButton = new QToolButton(container);
    closeButton->setObjectName(QStringLiteral("terminalTabCloseButton"));
    closeButton->setText(QString::fromUtf8("×"));
    closeButton->setToolTip(QStringLiteral("关闭标签"));
    closeButton->setFixedSize(19, 19);
    containerLayout->addWidget(closeButton);
    m_tabs->setTabButton(index, QTabBar::RightSide, container);
    if (nativeButton && nativeButton != container) nativeButton->deleteLater();
    connect(closeButton, &QToolButton::clicked, this, [this, container] {
        for (int candidate = 0; candidate < m_tabs->count(); ++candidate) {
            if (m_tabs->tabButton(candidate, QTabBar::RightSide) != container) continue;
            closeSession(candidate);
            return;
        }
    });
}

void TerminalWorkspace::closeSession(int index)
{
    if (index < 0 || index >= m_stack->count()) return;
    if (m_stack->widget(index)->property("settingsTab").toBool() || !AppSettings::value("tabs/confirmClose").toBool()) { removeSession(index); return; }
    requestCloseSessions({m_stack->widget(index)}, QStringLiteral("确认关闭会话"));
}

void TerminalWorkspace::requestCloseSessions(const QList<QPointer<QWidget>> &pages, const QString &title)
{
    if (pages.isEmpty() || m_confirmingClose) return;
    if (!AppSettings::value("tabs/confirmClose").toBool()) {
        for (auto it = pages.crbegin(); it != pages.crend(); ++it) if (*it) {
            const int index = m_stack->indexOf(*it); if (index >= 0) removeSession(index); else removePane(*it);
        }
        return;
    }
    const QScopedValueRollback confirming(m_confirmingClose, true);
    QString text;
    if (pages.size() == 1 && pages.first()) {
        const auto profile = pages.first()->property("serverProfile").value<ServerProfile>();
        text = pages.first()->objectName() == QStringLiteral("terminalSplitPane")
            ? QStringLiteral("确认关闭“%1”的当前分屏？").arg(profile.name)
            : QStringLiteral("确认关闭“%1”的标签页及其所有分屏？").arg(profile.name);
    } else {
        text = QStringLiteral("确认关闭这 %1 个标签页？").arg(pages.size());
    }
    QMessageBox dialog(QMessageBox::Question, title, text, QMessageBox::Yes | QMessageBox::Cancel, this);
    dialog.setObjectName(QStringLiteral("terminalCloseConfirmation"));
    dialog.setTextFormat(Qt::PlainText);
    dialog.setInformativeText(QStringLiteral("关闭会断开对应 SSH 会话，并停止该会话中进行的文件传输。"));
    dialog.button(QMessageBox::Yes)->setText(QStringLiteral("关闭会话"));
    dialog.button(QMessageBox::Cancel)->setText(QStringLiteral("取消"));
    dialog.setDefaultButton(QMessageBox::Cancel);
    dialog.setEscapeButton(QMessageBox::Cancel);
    if (dialog.exec() != QMessageBox::Yes) return;
    // Track the pages, not their indices: nested events can change the tab list
    // while the dialog is open. New tabs must never join an earlier close request.
    for (auto iterator = pages.crbegin(); iterator != pages.crend(); ++iterator) {
        if (!*iterator) continue;
        const int index = m_stack->indexOf(*iterator);
        if (index >= 0) removeSession(index);
        else if ((*iterator)->objectName() == QStringLiteral("terminalSplitPane")) removePane(*iterator);
    }
}

void TerminalWorkspace::removeSession(int index)
{
    restoreExpandedFileWorkspace();
    if (index < 0 || index >= m_tabs->count()) return;
    auto *page = m_stack->widget(index);
    const auto profile = page->property("serverProfile").value<ServerProfile>();
    {
        const QSignalBlocker blocker(m_tabs);
        m_tabs->removeTab(index);
        m_stack->removeWidget(page);
    }
    if (!profile.id.isEmpty()) emit sessionConnectionChanged(profile.id, false, QStringLiteral("SSH 标签已关闭"));
    for (auto *pane : panes(page)) {
        if (auto *session = pane->findChild<SshSession *>()) {
            session->disconnectFromHost();
            emit sessionClosed(session);
        }
    }
    page->deleteLater();
    updateTabSelection();
    if (m_tabs->currentIndex() >= 0 && m_tabs->currentIndex() < m_stack->count()) {
        m_stack->setCurrentIndex(m_tabs->currentIndex());
        publishActiveSession();
    }
    updateWorkspaceState();
    emit sessionCountChanged(sessionCount());
    emit tabsChanged();
    persistState();
}

void TerminalWorkspace::closeOtherSessions(int index)
{
    if (index < 0 || index >= m_stack->count()) return;
    const QPointer<QWidget> keptPage = m_stack->widget(index);
    QList<QPointer<QWidget>> pages;
    for (int candidate = 0; candidate < m_stack->count(); ++candidate) {
        if (m_stack->widget(candidate) != keptPage) pages.append(m_stack->widget(candidate));
    }
    requestCloseSessions(pages, QStringLiteral("确认关闭其他会话"));
    if (!keptPage) return;
    const int keptIndex = m_stack->indexOf(keptPage);
    if (keptIndex >= 0) m_tabs->setCurrentIndex(keptIndex);
}

void TerminalWorkspace::closeAllSessions()
{
    QList<QPointer<QWidget>> pages;
    for (int index = 0; index < m_stack->count(); ++index) pages.append(m_stack->widget(index));
    requestCloseSessions(pages, QStringLiteral("确认关闭全部会话"));
}

void TerminalWorkspace::updateWorkspaceState()
{
    if (!m_viewStack) return;
    if (m_tabs->count() == 0) {
        if (m_tabToolbar) m_tabToolbar->hide();
        refreshRecentLogins();
        m_viewStack->setCurrentWidget(m_emptyPage);
    } else {
        if (m_tabToolbar) m_tabToolbar->setVisible(!m_nativeTitlebarTabs);
        m_viewStack->setCurrentWidget(m_sessionsPage);
    }
}

void TerminalWorkspace::refreshRecentLogins()
{
    if (!m_recentLogins || !m_recentEmptyLabel) return;
    m_recentLogins->clear();
    const auto entries = m_repository ? m_repository->loadRecentLogins(30) : QVector<LoginHistoryEntry>{};
    for (const auto &entry : entries) {
        auto *item = new QTreeWidgetItem(m_recentLogins, {
            entry.serverName,
            QStringLiteral("%1:%2").arg(entry.host).arg(entry.port),
            entry.user,
            entry.connectedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
        });
        item->setData(0, Qt::UserRole, entry.serverId);
        item->setToolTip(0, QStringLiteral("双击连接 %1").arg(entry.serverName));
    }
    const bool empty = entries.isEmpty();
    m_recentLogins->setVisible(!empty);
    m_recentEmptyLabel->setVisible(empty);
}

void TerminalWorkspace::persistState()
{
    if (m_repository) {
        const auto ids = serverIds();
        m_repository->saveTerminalState(ids, qMax(0, ids.indexOf(m_tabs->tabData(m_tabs->currentIndex()).toString())));
    }
}

void TerminalWorkspace::publishActiveSession()
{
    auto *pane = currentPane();
    if (!pane) return;
    const auto profile = pane->property("serverProfile").value<ServerProfile>();
    auto *session = pane->findChild<SshSession *>();
    if (auto *files = pane->findChild<FilePanel *>()) files->updateServer(profile);
    updateTabPresentation(m_stack->currentWidget());
    if (!profile.id.isEmpty() && session) emit activeSessionChanged(profile, session);
    publishMonitorSession();
}

void TerminalWorkspace::publishMonitorSession()
{
    auto *group=m_stack->currentWidget();
    const auto items=panes(group);
    if(items.isEmpty())return;
    auto *pinned=group->property("monitorTerminalPane").value<QWidget *>();
    if(!items.contains(pinned)){
        pinned=items.first();
        const auto previous=group->property("monitorEndpoint").toStringList();
        for(auto *candidate:items){
            if(monitorEndpoint(candidate->property("serverProfile").value<ServerProfile>())!=previous)continue;
            pinned=candidate;
            if(auto *source=candidate->findChild<SshSession *>();source && source->isConnected())break;
        }
    }
    const auto profile=pinned->property("serverProfile").value<ServerProfile>();
    auto *session=pinned->findChild<SshSession *>();
    // Focus does not choose a monitor source. Only hand over after the pinned
    // channel is lost, and only to a connected channel for the same endpoint.
    if(!session || !session->isConnected())for(auto *candidate:items){
        const auto next=candidate->property("serverProfile").value<ServerProfile>();
        auto *source=candidate->findChild<SshSession *>();
        if(source && source->isConnected() && monitorEndpoint(next)==monitorEndpoint(profile)){
            pinned=candidate;session=source;break;
        }
    }
    group->setProperty("monitorTerminalPane",QVariant::fromValue(pinned));
    group->setProperty("monitorEndpoint",monitorEndpoint(pinned->property("serverProfile").value<ServerProfile>()));
    if(session)emit monitorSessionChanged(group,pinned->property("serverProfile").value<ServerProfile>(),session);
    updateLatencyProbes();
}

void TerminalWorkspace::updateLatencyProbes(bool refreshConfiguration)
{
    if (!m_stack || !m_viewStack) return;
    auto *group = isHomePageVisible() ? nullptr : m_stack->currentWidget();
    for (auto *probe : findChildren<TcpLatencyProbe *>()) {
        if (refreshConfiguration) probe->setProperty("latencyConfiguration", QVariant{});
        if (probe->parent() != group) probe->setActive(false);
    }
    if (!group) {
        emit latencyChanged(nullptr, {});
        return;
    }
    auto *pane = group->property("monitorTerminalPane").value<QWidget *>();
    // A closed monitor pane can remain in the property until source handover.
    if (!panes(group).contains(pane)) pane = nullptr;
    auto *session = pane ? pane->findChild<SshSession *>() : nullptr;
    const auto profile = pane ? pane->property("serverProfile").value<ServerProfile>() : ServerProfile{};
    auto *probe = group->findChild<TcpLatencyProbe *>(QString{}, Qt::FindDirectChildrenOnly);
    const bool enabled = AppSettings::value("general/showLatency").toBool()
        && session && session->isConnected() && profile.connectionMode == ConnectionMode::Ssh;
    if (!enabled) {
        if (probe) probe->setActive(false);
        emit latencyChanged(group, {});
        return;
    }
    if (!probe) {
        probe = new TcpLatencyProbe(group);
        connect(probe, &TcpLatencyProbe::updated, this, [this, group, probe](const QString &text) {
            if (probe->isActive()) emit latencyChanged(group, text);
        });
    }
    auto proxy = AppSettings::proxyFor(profile.host, profile.port);
    const auto reference = AppSettings::value("proxy/credentialRef").toString();
    const QStringList configuration{profile.host, QString::number(profile.port),
        QString::number(proxy.type()), proxy.hostName(), QString::number(proxy.port()),
        proxy.user(), reference, AppSettings::value("proxy/mode").toString()};
    if (refreshConfiguration || probe->property("latencyConfiguration").toStringList() != configuration) {
        probe->setActive(false);
        if (AppSettings::value("proxy/mode") == "manual" && m_credentialStore && !reference.isEmpty())
            proxy.setPassword(m_credentialStore->load(reference).password);
        probe->setTarget(profile.host, profile.port, proxy);
        probe->setProperty("latencyConfiguration", configuration);
    }
    probe->setActive(true);
    emit latencyChanged(group, probe->text());
}

QStringList TerminalWorkspace::serverIds() const
{
    QStringList result;
    for (int index = 0; index < m_stack->count(); ++index)
        for (auto *pane : panes(m_stack->widget(index))) result.append(pane->property("serverProfile").value<ServerProfile>().id);
    return result;
}

} // namespace noxshell::ui
