#pragma once

#include "../core/ServerProfile.h"

#include <QWidget>

class QStackedWidget;
class QTabBar;
class QTreeWidget;
class QLabel;
class QAction;
class QMenu;

namespace noxshell {
class CredentialStore;
class ServerRepository;
class SshSession;
}

namespace noxshell::ui {

class TerminalWorkspace final : public QWidget {
    Q_OBJECT

public:
    explicit TerminalWorkspace(ServerRepository *repository, CredentialStore *credentialStore,
        QWidget *parent = nullptr);

    [[nodiscard]] int sessionCount() const;
    [[nodiscard]] bool hasConnectedSession(const QString &serverId) const;
    bool activateExisting(const QString &serverId);
    void openOrActivate(const ServerProfile &profile, bool connectNow = true);
    void duplicateSession(const ServerProfile &profile);
    void updateServer(const ServerProfile &profile);
    void closeServer(const QString &serverId);

signals:
    void sessionCountChanged(int count);
    void activeSessionChanged(const ServerProfile &profile, SshSession *session);
    void sessionClosed(SshSession *session);
    void sessionConnectionChanged(const QString &serverId, bool connected, const QString &message);
    void commandSubmitted(const QString &serverId, const QString &command);

private:
    Q_INVOKABLE bool prepareTabContextMenu(int index);
    void addSession(const ServerProfile &profile, bool activate, bool persist, bool connectNow);
    void duplicateSessionAt(int index);
    void closeSession(int index);
    void closeOtherSessions(int index);
    void closeAllSessions();
    void updateWorkspaceState();
    void refreshRecentLogins();
    void persistState();
    void publishActiveSession();
    [[nodiscard]] QStringList serverIds() const;

    ServerRepository *m_repository{};
    CredentialStore *m_credentialStore{};
    QTabBar *m_tabs{};
    QStackedWidget *m_stack{};
    QStackedWidget *m_viewStack{};
    QWidget *m_emptyPage{};
    QWidget *m_sessionsPage{};
    QTreeWidget *m_recentLogins{};
    QLabel *m_recentEmptyLabel{};
    QMenu *m_tabMenu{};
    QAction *m_connectAction{};
    QAction *m_disconnectAction{};
    QAction *m_closeOthersAction{};
    int m_tabContextIndex{-1};
};

} // namespace noxshell::ui
