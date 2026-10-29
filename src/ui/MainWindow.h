#pragma once

#include "../core/LinuxMetrics.h"
#include "../core/ServerProfile.h"
#include "AppTheme.h"
#include "SystemDetailPanel.h"

#include <QMainWindow>
#include <QHash>
#include <QSet>
#include <QList>
#include <QPointer>
#include <QStringList>

class QLabel;
class QTimer;
class QPushButton;
class QToolButton;
class QToolBar;
class QStackedWidget;
class QSplitter;
class QAction;

namespace noxshell {
class SshSession;
class ServerRepository;
class CredentialStore;
}

namespace noxshell::ui {

class HostSidebar;
class MetricCard;
class SystemDetailPanel;
class TerminalWorkspace;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QString databasePath = {}, QWidget *parent = nullptr, CredentialStore *credentialStore = nullptr);
    ~MainWindow() override;
    bool confirmApplicationQuit();
    void setQuitInProgress(bool quitting) { m_quitInProgress = quitting; }

protected:
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    QToolBar *createWindowToolbar();
    QWidget *createWorkspace();
    QWidget *createMetricStrip();
    QWidget *createTerminalWorkspace();
    QWidget *createMonitorWorkspace();
    QWidget *createStatusBar();
    void setMonitorVisible(bool visible);
    void showTerminalSettings();
    void showCredentialManager();
    void showServerManager();
    void setThemeMode(ThemeMode mode, bool persist = true);
    void updateThemePresentation();
    void updateConnectionPresentation(const QString &serverId, bool connected, const QString &message);
    void setConnectionBadge(const QString &text, const QString &foreground, const QString &background);
    void selectServer(const ServerProfile &profile);
    void activateMonitorSession(QWidget *tabPage, const ServerProfile &profile, SshSession *session);
    void cacheMonitorState();
    void bindSession(SshSession *session);
    void releaseSession(SshSession *session);
    void connectToServer(const ServerProfile &profile);
    void selectAndEditServer(const ServerProfile &profile);
    void selectAndDeleteServer(const ServerProfile &profile);
    void duplicateServer(const ServerProfile &profile);
    void requestMetrics();
    void displayMetrics(const MetricSample &sample);
    void resetMetrics(const QString &detail);
    void addServer();
    void addServerInGroup(const QString &group);
    void addRdpServer();
    void addRdpServerInGroup(const QString &group);
    void editServer(const ServerProfile &profile);
    void deleteServer(const ServerProfile &profile);
    bool persistProfile(ServerProfile &profile, bool preserveEmptySecret, const ServerProfile *originalProfile = nullptr);

    HostSidebar *m_sidebar{};
    QLabel *m_serverMeta{};
    QLabel *m_serverLatency{};
    QLabel *m_onlineBadge{};
    QLabel *m_sampleStatus{};
    QPushButton *m_rdpCredentialButton{};
    ServerProfile m_pendingCredentialProfile;
    bool m_readingRdpCredentials{false};
    QToolButton *m_monitorToggleButton{};
    QToolButton *m_newConnectionButton{};
    QWidget *m_titlebarTabsHost{};
    QToolButton *m_settingsButton{};
    QToolButton *m_themeModeButton{};
    QAction *m_systemThemeAction{};
    QAction *m_lightThemeAction{};
    QAction *m_darkThemeAction{};
    QToolButton *m_maximizeWindowButton{};
    QWidget *m_monitorRail{};
    MetricCard *m_cpuCard{};
    MetricCard *m_memoryCard{};
    MetricCard *m_loadCard{};
    QLabel *m_uptimeValue{};
    SystemDetailPanel *m_systemDetailPanel{};
    TerminalWorkspace *m_terminalWorkspace{};
    QPointer<SshSession> m_session;
    struct MonitorState {
        QStringList endpoint;
        SystemDetailPanel::ViewState view;
    };
    QPointer<QWidget> m_monitorPage;
    QHash<QWidget *,MonitorState> m_monitorStates;
    QHash<QWidget *,QMetaObject::Connection> m_monitorCleanupConnections;
    QList<QWidget *> m_monitorCacheOrder;
    QSet<SshSession *> m_boundSessions;
    QTimer *m_metricTimer{};
    ServerProfile m_currentServer;
    ServerRepository *m_repository{};
    CredentialStore *m_credentialStore{};
    bool m_hasMetricSample{false};
    bool m_serverDeletionInFlight{false};
    bool m_nativeTitleBarControls{false};
    bool m_hiddenByClose{false};
    bool m_quitInProgress{false};
    ThemeMode m_themeMode{ThemeMode::System};
};

} // namespace noxshell::ui
