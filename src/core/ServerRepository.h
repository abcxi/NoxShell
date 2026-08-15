#pragma once

#include "ServerProfile.h"
#include "LinuxMetrics.h"
#include "MetricHistory.h"
#include "MonitoringData.h"
#include "FileTransferTask.h"

#include <QObject>
#include <QSqlDatabase>
#include <QStringList>
#include <QVector>
#include <QDateTime>

namespace noxshell {

struct TerminalRestoreState {
    QStringList serverIds;
    int currentIndex{0};
};

struct LoginHistoryEntry {
    qint64 id{};
    QString serverId;
    QString serverName;
    QString host;
    QString user;
    quint16 port{22};
    QDateTime connectedAt;
};

class ServerRepository final : public QObject {
    Q_OBJECT

public:
    explicit ServerRepository(QString databasePath = {}, bool seedDemoData = true, QObject *parent = nullptr);
    ~ServerRepository() override;

    bool initialize();
    [[nodiscard]] QVector<ServerProfile> loadServers();
    bool saveServer(ServerProfile &profile);
    bool deleteServer(const QString &id);
    bool saveKnownHost(const QString &host, quint16 port, const QString &algorithm, const QString &fingerprint);
    [[nodiscard]] QString knownHostFingerprint(const QString &host, quint16 port);
    [[nodiscard]] TerminalRestoreState loadTerminalState();
    bool saveTerminalState(const QStringList &serverIds, int currentIndex);
    bool recordSuccessfulLogin(const QString &serverId, const QDateTime &connectedAt = QDateTime::currentDateTime());
    [[nodiscard]] QVector<LoginHistoryEntry> loadRecentLogins(int limit = 20);
    bool saveMetricSample(const QString &serverId, const MetricSample &sample);
    [[nodiscard]] QVector<MetricHistoryPoint> loadMetricHistory(const QString &serverId, const QDateTime &since);
    [[nodiscard]] MonitoringThresholds loadMonitoringThresholds(const QString &serverId);
    bool saveMonitoringThresholds(const QString &serverId, const MonitoringThresholds &thresholds);
    bool recordMonitoringAlert(const MonitoringAlert &alert);
    [[nodiscard]] QVector<MonitoringAlert> loadMonitoringAlerts(const QString &serverId, int limit = 50);
    bool saveTransferTask(const QString &serverId, const FileTransferTask &task);
    [[nodiscard]] QVector<FileTransferTask> loadTransferTasks(const QString &serverId, int limit = 100);
    bool clearTransferTasks(const QString &serverId);
    [[nodiscard]] QString lastError() const { return m_lastError; }
    [[nodiscard]] QString databasePath() const { return m_databasePath; }

private:
    bool migrate();
    bool seedIfEmpty();
    void setError(const QString &context, const QString &detail);

    QString m_databasePath;
    QString m_connectionName;
    bool m_seedDemoData{true};
    QSqlDatabase m_database;
    QString m_lastError;
};

} // namespace noxshell
