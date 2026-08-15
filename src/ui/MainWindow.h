#pragma once

#include "../core/LinuxMetrics.h"
#include "../core/MetricHistory.h"
#include "../core/MonitoringData.h"
#include "../core/ServerProfile.h"

#include <QMainWindow>
#include <QHash>
#include <QSet>

class QLabel;
class QTimer;
class QPushButton;
class QComboBox;
class QListWidget;
class QToolButton;
class QToolBar;
class QStackedWidget;

namespace noxshell {
class SshSession;
class ServerRepository;
class CredentialStore;
}

namespace noxshell::ui {

class FilePanel;
class HostSidebar;
class MetricCard;
class TerminalWorkspace;
class TrendChart;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QString databasePath = {}, QWidget *parent = nullptr);

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
    void refreshTrendCharts();
    void configureMonitoringThresholds();
    void evaluateMonitoringAlerts(const MetricSample &sample);
    void refreshAlertList();
    void addServer();
    void editServer(const ServerProfile &profile);
    void deleteServer(const ServerProfile &profile);
    bool persistProfile(ServerProfile &profile, bool preserveEmptySecret);

    HostSidebar *m_sidebar{};
    QLabel *m_serverMeta{};
    QLabel *m_onlineBadge{};
    QLabel *m_alertTitle{};
    QLabel *m_alertText{};
    QLabel *m_sampleStatus{};
    QToolButton *m_sidebarToggleButton{};
    QToolButton *m_monitorToggleButton{};
    QWidget *m_monitorRail{};
    MetricCard *m_cpuCard{};
    MetricCard *m_memoryCard{};
    MetricCard *m_loadCard{};
    MetricCard *m_diskCard{};
    TerminalWorkspace *m_terminalWorkspace{};
    FilePanel *m_filePanel{};
    QStackedWidget *m_fileWorkspaceStack{};
    QWidget *m_filePlaceholder{};
    TrendChart *m_cpuTrend{};
    TrendChart *m_memoryTrend{};
    TrendChart *m_loadTrend{};
    TrendChart *m_diskTrend{};
    QComboBox *m_historyRange{};
    QListWidget *m_alertList{};
    SshSession *m_session{};
    QHash<SshSession *, FilePanel *> m_filePanels;
    QSet<SshSession *> m_boundSessions;
    QTimer *m_metricTimer{};
    ServerProfile m_currentServer;
    ServerRepository *m_repository{};
    CredentialStore *m_credentialStore{};
    MetricHistory m_metricHistory{3600};
    MonitoringThresholds m_thresholds;
    QSet<QString> m_activeAlerts;
    int m_historyWindowSeconds{900};
    bool m_hasMetricSample{false};
    bool m_serverDeletionInFlight{false};
    bool m_nativeTitleBarControls{false};
};

} // namespace noxshell::ui
