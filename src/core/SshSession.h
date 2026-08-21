#pragma once

#include "LinuxMetrics.h"
#include "FileTransferTask.h"
#include "RemoteFileEntry.h"
#include "ServerProfile.h"

#include <QObject>
#include <QHash>
#include <QThread>

#include <optional>

namespace noxshell {

class Libssh2Worker;
class ServerRepository;
class CredentialStore;

class SshSession final : public QObject {
    Q_OBJECT

public:
    explicit SshSession(ServerRepository *repository = nullptr, CredentialStore *credentialStore = nullptr, QObject *parent = nullptr);
    ~SshSession() override;

    void connectTo(const ServerProfile &profile);
    void disconnectFromHost();
    void execute(const QString &command);
    void sendInput(const QByteArray &data);
    void resizeTerminal(int columns, int rows, int pixelWidth, int pixelHeight);
    void requestMetrics();
    void requestHomeDirectory();
    void listDirectory(const QString &path);
    void uploadFile(const QString &localPath, const QString &remotePath);
    void downloadFile(const QString &remotePath, const QString &localPath);
    quint64 readFile(const QString &remotePath, quint64 maxBytes = 4 * 1024 * 1024);
    quint64 writeFile(const QString &remotePath, const QByteArray &data, bool overwrite = true);
    void cancelTransfer(quint64 taskId);
    void retryTransfer(quint64 taskId);
    void setTransferRateLimit(quint64 bytesPerSecond) { m_transferRateLimit = bytesPerSecond; }
    void setTransferPersistenceEnabled(bool enabled) { m_transferPersistenceEnabled = enabled; }
    [[nodiscard]] quint64 transferRateLimit() const { return m_transferRateLimit; }
    void createDirectory(const QString &path);
    void renamePath(const QString &sourcePath, const QString &destinationPath);
    void removePath(const QString &path, bool directory);
    void changePermissions(const QString &path, quint32 permissions, bool recursive = false,
        PermissionScope scope = PermissionScope::FilesAndDirectories);
    void approveHostKey(bool approved);

    [[nodiscard]] bool isConnected() const { return m_connected; }
    [[nodiscard]] ServerProfile profile() const { return m_profile; }

signals:
    void connectRequested(const ServerProfile &profile);
    void disconnectRequested();
    void executeRequested(const QString &command);
    void inputRequested(const QByteArray &data);
    void terminalResizeRequested(int columns, int rows, int pixelWidth, int pixelHeight);
    void collectMetricsRequested(quint64 requestId);
    void homeDirectoryRequested(quint64 requestId);
    void listDirectoryRequested(quint64 requestId, const QString &path);
    void uploadFileRequested(quint64 requestId, const QString &localPath, const QString &remotePath, quint64 bytesPerSecond);
    void downloadFileRequested(quint64 requestId, const QString &remotePath, const QString &localPath, quint64 bytesPerSecond);
    void readFileRequested(quint64 requestId, const QString &remotePath, quint64 maxBytes);
    void writeFileRequested(quint64 requestId, const QString &remotePath, const QByteArray &data, bool overwrite);
    void createDirectoryRequested(quint64 requestId, const QString &path);
    void renamePathRequested(quint64 requestId, const QString &sourcePath, const QString &destinationPath);
    void removePathRequested(quint64 requestId, const QString &path, bool directory);
    void changePermissionsRequested(quint64 requestId, const QString &path, quint32 permissions,
        bool recursive, PermissionScope scope);
    void hostKeyApprovalRequested(bool approved);
    void connectionChanged(bool connected, const QString &message);
    void outputReceived(const QString &text);
    void rawOutputReceived(const QByteArray &data);
    void promptChanged(const QString &prompt);
    void hostKeyVerificationRequired(const QString &target, const QString &fingerprint, const QString &algorithm);
    void metricSampleReceived(const MetricSample &sample);
    void metricsCollectionFailed(const QString &message);
    void homeDirectoryResolved(const QString &path);
    void homeDirectoryResolutionFailed(const QString &message);
    void directoryListed(const QString &path, const RemoteFileEntries &entries);
    void directoryListingFailed(const QString &path, const QString &message);
    void fileOperationProgress(RemoteFileOperation operation, const QString &path, quint64 completed, quint64 total);
    void fileOperationFinished(RemoteFileOperation operation, const QString &path);
    void fileOperationFailed(RemoteFileOperation operation, const QString &path, const QString &message);
    void remoteFileRead(quint64 requestId, const QString &path, const QByteArray &data);
    void remoteFileReadFailed(quint64 requestId, const QString &path, const QString &message);
    void remoteFileWritten(quint64 requestId, const QString &path);
    void remoteFileWriteFailed(quint64 requestId, const QString &path, const QString &message);
    void transferTaskChanged(const FileTransferTask &task);
    void transferQueueReset();

private:
    void connectDemo();
    void executeDemo(const QString &command);
    QString demoResponseFor(const QString &command) const;
    RemoteFileEntries demoEntriesFor(const QString &path) const;
    quint64 nextFileRequestId();
    void completeDemoOperation(quint64 requestId, RemoteFileOperation operation, const QString &path);
    void enqueueTransfer(RemoteFileOperation operation, const QString &localPath, const QString &remotePath);
    void startNextTransfer();
    void finishActiveTransfer(TransferState state, const QString &message = {});
    void publishTransferTask(const FileTransferTask &task);
    void restoreTransferQueue();
    [[nodiscard]] qsizetype transferIndex(quint64 taskId) const;

    ServerProfile m_profile;
    bool m_connected{false};
    bool m_demo{true};
    bool m_metricsInFlight{false};
    quint64 m_metricRequestId{};
    int m_demoMetricTick{};
    QByteArray m_demoInputBuffer;
    quint32 m_directoryGeneration{1};
    quint32 m_directoryRequestSerial{};
    QHash<QString, RemoteFileEntries> m_demoFileOverrides;
    QHash<QString, QByteArray> m_demoFileContents;
    QVector<FileTransferTask> m_transferQueue;
    quint64 m_nextTransferTaskId{};
    quint64 m_activeTransferTaskId{};
    quint64 m_transferRateLimit{};
    bool m_transferPersistenceEnabled{true};
    std::optional<LinuxMetricsSnapshot> m_previousMetrics;
    QString m_pendingFingerprint;
    QString m_pendingAlgorithm;
    ServerRepository *m_repository{};
    CredentialStore *m_credentialStore{};
    QThread m_workerThread;
    Libssh2Worker *m_worker{};
};

} // namespace noxshell
