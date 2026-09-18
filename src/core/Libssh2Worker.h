#pragma once

#include "ServerProfile.h"
#include "RemoteFileEntry.h"
#include "MetricsCollectionPolicy.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QStringList>

#include <atomic>
#include <functional>

class QTimer;
struct _LIBSSH2_SESSION;
struct _LIBSSH2_CHANNEL;
struct _LIBSSH2_SFTP;

namespace noxshell {

class Libssh2Worker final : public QObject {
    Q_OBJECT

public:
    explicit Libssh2Worker(QObject *parent = nullptr, int authenticationTimeoutMs = 30000,
        int inputStallTimeoutMs = 8000);
    ~Libssh2Worker() override;

    // May be called from the GUI thread while a connection operation is pending.
    void cancelConnection();
    quint64 connectionGeneration() const;
    void setDesiredDirectorySizeRequest(quint64 requestId);

public slots:
    void connectTo(const ServerProfile &profile, quint64 requestGeneration = 0);
    void approveHostKey(bool approved);
    void execute(const QString &command);
    void sendInput(const QByteArray &data);
    void resizePty(int columns, int rows, int pixelWidth, int pixelHeight);
    void collectMetrics(quint64 requestId);
    void resolveHomeDirectory(quint64 requestId);
    void listDirectory(quint64 requestId, const QString &path);
    void calculateDirectorySize(quint64 requestId, const QString &path);
    void uploadFile(quint64 requestId, const QString &localPath, const QString &remotePath, quint64 bytesPerSecond);
    void downloadFile(quint64 requestId, const QString &remotePath, const QString &localPath, quint64 bytesPerSecond);
    void readFile(quint64 requestId, const QString &remotePath, quint64 maxBytes);
    void writeFile(quint64 requestId, const QString &remotePath, const QByteArray &data, bool overwrite);
    void cancelTransfer(quint64 requestId);
    void createDirectory(quint64 requestId, const QString &path);
    void renamePath(quint64 requestId, const QString &sourcePath, const QString &destinationPath);
    void removePath(quint64 requestId, const QString &path, bool directory);
    void changePermissions(quint64 requestId, const QString &path, quint32 permissions,
        bool recursive, PermissionScope scope);
    void disconnectFromHost();

signals:
    void passwordAuthenticationRejected(quint64 generation);
    void connectionChanged(bool connected, const QString &message, quint64 generation);
    void outputReceived(const QString &text);
    void rawOutputReceived(const QByteArray &data);
    void promptChanged(const QString &prompt);
    void hostKeyVerificationRequired(const QString &fingerprint, const QString &algorithm);
    void metricsPayloadReceived(quint64 requestId, const QByteArray &payload);
    void metricsCollectionFailed(quint64 requestId, const QString &message);
    void homeDirectoryResolved(quint64 requestId, const QString &path);
    void homeDirectoryResolutionFailed(quint64 requestId, const QString &message);
    void directoryListed(quint64 requestId, const QString &path, const RemoteFileEntries &entries);
    void directoryListingFailed(quint64 requestId, const QString &path, const QString &message);
    void directorySizeCalculated(quint64 requestId, const QString &path, quint64 bytes, const QString &error);
    void fileOperationProgress(quint64 requestId, RemoteFileOperation operation, const QString &path, quint64 completed, quint64 total);
    void fileOperationFinished(quint64 requestId, RemoteFileOperation operation, const QString &path);
    void fileOperationFailed(quint64 requestId, RemoteFileOperation operation, const QString &path, const QString &message);
    void remoteFileRead(quint64 requestId, const QString &path, const QByteArray &data);
    void remoteFileReadFailed(quint64 requestId, const QString &path, const QString &message);
    void remoteFileWritten(quint64 requestId, const QString &path);
    void remoteFileWriteFailed(quint64 requestId, const QString &path, const QString &message);

private slots:
    bool drainChannel();
    void advanceDirectorySize();

private:
    void reportConnectionState(bool connected, const QString &message);
    void fail(const QString &stage, const QString &detail);
    void continueAuthentication();
    int authenticatePassword();
    int authenticateKeyboardInteractive();
    int authenticatePrivateKey();
    int authenticateAgent();
    bool advertisedAuthenticationMethods(QStringList &methods);
    int runConnectionOperation(const std::function<int()> &operation, int timeoutMs);
    QString connectionOperationError() const;
    bool connectionCanceled() const;
    bool openShell();
    QString lastSessionError() const;
    QString hostKeyAlgorithm() const;
    static QString normalizeFingerprint(const QString &fingerprint);
    void cleanup();
    bool beginSftpOperation(quint64 requestId, RemoteFileOperation operation, const QString &path, _LIBSSH2_SFTP *&sftp);
    void endSftpOperation(_LIBSSH2_SFTP *sftp);
    bool runRemoteCommand(const QByteArray &command, QByteArray &output, QByteArray &errorOutput,
        QString &failure, const QString &description);
    bool connectSocket(const QString &host, quint16 port, int timeoutMs, QString &error);
    bool waitForSocket(int timeoutMs) const;
    void closeSocket();
    void finishDirectorySize();
    bool deferDuringSizeChannelSetup(std::function<void()> operation);

    ServerProfile m_profile;
    qintptr m_socketDescriptor{-1};
    QTimer *m_readTimer{};
    _LIBSSH2_SESSION *m_session{};
    _LIBSSH2_CHANNEL *m_channel{};
    QString m_fingerprint;
    bool m_waitingForHostKey{false};
    bool m_connected{false};
    bool m_directoryShellFallback{false};
    QElapsedTimer m_metricsClock;
    MetricsCollectionPolicy m_metricsPolicy;
    const int m_authenticationTimeoutMs;
    const int m_inputStallTimeoutMs;
    QString m_connectionFailure;
    std::atomic<quint64> m_connectionGeneration{1};
    quint64 m_activeConnectionGeneration{1};
    std::atomic<quint64> m_cancelTransferId{};
    enum class SizePhase { Idle, Opening, Starting, Reading, SendingEof, Closing, Freeing };
    QTimer *m_sizeTimer{};
    _LIBSSH2_CHANNEL *m_sizeChannel{};
    SizePhase m_sizePhase{SizePhase::Idle};
    std::atomic<quint64> m_desiredSizeRequest{};
    quint64 m_sizeRequest{}, m_pendingSizeRequest{}, m_sizeBytes{};
    QString m_sizePath, m_pendingSizePath, m_sizeFailure;
    QByteArray m_sizeCommand, m_sizeOutput, m_sizeError;
    QElapsedTimer m_sizeClock;
    bool m_sizeStopSent{};
};

} // namespace noxshell
