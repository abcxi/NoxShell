#pragma once

#include "ServerProfile.h"
#include "RemoteFileEntry.h"

#include <QByteArray>
#include <QObject>
#include <QStringList>

#include <atomic>

class QTimer;
struct _LIBSSH2_SESSION;
struct _LIBSSH2_CHANNEL;
struct _LIBSSH2_SFTP;

namespace noxshell {

class Libssh2Worker final : public QObject {
    Q_OBJECT

public:
    explicit Libssh2Worker(QObject *parent = nullptr);
    ~Libssh2Worker() override;

public slots:
    void connectTo(const ServerProfile &profile);
    void approveHostKey(bool approved);
    void execute(const QString &command);
    void sendInput(const QByteArray &data);
    void resizePty(int columns, int rows, int pixelWidth, int pixelHeight);
    void collectMetrics(quint64 requestId);
    void resolveHomeDirectory(quint64 requestId);
    void listDirectory(quint64 requestId, const QString &path);
    void uploadFile(quint64 requestId, const QString &localPath, const QString &remotePath, quint64 bytesPerSecond);
    void downloadFile(quint64 requestId, const QString &remotePath, const QString &localPath, quint64 bytesPerSecond);
    void readFile(quint64 requestId, const QString &remotePath, quint64 maxBytes);
    void writeFile(quint64 requestId, const QString &remotePath, const QByteArray &data, bool overwrite);
    void cancelTransfer(quint64 requestId);
    void createDirectory(quint64 requestId, const QString &path);
    void renamePath(quint64 requestId, const QString &sourcePath, const QString &destinationPath);
    void removePath(quint64 requestId, const QString &path, bool directory);
    void disconnectFromHost();

signals:
    void connectionChanged(bool connected, const QString &message);
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
    void fileOperationProgress(quint64 requestId, RemoteFileOperation operation, const QString &path, quint64 completed, quint64 total);
    void fileOperationFinished(quint64 requestId, RemoteFileOperation operation, const QString &path);
    void fileOperationFailed(quint64 requestId, RemoteFileOperation operation, const QString &path, const QString &message);
    void remoteFileRead(quint64 requestId, const QString &path, const QByteArray &data);
    void remoteFileReadFailed(quint64 requestId, const QString &path, const QString &message);
    void remoteFileWritten(quint64 requestId, const QString &path);
    void remoteFileWriteFailed(quint64 requestId, const QString &path, const QString &message);

private slots:
    void drainChannel();

private:
    void fail(const QString &stage, const QString &detail);
    void continueAuthentication();
    bool authenticatePassword();
    bool authenticateKeyboardInteractive();
    bool authenticatePrivateKey();
    bool authenticateAgent();
    QStringList advertisedAuthenticationMethods() const;
    bool openShell();
    QString lastSessionError() const;
    QString hostKeyAlgorithm() const;
    static QString normalizeFingerprint(const QString &fingerprint);
    void cleanup();
    bool beginSftpOperation(quint64 requestId, RemoteFileOperation operation, const QString &path, _LIBSSH2_SFTP *&sftp);
    void endSftpOperation(_LIBSSH2_SFTP *sftp);
    bool connectSocket(const QString &host, quint16 port, int timeoutMs, QString &error);
    bool waitForSocket(int timeoutMs) const;
    void closeSocket();

    ServerProfile m_profile;
    qintptr m_socketDescriptor{-1};
    QTimer *m_readTimer{};
    _LIBSSH2_SESSION *m_session{};
    _LIBSSH2_CHANNEL *m_channel{};
    QString m_fingerprint;
    bool m_waitingForHostKey{false};
    bool m_connected{false};
    std::atomic<quint64> m_cancelTransferId{};
};

} // namespace noxshell
