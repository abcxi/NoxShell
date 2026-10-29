#pragma once

#include "../core/ServerProfile.h"

#include <QWidget>
#include <QPointer>
#include <QList>
#include <QByteArray>
#include <QPair>

class QStackedWidget;
class QTabBar;
class QTabWidget;
class QTreeWidget;
class QLabel;
class QAction;
class QMenu;
class QSplitter;

namespace noxshell {
class CredentialStore;
class ServerRepository;
class SshSession;
}

namespace noxshell::ui {

class TerminalView;

class TerminalWorkspace final : public QWidget {
    Q_OBJECT

public:
    explicit TerminalWorkspace(ServerRepository *repository, CredentialStore *credentialStore,
        QWidget *parent = nullptr);

    [[nodiscard]] int sessionCount() const;
    [[nodiscard]] int activePaneCount() const;
    [[nodiscard]] bool fileWorkspaceVisible() const;
    QWidget *tabToolbar() const { return m_tabToolbar; }
    QTabBar *sessionTabs() const { return m_tabs; }
    void setNativeTitlebarTabs(bool enabled);
    void splitActivePane(Qt::Orientation orientation);
    // selectionMode: 0 = single, 1 = toggle, 2 = range (also used by Cocoa tabs).
    Q_INVOKABLE void selectSessionTab(int index, int selectionMode = 0);
    void mergeSelectedSessions();
    void unmergeActiveSession();
    void arrangeActivePanes(int layout = 0); // 0 = grid, 1 = left/right, 2 = top/bottom
    [[nodiscard]] bool hasConnectedSession(const QString &serverId) const;
    [[nodiscard]] bool hasConnectingSession(const QString &serverId) const;
    bool activateExisting(const QString &serverId);
    void openOrActivate(const ServerProfile &profile, bool connectNow = true);
    void duplicateSession(const ServerProfile &profile);
    void updateServer(const ServerProfile &profile);
    void closeServer(const QString &serverId);
    void setFileWorkspaceVisible(bool visible);
    void setServerManager(QWidget *manager);
    void showServerManager();
    void showCredentialManager();
    void showSettings(QWidget *page);
    [[nodiscard]] bool isHomePageVisible() const;

signals:
    void sessionCountChanged(int count);
    void activeSessionChanged(const ServerProfile &profile, SshSession *session);
    void monitorSessionChanged(QWidget *tabPage, const ServerProfile &profile, SshSession *session);
    void latencyChanged(QWidget *tabPage, const QString &text);
    void sessionClosed(SshSession *session);
    void sessionConnectionChanged(const QString &serverId, bool connected, const QString &message);
    void commandSubmitted(const QString &serverId, const QString &command);
    void homePageVisibilityChanged(bool visible);
    void savedCredentialsChanged();
    void tabsChanged();
    void sessionCommandSubmitted(SshSession *session, const QString &command);
    void splitLayoutChanged(int paneCount);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *activePane(QWidget *page) const;
    QWidget *currentPane() const;
    QList<QWidget *> panes(QWidget *page) const;
    QWidget *createPane(QWidget *page, const ServerProfile &profile, bool fileVisible);
    void activatePane(QWidget *pane, bool focus = false);
    void setPaneFileWorkspaceVisible(QWidget *pane, bool visible);
    void setPaneFileWorkspaceExpanded(QWidget *pane, bool expanded);
    void restoreExpandedFileWorkspace();
    void closePane(QWidget *pane);
    void removePane(QWidget *pane);
    void updateTabPresentation(QWidget *page);
    void updateTabSelection();
    void layoutPanes(QWidget *page, const QList<QWidget *> &panes, int layout);
    QWidget *appendPaneTab(QWidget *pane);
    Q_INVOKABLE bool prepareTabContextMenu(int index);
    void addSession(const ServerProfile &profile, bool activate, bool persist, bool connectNow);
    void duplicateSessionAt(int index);
    void closeSession(int index);
    void removeSession(int index);
    void requestCloseSessions(const QList<QPointer<QWidget>> &pages, const QString &title);
    void installCloseButton(int index);
    void closeOtherSessions(int index);
    void closeAllSessions();
    void updateWorkspaceState();
    void refreshRecentLogins();
    void persistState();
    void publishActiveSession();
    void updateLatencyProbes(bool refreshConfiguration = false);
    void publishMonitorSession();
    [[nodiscard]] QStringList serverIds() const;

    ServerRepository *m_repository{};
    CredentialStore *m_credentialStore{};
    QTabBar *m_tabs{};
    QStackedWidget *m_stack{};
    QStackedWidget *m_viewStack{};
    QWidget *m_emptyPage{};
    QTabWidget *m_homeTabs{};
    QWidget *m_serverManagerPage{};
    QWidget *m_sessionsPage{};
    QWidget *m_tabToolbar{};
    QTreeWidget *m_recentLogins{};
    QLabel *m_recentEmptyLabel{};
    QMenu *m_tabMenu{};
    QAction *m_connectAction{};
    QAction *m_disconnectAction{};
    QAction *m_closeOthersAction{};
    QAction *m_mergeAction{}, *m_unmergeAction{};
    QList<QPointer<QWidget>> m_selectedPages;
    QPointer<QWidget> m_selectionAnchor;
    int m_tabContextIndex{-1};
    bool m_fileWorkspaceVisible{true};
    bool m_confirmingClose{false};
    bool m_nativeTitlebarTabs{false};
    QPointer<QWidget> m_expandedFilePane;
    QList<QPointer<QWidget>> m_expansionHiddenWidgets;
    QList<QPair<QPointer<QSplitter>, QByteArray>> m_expansionSplitStates;
    QList<QPointer<TerminalView>> m_expansionSuspendedViews;
};

} // namespace noxshell::ui
