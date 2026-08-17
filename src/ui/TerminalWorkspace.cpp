#include "TerminalWorkspace.h"

#include "../core/CredentialStore.h"
#include "../core/ServerRepository.h"
#include "../core/SshSession.h"
#include "TerminalPanel.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QGuiApplication>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace noxshell::ui {

namespace {
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
    emptyLayout->setContentsMargins(34, 26, 34, 28);
    emptyLayout->setSpacing(8);
    auto *recentTitle = new QLabel(QStringLiteral("最近登录"));
    recentTitle->setObjectName(QStringLiteral("recentLoginTitle"));
    auto *recentHint = new QLabel(QStringLiteral("尚未建立 SSH 会话 · 双击记录可重新连接"));
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
    m_recentEmptyLabel = new QLabel(QStringLiteral("暂无成功登录记录\n双击左侧主机即可开始连接"));
    m_recentEmptyLabel->setObjectName(QStringLiteral("recentLoginEmpty"));
    m_recentEmptyLabel->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(recentTitle);
    emptyLayout->addWidget(recentHint);
    emptyLayout->addSpacing(6);
    emptyLayout->addWidget(m_recentLogins, 1);
    emptyLayout->addWidget(m_recentEmptyLabel, 1);

    m_sessionsPage = new QWidget;
    m_sessionsPage->setObjectName(QStringLiteral("terminalSessionsPage"));
    auto *sessionsLayout = new QVBoxLayout(m_sessionsPage);
    sessionsLayout->setContentsMargins(0, 0, 0, 0);
    sessionsLayout->setSpacing(0);

    auto *toolbar = new QWidget;
    toolbar->setObjectName(QStringLiteral("terminalTabToolbar"));
    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(8, 5, 7, 5);
    toolbarLayout->setSpacing(6);
    m_tabs = new QTabBar;
    m_tabs->setObjectName(QStringLiteral("terminalSessionTabs"));
    // Cocoa's native tab style can report a near-zero height for a standalone
    // QTabBar. Keep the session strip explicit so its labels do not collapse
    // while the surrounding toolbar remains visible as an empty row.
    m_tabs->setFixedHeight(32);
    m_tabs->setExpanding(false);
    m_tabs->setTabsClosable(true);
    m_tabs->setElideMode(Qt::ElideRight);
    m_tabs->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tabMenu = new QMenu(this);
    m_connectAction = m_tabMenu->addAction(QStringLiteral("连接"));
    m_disconnectAction = m_tabMenu->addAction(QStringLiteral("断开连接"));
    m_connectAction->setObjectName(QStringLiteral("terminalConnectAction"));
    m_disconnectAction->setObjectName(QStringLiteral("terminalDisconnectAction"));
    m_tabMenu->addSeparator();
    auto *duplicateAction = m_tabMenu->addAction(QStringLiteral("复制会话"));
    duplicateAction->setObjectName(QStringLiteral("terminalDuplicateAction"));
    m_tabMenu->addSeparator();
    auto *closeCurrentAction = m_tabMenu->addAction(QStringLiteral("关闭当前"));
    m_closeOthersAction = m_tabMenu->addAction(QStringLiteral("关闭其他"));
    auto *closeAllAction = m_tabMenu->addAction(QStringLiteral("关闭全部"));
    closeCurrentAction->setObjectName(QStringLiteral("terminalCloseCurrentAction"));
    m_closeOthersAction->setObjectName(QStringLiteral("terminalCloseOthersAction"));
    closeAllAction->setObjectName(QStringLiteral("terminalCloseAllAction"));
    auto *clearButton = new QPushButton(QStringLiteral("清屏"));
    clearButton->setObjectName(QStringLiteral("clearTerminalButton"));
    clearButton->setToolTip(QStringLiteral("清空当前会话的本地终端显示"));
    toolbarLayout->addWidget(m_tabs, 1);
    toolbarLayout->addWidget(clearButton);
    sessionsLayout->addWidget(toolbar);

    m_stack = new QStackedWidget;
    sessionsLayout->addWidget(m_stack, 1);
    m_viewStack->addWidget(m_emptyPage);
    m_viewStack->addWidget(m_sessionsPage);
    layout->addWidget(m_viewStack, 1);

    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && index < m_stack->count()) m_stack->setCurrentIndex(index);
        publishActiveSession();
        persistState();
    });
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
        auto *page = m_stack->widget(m_tabContextIndex);
        const auto profile = page->property("serverProfile").value<ServerProfile>();
        if (profile.id.isEmpty()) return;
        if (auto *panel = page->findChild<TerminalPanel *>()) panel->connectToServer(profile);
    });
    connect(m_disconnectAction, &QAction::triggered, this, [this] {
        if (m_tabContextIndex < 0 || m_tabContextIndex >= m_stack->count()) return;
        if (auto *session = m_stack->widget(m_tabContextIndex)->findChild<SshSession *>()) {
            session->disconnectFromHost();
        }
    });
    connect(duplicateAction, &QAction::triggered, this, [this] { duplicateSessionAt(m_tabContextIndex); });
    connect(closeCurrentAction, &QAction::triggered, this, [this] { closeSession(m_tabContextIndex); });
    connect(m_closeOthersAction, &QAction::triggered, this, [this] { closeOtherSessions(m_tabContextIndex); });
    connect(closeAllAction, &QAction::triggered, this, &TerminalWorkspace::closeAllSessions);
    connect(clearButton, &QPushButton::clicked, this, [this] {
        const int index = m_tabs->currentIndex();
        if (index < 0 || index >= m_stack->count()) return;
        if (auto *panel = m_stack->widget(index)->findChild<TerminalPanel *>()) panel->clearTerminal();
    });
    connect(m_recentLogins, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (!item || !m_repository) return;
        const auto serverId = item->data(0, Qt::UserRole).toString();
        const auto servers = m_repository->loadServers();
        const auto found = std::find_if(servers.cbegin(), servers.cend(), [&serverId](const ServerProfile &profile) {
            return profile.id == serverId;
        });
        if (found != servers.cend()) openOrActivate(*found, true);
    });

    refreshRecentLogins();
    updateWorkspaceState();
}

bool TerminalWorkspace::prepareTabContextMenu(int index)
{
    if (index < 0 || index >= m_tabs->count() || index >= m_stack->count()) return false;
    m_tabContextIndex = index;
    m_tabs->setCurrentIndex(index);
    const auto phase = static_cast<TabConnectionPhase>(
        m_stack->widget(index)->property("terminalConnectionPhase").toInt());
    m_connectAction->setEnabled(phase == TabConnectionPhase::Disconnected);
    m_disconnectAction->setEnabled(phase != TabConnectionPhase::Disconnected);
    m_closeOthersAction->setEnabled(m_tabs->count() > 1);
    return true;
}

int TerminalWorkspace::sessionCount() const
{
    return m_tabs->count();
}

bool TerminalWorkspace::hasConnectedSession(const QString &serverId) const
{
    for (int index = 0; index < m_tabs->count(); ++index) {
        if (m_tabs->tabData(index).toString() != serverId) continue;
        if (const auto *panel = m_stack->widget(index)->findChild<TerminalPanel *>(); panel && panel->isConnected()) {
            return true;
        }
    }
    return false;
}

bool TerminalWorkspace::activateExisting(const QString &serverId)
{
    int fallbackIndex = -1;
    for (int index = m_tabs->count() - 1; index >= 0; --index) {
        if (m_tabs->tabData(index).toString() != serverId) continue;
        if (fallbackIndex < 0) fallbackIndex = index;
        if (!m_stack->widget(index)->property("serverConfigurationCurrent").toBool()) continue;
        fallbackIndex = index;
        break;
    }
    if (fallbackIndex < 0) return false;
    const bool indexChanged = m_tabs->currentIndex() != fallbackIndex;
    m_tabs->setCurrentIndex(fallbackIndex);
    if (!indexChanged) publishActiveSession();
    return true;
}

void TerminalWorkspace::openOrActivate(const ServerProfile &profile, bool connectNow)
{
    for (int index = 0; index < m_tabs->count(); ++index) {
        if (m_tabs->tabData(index).toString() != profile.id) continue;
        auto *page = m_stack->widget(index);
        if (!page->property("serverConfigurationCurrent").toBool()) continue;
        const bool indexChanged = m_tabs->currentIndex() != index;
        m_tabs->setCurrentIndex(index);
        if (!indexChanged) publishActiveSession();
        if (connectNow) {
            if (auto *panel = page->findChild<TerminalPanel *>()) panel->connectToServer(profile);
        }
        return;
    }
    addSession(profile, true, true, connectNow);
}

void TerminalWorkspace::duplicateSession(const ServerProfile &profile)
{
    addSession(profile, true, true, true);
}

void TerminalWorkspace::duplicateSessionAt(int index)
{
    if (index < 0 || index >= m_stack->count()) return;
    const auto profile = m_stack->widget(index)->property("serverProfile").value<ServerProfile>();
    if (!profile.id.isEmpty()) duplicateSession(profile);
}

void TerminalWorkspace::updateServer(const ServerProfile &profile)
{
    for (int index = 0; index < m_tabs->count(); ++index) {
        if (m_tabs->tabData(index).toString() != profile.id) continue;
        // A running terminal is an immutable connection snapshot. Editing the
        // saved host must not rewrite, clear or reconnect that existing SSH
        // channel. The next explicit Connect action creates a new tab using
        // the newly saved profile.
        auto *page = m_stack->widget(index);
        page->setProperty("serverConfigurationCurrent", false);
    }
}

void TerminalWorkspace::closeServer(const QString &serverId)
{
    for (int index = m_tabs->count() - 1; index >= 0; --index) {
        if (m_tabs->tabData(index).toString() == serverId) closeSession(index);
    }
}

void TerminalWorkspace::addSession(const ServerProfile &profile, bool activate, bool persist, bool connectNow)
{
    auto *page = new QWidget;
    page->setProperty("serverProfile", QVariant::fromValue(profile));
    page->setProperty("serverConfigurationCurrent", true);
    page->setProperty("terminalConnectionPhase", static_cast<int>(TabConnectionPhase::Disconnected));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *session = new SshSession(m_repository, m_credentialStore, page);
    session->setTransferPersistenceEnabled(false);
    auto *panel = new TerminalPanel(session);
    layout->addWidget(panel);
    connect(panel, &TerminalPanel::commandSubmitted, this, [this, page](const QString &command) {
        const auto profile = page->property("serverProfile").value<ServerProfile>();
        if (!profile.id.isEmpty()) emit commandSubmitted(profile.id, command);
    });
    connect(session, &SshSession::connectionChanged, this, [this, page](bool connected, const QString &message) {
        const auto profile = page->property("serverProfile").value<ServerProfile>();
        const auto phase = connected ? TabConnectionPhase::Connected
                                     : (isConnectionProgress(message) ? TabConnectionPhase::Connecting
                                                                      : TabConnectionPhase::Disconnected);
        page->setProperty("terminalConnectionPhase", static_cast<int>(phase));
        const int tabIndex = m_stack->indexOf(page);
        if (tabIndex >= 0) {
            m_tabs->setTabIcon(tabIndex, tabConnectionIcon(phase));
            m_tabs->setTabToolTip(tabIndex, QStringLiteral("%1 · %2@%3:%4%5")
                    .arg(phaseText(phase), profile.user, profile.host)
                    .arg(profile.port)
                    .arg(message.isEmpty() ? QString{} : QStringLiteral(" · %1").arg(message)));
        }
        if (connected && m_repository) {
            m_repository->recordSuccessfulLogin(profile.id);
            refreshRecentLogins();
        }
        if (!profile.id.isEmpty()) emit sessionConnectionChanged(profile.id, connected, message);
    });
    connect(session, &SshSession::hostKeyVerificationRequired, this,
        [this, session](const QString &target, const QString &fingerprint, const QString &algorithm) {
            const auto answer = QMessageBox::warning(this, QStringLiteral("确认未知主机指纹"),
                QStringLiteral("首次连接到 %1。\n\n算法：%2\nSHA-256 指纹：\n%3\n\n仅在已通过可信渠道核对后接受。")
                    .arg(target, algorithm, fingerprint),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            session->approveHostKey(answer == QMessageBox::Yes);
        });

    const int previousIndex = m_tabs->currentIndex();
    int index = -1;
    {
        const QSignalBlocker blocker(m_tabs);
        index = m_tabs->addTab(profile.name);
        m_tabs->setTabData(index, profile.id);
        m_tabs->setTabIcon(index, tabConnectionIcon(TabConnectionPhase::Disconnected));
        m_tabs->setTabToolTip(index, QStringLiteral("未连接 · %1@%2:%3").arg(profile.user, profile.host).arg(profile.port));
        m_stack->addWidget(page);
        if (activate) m_tabs->setCurrentIndex(index);
    }
    panel->setServer(profile);
    if (activate || previousIndex < 0) {
        m_stack->setCurrentIndex(m_tabs->currentIndex());
        publishActiveSession();
    }
    if (connectNow) panel->connectToServer(profile);
    updateWorkspaceState();
    emit sessionCountChanged(m_tabs->count());
    if (persist) persistState();
}

void TerminalWorkspace::closeSession(int index)
{
    if (index < 0 || index >= m_tabs->count()) return;
    auto *page = m_stack->widget(index);
    auto *session = page->findChild<SshSession *>();
    const auto profile = page->property("serverProfile").value<ServerProfile>();
    {
        const QSignalBlocker blocker(m_tabs);
        m_tabs->removeTab(index);
        m_stack->removeWidget(page);
    }
    if (!profile.id.isEmpty()) emit sessionConnectionChanged(profile.id, false, QStringLiteral("SSH 标签已关闭"));
    if (session) emit sessionClosed(session);
    page->deleteLater();
    if (m_tabs->currentIndex() >= 0 && m_tabs->currentIndex() < m_stack->count()) {
        m_stack->setCurrentIndex(m_tabs->currentIndex());
        publishActiveSession();
    }
    updateWorkspaceState();
    emit sessionCountChanged(m_tabs->count());
    persistState();
}

void TerminalWorkspace::closeOtherSessions(int index)
{
    if (index < 0 || index >= m_stack->count()) return;
    auto *keptPage = m_stack->widget(index);
    for (int candidate = m_stack->count() - 1; candidate >= 0; --candidate) {
        if (m_stack->widget(candidate) != keptPage) closeSession(candidate);
    }
    const int keptIndex = m_stack->indexOf(keptPage);
    if (keptIndex >= 0) m_tabs->setCurrentIndex(keptIndex);
}

void TerminalWorkspace::closeAllSessions()
{
    for (int index = m_tabs->count() - 1; index >= 0; --index) closeSession(index);
}

void TerminalWorkspace::updateWorkspaceState()
{
    if (!m_viewStack) return;
    if (m_tabs->count() == 0) {
        refreshRecentLogins();
        m_viewStack->setCurrentWidget(m_emptyPage);
    } else {
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
    if (m_repository) m_repository->saveTerminalState(serverIds(), m_tabs->currentIndex());
}

void TerminalWorkspace::publishActiveSession()
{
    const int index = m_tabs->currentIndex();
    if (index < 0 || index >= m_stack->count()) return;
    auto *page = m_stack->widget(index);
    const auto profile = page->property("serverProfile").value<ServerProfile>();
    auto *session = page->findChild<SshSession *>();
    if (!profile.id.isEmpty() && session) emit activeSessionChanged(profile, session);
}

QStringList TerminalWorkspace::serverIds() const
{
    QStringList result;
    for (int index = 0; index < m_tabs->count(); ++index) result.append(m_tabs->tabData(index).toString());
    return result;
}

} // namespace noxshell::ui
