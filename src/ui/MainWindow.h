#pragma once

#include "../core/LinuxMetrics.h"
#include "../core/ServerProfile.h"
#include "AppTheme.h"

#include <QMainWindow>
#include <QHash>
#include <QSet>

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

class FilePanel;
class HostSidebar;
class MetricCard;
class SystemDetailPanel;
class TerminalWorkspace;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QString databasePath = {}, QWidget *parent = nullptr);

protected:
    void changeEvent(QEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    QToolBar *createWindowToolbar();
    QWidget *createWorkspace();
    QWidget *createMetricStrip();
    QWidget *createTerminalWorkspace();
    QWidget *createMonitorWorkspace();
    QWidget *createFileWorkspace();
    QWidget *createStatusBar();
    void setSidebarVisible(bool visible);
    void setMonitorVisible(bool visible);
    void setFileWorkspaceVisible(bool visible);
    void showTerminalSettings();
    void setThemeMode(ThemeMode mode, bool persist = true);
    void updateThemePresentation();
    void updateConnectionPresentation(const QString &serverId, bool connected, const QString &message);
    void setConnectionBadge(const QString &text, const QString &foreground, const QString &background);
    void selectServer(const ServerProfile &profile);
    void activateTerminalSession(const ServerProfile &profile, SshSession *session);
    void bindSession(SshSession *session);
    void releaseSession(SshSession *session);
    void showFilePanel(const ServerProfile &profile, SshSession *session);
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
    bool persistProfile(ServerProfile &profile, bool preserveEmptySecret);

    HostSidebar *m_sidebar{};
    QLabel *m_serverMeta{};
    QLabel *m_onlineBadge{};
    QLabel *m_sampleStatus{};
    QToolButton *m_sidebarToggleButton{};
    QToolButton *m_monitorToggleButton{};
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
    QSplitter *m_terminalFileSplitter{};
    FilePanel *m_filePanel{};
    QStackedWidget *m_fileWorkspaceStack{};
    QWidget *m_filePlaceholder{};
    QWidget *m_fileWorkspacePane{};
    SshSession *m_session{};
    QHash<SshSession *, FilePanel *> m_filePanels;
    QSet<SshSession *> m_boundSessions;
    QTimer *m_metricTimer{};
    ServerProfile m_currentServer;
    ServerRepository *m_repository{};
    CredentialStore *m_credentialStore{};
    bool m_hasMetricSample{false};
    bool m_serverDeletionInFlight{false};
    bool m_nativeTitleBarControls{false};
    bool m_fileWorkspaceVisible{true};
    ThemeMode m_themeMode{ThemeMode::System};
};

} // namespace noxshell::ui
