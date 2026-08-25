#include "SshSession.h"

#include "Libssh2Worker.h"
#include "CredentialStore.h"
#include "ServerRepository.h"

#include <QDateTime>
#include <QDir>
#include <QMetaObject>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>
#include <functional>

namespace noxshell {

namespace {
QString trustKey(const ServerProfile &profile)
{
    return QStringLiteral("%1:%2").arg(profile.host).arg(profile.port);
}
} // namespace

SshSession::SshSession(ServerRepository *repository, CredentialStore *credentialStore, QObject *parent)
    : QObject(parent)
    , m_repository(repository)
    , m_credentialStore(credentialStore)
    , m_worker(new Libssh2Worker)
{
    qRegisterMetaType<ServerProfile>();
    qRegisterMetaType<MetricSample>();
    qRegisterMetaType<RemoteFileEntries>();
    qRegisterMetaType<RemoteFileOperation>();
    qRegisterMetaType<PermissionScope>();
    qRegisterMetaType<FileTransferTask>();
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(this, &SshSession::connectRequested, m_worker, &Libssh2Worker::connectTo, Qt::QueuedConnection);
    connect(this, &SshSession::disconnectRequested, m_worker, &Libssh2Worker::disconnectFromHost, Qt::QueuedConnection);
    connect(this, &SshSession::executeRequested, m_worker, &Libssh2Worker::execute, Qt::QueuedConnection);
    connect(this, &SshSession::inputRequested, m_worker, &Libssh2Worker::sendInput, Qt::QueuedConnection);
    connect(this, &SshSession::terminalResizeRequested, m_worker, &Libssh2Worker::resizePty, Qt::QueuedConnection);
    connect(this, &SshSession::collectMetricsRequested, m_worker, &Libssh2Worker::collectMetrics, Qt::QueuedConnection);
    connect(this, &SshSession::homeDirectoryRequested, m_worker, &Libssh2Worker::resolveHomeDirectory, Qt::QueuedConnection);
    connect(this, &SshSession::listDirectoryRequested, m_worker, &Libssh2Worker::listDirectory, Qt::QueuedConnection);
    connect(this, &SshSession::uploadFileRequested, m_worker, &Libssh2Worker::uploadFile, Qt::QueuedConnection);
    connect(this, &SshSession::downloadFileRequested, m_worker, &Libssh2Worker::downloadFile, Qt::QueuedConnection);
    connect(this, &SshSession::readFileRequested, m_worker, &Libssh2Worker::readFile, Qt::QueuedConnection);
    connect(this, &SshSession::writeFileRequested, m_worker, &Libssh2Worker::writeFile, Qt::QueuedConnection);
    connect(this, &SshSession::createDirectoryRequested, m_worker, &Libssh2Worker::createDirectory, Qt::QueuedConnection);
    connect(this, &SshSession::renamePathRequested, m_worker, &Libssh2Worker::renamePath, Qt::QueuedConnection);
    connect(this, &SshSession::removePathRequested, m_worker, &Libssh2Worker::removePath, Qt::QueuedConnection);
    connect(this, &SshSession::changePermissionsRequested, m_worker, &Libssh2Worker::changePermissions, Qt::QueuedConnection);
    connect(this, &SshSession::hostKeyApprovalRequested, m_worker, &Libssh2Worker::approveHostKey, Qt::QueuedConnection);
    connect(m_worker, &Libssh2Worker::connectionChanged, this, [this](bool connected, const QString &message) {
        m_connected = connected;
        emit connectionChanged(connected, message);
    });
    connect(m_worker, &Libssh2Worker::outputReceived, this, &SshSession::outputReceived);
    connect(m_worker, &Libssh2Worker::rawOutputReceived, this, &SshSession::rawOutputReceived);
    connect(m_worker, &Libssh2Worker::promptChanged, this, &SshSession::promptChanged);
    connect(m_worker, &Libssh2Worker::metricsPayloadReceived, this,
        [this](quint64 requestId, const QByteArray &payload) {
            if (requestId != m_metricRequestId) return;
            m_metricsInFlight = false;
            LinuxMetricsSnapshot snapshot;
            QString error;
            if (!LinuxMetricsParser::parse(payload, snapshot, &error)) {
                emit metricsCollectionFailed(error);
                return;
            }
            const auto sample = LinuxMetricsParser::calculate(snapshot, m_previousMetrics ? &*m_previousMetrics : nullptr);
            m_previousMetrics = std::move(snapshot);
            emit metricSampleReceived(sample);
        });
    connect(m_worker, &Libssh2Worker::metricsCollectionFailed, this,
        [this](quint64 requestId, const QString &message) {
            if (requestId != m_metricRequestId) return;
            m_metricsInFlight = false;
            emit metricsCollectionFailed(message);
        });
    connect(m_worker, &Libssh2Worker::homeDirectoryResolved, this,
        [this](quint64 requestId, const QString &path) {
            if (static_cast<quint32>(requestId >> 32) != m_directoryGeneration) return;
            emit homeDirectoryResolved(path);
        });
    connect(m_worker, &Libssh2Worker::homeDirectoryResolutionFailed, this,
        [this](quint64 requestId, const QString &message) {
            if (static_cast<quint32>(requestId >> 32) != m_directoryGeneration) return;
            emit homeDirectoryResolutionFailed(message);
        });
    connect(m_worker, &Libssh2Worker::directoryListed, this,
        [this](quint64 requestId, const QString &path, const RemoteFileEntries &entries) {
            if (static_cast<quint32>(requestId >> 32) != m_directoryGeneration) return;
            emit directoryListed(path, entries);
        });
    connect(m_worker, &Libssh2Worker::directoryListingFailed, this,
        [this](quint64 requestId, const QString &path, const QString &message) {
            if (static_cast<quint32>(requestId >> 32) != m_directoryGeneration) return;
            emit directoryListingFailed(path, message);
        });
    connect(m_worker, &Libssh2Worker::fileOperationProgress, this,
        [this](quint64 requestId, RemoteFileOperation operation, const QString &path, quint64 completed, quint64 total) {
            if (static_cast<quint32>(requestId >> 32) != m_directoryGeneration) return;
            const auto index = transferIndex(requestId);
            if (index >= 0) {
                auto &task = m_transferQueue[index];
                task.completed = completed;
                task.total = total;
                publishTransferTask(task);
            }
            emit fileOperationProgress(operation, path, completed, total);
        });
    connect(m_worker, &Libssh2Worker::fileOperationFinished, this,
        [this](quint64 requestId, RemoteFileOperation operation, const QString &path) {
            if (static_cast<quint32>(requestId >> 32) != m_directoryGeneration) return;
            emit fileOperationFinished(operation, path);
            if (requestId == m_activeTransferTaskId) finishActiveTransfer(TransferState::Completed);
        });
    connect(m_worker, &Libssh2Worker::fileOperationFailed, this,
        [this](quint64 requestId, RemoteFileOperation operation, const QString &path, const QString &message) {
            if (static_cast<quint32>(requestId >> 32) != m_directoryGeneration) return;
            emit fileOperationFailed(operation, path, message);
            if (requestId == m_activeTransferTaskId) {
                finishActiveTransfer(message.startsWith(QStringLiteral("已取消")) ? TransferState::Canceled : TransferState::Failed, message);
            }
        });
    connect(m_worker, &Libssh2Worker::remoteFileRead, this, [this](quint64 requestId, const QString &path, const QByteArray &data) {
        if (static_cast<quint32>(requestId >> 32) == m_directoryGeneration) emit remoteFileRead(requestId, path, data);
    });
    connect(m_worker, &Libssh2Worker::remoteFileReadFailed, this, [this](quint64 requestId, const QString &path, const QString &message) {
        if (static_cast<quint32>(requestId >> 32) == m_directoryGeneration) emit remoteFileReadFailed(requestId, path, message);
    });
    connect(m_worker, &Libssh2Worker::remoteFileWritten, this, [this](quint64 requestId, const QString &path) {
        if (static_cast<quint32>(requestId >> 32) == m_directoryGeneration) emit remoteFileWritten(requestId, path);
    });
    connect(m_worker, &Libssh2Worker::remoteFileWriteFailed, this, [this](quint64 requestId, const QString &path, const QString &message) {
        if (static_cast<quint32>(requestId >> 32) == m_directoryGeneration) emit remoteFileWriteFailed(requestId, path, message);
    });
    connect(m_worker, &Libssh2Worker::hostKeyVerificationRequired, this,
        [this](const QString &fingerprint, const QString &algorithm) {
            m_pendingFingerprint = fingerprint;
            m_pendingAlgorithm = algorithm;
            emit hostKeyVerificationRequired(trustKey(m_profile), fingerprint, algorithm);
        });
    m_workerThread.setObjectName(QStringLiteral("ssh-libssh2-worker"));
    m_workerThread.start();
}

SshSession::~SshSession()
{
    if (m_workerThread.isRunning()) {
        QMetaObject::invokeMethod(m_worker, "disconnectFromHost", Qt::BlockingQueuedConnection);
        m_workerThread.quit();
        m_workerThread.wait(3000);
    }
}

void SshSession::connectTo(const ServerProfile &profile)
{
    disconnectFromHost();
    m_profile = profile;
    m_demo = profile.connectionMode == ConnectionMode::Demo;
    m_connected = false;
    m_metricsInFlight = false;
    ++m_metricRequestId;
    ++m_directoryGeneration;
    m_directoryRequestSerial = 0;
    m_demoMetricTick = 0;
    m_demoInputBuffer.clear();
    m_demoFileOverrides.clear();
    m_demoFileContents.clear();
    m_transferQueue.clear();
    emit transferQueueReset();
    m_activeTransferTaskId = 0;
    m_previousMetrics.reset();
    m_pendingFingerprint.clear();
    m_pendingAlgorithm.clear();
    restoreTransferQueue();

    if (m_demo) {
        connectDemo();
        return;
    }

    auto request = profile;
    if (request.expectedFingerprint.isEmpty()) {
        request.expectedFingerprint = m_repository ? m_repository->knownHostFingerprint(request.host, request.port) : QString{};
    }
    if (m_credentialStore && !request.credentialRef.isEmpty()) {
        const auto secret = m_credentialStore->load(request.credentialRef);
        request.password = secret.password;
        request.keyPassphrase = secret.keyPassphrase;
    }
    emit connectionChanged(false, QStringLiteral("正在连接 %1:%2…").arg(request.host).arg(request.port));
    emit connectRequested(request);
}

void SshSession::disconnectFromHost()
{
    if (m_activeTransferTaskId) m_worker->cancelTransfer(m_activeTransferTaskId);
    for (auto &task : m_transferQueue) {
        if (task.state == TransferState::Queued || task.state == TransferState::Running) {
            task.state = TransferState::Canceled;
            task.message = QStringLiteral("连接已切换");
            publishTransferTask(task);
        }
    }
    m_activeTransferTaskId = 0;
    ++m_directoryGeneration;
    m_directoryRequestSerial = 0;
    if (m_demo) {
        if (m_connected) {
            m_connected = false;
            emit connectionChanged(false, QStringLiteral("SSH 已断开"));
        }
        return;
    }
    if (m_workerThread.isRunning()) {
        emit disconnectRequested();
    }
}

void SshSession::execute(const QString &command)
{
    if (m_demo) {
        executeDemo(command);
        return;
    }
    emit executeRequested(command);
}

void SshSession::sendInput(const QByteArray &data)
{
    if (m_demo) {
        for (char byte : data) {
            if (byte == '\r' || byte == '\n') {
                emit rawOutputReceived(QByteArray("\r\n"));
                const auto command = QString::fromUtf8(m_demoInputBuffer);
                m_demoInputBuffer.clear();
                executeDemo(command);
                emit rawOutputReceived(QStringLiteral("%1@%2:/var/www/app# ").arg(m_profile.user, m_profile.name).toUtf8());
            } else if (byte == '\x03') {
                m_demoInputBuffer.clear();
                emit rawOutputReceived(QByteArray("^C\r\n") + QStringLiteral("%1@%2:/var/www/app# ").arg(m_profile.user, m_profile.name).toUtf8());
            } else if (byte == '\x0c') {
                const auto prompt = QStringLiteral("%1@%2:/var/www/app# ").arg(m_profile.user, m_profile.name).toUtf8();
                emit rawOutputReceived(QByteArray("\x1b[2J\x1b[H") + prompt + m_demoInputBuffer);
            } else if (byte == '\x7f' || byte == '\b') {
                if (!m_demoInputBuffer.isEmpty()) {
                    m_demoInputBuffer.chop(1);
                    emit rawOutputReceived(QByteArray("\b \b"));
                }
            } else if (static_cast<unsigned char>(byte) >= 0x20) {
                m_demoInputBuffer.append(byte);
                emit rawOutputReceived(QByteArray(1, byte));
            }
        }
        return;
    }
    emit inputRequested(data);
}

void SshSession::resizeTerminal(int columns, int rows, int pixelWidth, int pixelHeight)
{
    if (!m_demo) emit terminalResizeRequested(columns, rows, pixelWidth, pixelHeight);
}

void SshSession::requestMetrics()
{
    if (m_metricsInFlight) return;
    if (!m_connected) {
        emit metricsCollectionFailed(QStringLiteral("SSH 会话未连接"));
        return;
    }

    m_metricsInFlight = true;
    const auto requestId = ++m_metricRequestId;
    if (!m_demo) {
        emit collectMetricsRequested(requestId);
        return;
    }

    const int tick = ++m_demoMetricTick;
    QTimer::singleShot(0, this, [this, requestId, tick] {
        if (requestId != m_metricRequestId || !m_demo || !m_connected) return;
        m_metricsInFlight = false;
        MetricSample sample;
        sample.capturedAt = QDateTime::currentDateTime();
        sample.cpuReady = true;
        sample.cpuPercent = 38.0 + (tick * 7 % 13);
        sample.kernelPercent = 7.2 + (tick * 3 % 4);
        sample.memoryTotalBytes = 16ULL * 1024 * 1024 * 1024;
        sample.memoryPercent = 66.0 + (tick * 3 % 5);
        sample.memoryUsedBytes = static_cast<quint64>(sample.memoryTotalBytes * sample.memoryPercent / 100.0);
        sample.load1 = 1.02 + static_cast<double>(tick * 9 % 35) / 100.0;
        sample.load5 = 1.08;
        sample.load15 = 0.94;
        sample.uptimeSeconds = 12ULL * 86400 + 7ULL * 3600 + 23ULL * 60;
        sample.cpuCoreCount = 4;
        sample.primaryDisk.fileSystem = QStringLiteral("/dev/vda1");
        sample.primaryDisk.mountPoint = QStringLiteral("/");
        sample.primaryDisk.totalBytes = 200ULL * 1024 * 1024 * 1024;
        sample.primaryDisk.usedBytes = 146ULL * 1024 * 1024 * 1024;
        sample.primaryDisk.availableBytes = 54ULL * 1024 * 1024 * 1024;
        sample.primaryDisk.usagePercent = m_profile.state == ServerState::Warning ? 88 : 73;
        sample.disks = {
            sample.primaryDisk,
            {QStringLiteral("tmpfs"), QStringLiteral("/run"), 2ULL * 1024 * 1024 * 1024,
                72ULL * 1024 * 1024, 1976ULL * 1024 * 1024, 4},
            {QStringLiteral("tmpfs"), QStringLiteral("/dev/shm"), 8ULL * 1024 * 1024 * 1024,
                0, 8ULL * 1024 * 1024 * 1024, 0},
        };
        sample.networkRates = {
            {QStringLiteral("lo"), 2048.0, 2048.0},
            {QStringLiteral("eth0"), 118.0 * 1024.0 + tick * 120.0, 26.0 * 1024.0 + tick * 80.0},
        };
        sample.processes = {
            {1432, QStringLiteral("root"), 23.8, 4.1, 680ULL * 1024 * 1024, QStringLiteral("noxshell-agent")},
            {986, QStringLiteral("mysql"), 12.4, 18.6, 3ULL * 1024 * 1024 * 1024, QStringLiteral("mysqld")},
            {2014, QStringLiteral("www"), 5.7, 2.3, 376ULL * 1024 * 1024, QStringLiteral("nginx")},
            {770, QStringLiteral("root"), 1.2, 0.8, 128ULL * 1024 * 1024, QStringLiteral("sshd")},
        };
        emit metricSampleReceived(sample);
    });
}

void SshSession::listDirectory(const QString &path)
{
    const auto normalized = QDir::cleanPath(path.trimmed().isEmpty() ? QStringLiteral("/") : path.trimmed());
    const auto requestId = (static_cast<quint64>(m_directoryGeneration) << 32) | ++m_directoryRequestSerial;
    if (!m_connected) {
        emit directoryListingFailed(normalized, QStringLiteral("SSH 会话未连接"));
        return;
    }
    if (!m_demo) {
        emit listDirectoryRequested(requestId, normalized);
        return;
    }
    QTimer::singleShot(25, this, [this, requestId, normalized] {
        if (static_cast<quint32>(requestId >> 32) != m_directoryGeneration || !m_demo || !m_connected) return;
        emit directoryListed(normalized, demoEntriesFor(normalized));
    });
}

void SshSession::requestHomeDirectory()
{
    if (!m_connected) {
        emit homeDirectoryResolutionFailed(QStringLiteral("SSH 会话未连接"));
        return;
    }
    const auto requestId = nextFileRequestId();
    if (!m_demo) {
        emit homeDirectoryRequested(requestId);
        return;
    }
    QTimer::singleShot(0, this, [this, requestId] {
        if (static_cast<quint32>(requestId >> 32) != m_directoryGeneration || !m_connected || !m_demo) return;
        emit homeDirectoryResolved(QStringLiteral("/var/www/app"));
    });
}

quint64 SshSession::nextFileRequestId()
{
    return (static_cast<quint64>(m_directoryGeneration) << 32) | ++m_directoryRequestSerial;
}

void SshSession::uploadFile(const QString &localPath, const QString &remotePath)
{
    if (!m_connected) {
        emit fileOperationFailed(RemoteFileOperation::Upload, remotePath, QStringLiteral("SSH 会话未连接"));
    } else {
        if (!QFileInfo(localPath).isFile()) {
            emit fileOperationFailed(RemoteFileOperation::Upload, remotePath, QStringLiteral("本地文件不存在"));
            return;
        }
        enqueueTransfer(RemoteFileOperation::Upload, localPath, remotePath);
    }
}

void SshSession::downloadFile(const QString &remotePath, const QString &localPath)
{
    if (!m_connected) {
        emit fileOperationFailed(RemoteFileOperation::Download, remotePath, QStringLiteral("SSH 会话未连接"));
    } else {
        enqueueTransfer(RemoteFileOperation::Download, localPath, remotePath);
    }
}

quint64 SshSession::readFile(const QString &remotePath, quint64 maxBytes)
{
    const auto requestId = nextFileRequestId();
    if (!m_connected) {
        emit remoteFileReadFailed(requestId, remotePath, QStringLiteral("SSH 会话未连接"));
        return requestId;
    }
    if (!m_demo) {
        emit readFileRequested(requestId, remotePath, maxBytes);
        return requestId;
    }

    const auto entries = demoEntriesFor(QFileInfo(remotePath).path());
    const auto found = std::find_if(entries.cbegin(), entries.cend(), [&remotePath](const RemoteFileEntry &entry) {
        return entry.path == remotePath && !entry.directory;
    });
    if (found == entries.cend()) {
        emit remoteFileReadFailed(requestId, remotePath, QStringLiteral("远端文件不存在"));
        return requestId;
    }
    const auto data = m_demoFileContents.value(remotePath,
        QStringLiteral("# 玄壳演示文件\n# %1\n").arg(remotePath).toUtf8());
    if (static_cast<quint64>(data.size()) > maxBytes) {
        emit remoteFileReadFailed(requestId, remotePath, QStringLiteral("文件超过可编辑大小限制（%1 MB）").arg(maxBytes / 1024 / 1024));
        return requestId;
    }
    QTimer::singleShot(20, this, [this, requestId, remotePath, data] {
        if (static_cast<quint32>(requestId >> 32) == m_directoryGeneration && m_connected) {
            emit remoteFileRead(requestId, remotePath, data);
        }
    });
    return requestId;
}

quint64 SshSession::writeFile(const QString &remotePath, const QByteArray &data, bool overwrite)
{
    const auto requestId = nextFileRequestId();
    if (!m_connected) {
        emit remoteFileWriteFailed(requestId, remotePath, QStringLiteral("SSH 会话未连接"));
        return requestId;
    }
    if (!m_demo) {
        emit writeFileRequested(requestId, remotePath, data, overwrite);
        return requestId;
    }

    const auto parent = QFileInfo(remotePath).path();
    auto entries = demoEntriesFor(parent);
    auto existing = std::find_if(entries.begin(), entries.end(), [&remotePath](const RemoteFileEntry &entry) {
        return entry.path == remotePath;
    });
    if (existing != entries.end() && (existing->directory || !overwrite)) {
        emit remoteFileWriteFailed(requestId, remotePath,
            existing->directory ? QStringLiteral("目标是目录") : QStringLiteral("目标名称已存在"));
        return requestId;
    }
    if (existing == entries.end()) {
        RemoteFileEntry entry;
        entry.name = QFileInfo(remotePath).fileName();
        entry.path = remotePath;
        entry.permissions = 0100644;
        entry.owner = m_profile.user.isEmpty() ? QStringLiteral("root") : m_profile.user;
        entry.group = entry.owner;
        entries.append(entry);
        existing = std::prev(entries.end());
    }
    existing->size = static_cast<quint64>(data.size());
    existing->modifiedAt = QDateTime::currentDateTime();
    m_demoFileOverrides.insert(parent, entries);
    m_demoFileContents.insert(remotePath, data);
    QTimer::singleShot(20, this, [this, requestId, remotePath] {
        if (static_cast<quint32>(requestId >> 32) == m_directoryGeneration && m_connected) {
            emit remoteFileWritten(requestId, remotePath);
        }
    });
    return requestId;
}

void SshSession::enqueueTransfer(RemoteFileOperation operation, const QString &localPath, const QString &remotePath)
{
    if (m_transferQueue.size() >= 100) {
        const auto removable = std::find_if(m_transferQueue.begin(), m_transferQueue.end(), [](const FileTransferTask &task) {
            return task.state == TransferState::Completed || task.state == TransferState::Failed || task.state == TransferState::Canceled;
        });
        if (removable != m_transferQueue.end()) m_transferQueue.erase(removable);
    }
    FileTransferTask task;
    task.id = nextFileRequestId();
    task.operation = operation;
    task.localPath = localPath;
    task.remotePath = remotePath;
    task.total = operation == RemoteFileOperation::Upload ? static_cast<quint64>(QFileInfo(localPath).size()) : 0;
    m_transferQueue.append(task);
    publishTransferTask(task);
    startNextTransfer();
}

qsizetype SshSession::transferIndex(quint64 taskId) const
{
    for (qsizetype index = 0; index < m_transferQueue.size(); ++index) {
        if (m_transferQueue.at(index).id == taskId) return index;
    }
    return -1;
}

void SshSession::startNextTransfer()
{
    if (m_activeTransferTaskId || !m_connected) return;
    auto next = std::find_if(m_transferQueue.begin(), m_transferQueue.end(), [](const FileTransferTask &task) {
        return task.state == TransferState::Queued;
    });
    if (next == m_transferQueue.end()) return;
    next->state = TransferState::Running;
    next->message = QStringLiteral("传输中");
    m_activeTransferTaskId = next->id;
    const auto task = *next;
    publishTransferTask(task);

    if (!m_demo) {
        if (task.operation == RemoteFileOperation::Upload) emit uploadFileRequested(task.id, task.localPath, task.remotePath, m_transferRateLimit);
        else emit downloadFileRequested(task.id, task.remotePath, task.localPath, m_transferRateLimit);
        return;
    }

    QTimer::singleShot(25, this, [this, task] {
        const auto index = transferIndex(task.id);
        if (index < 0 || m_transferQueue[index].state != TransferState::Running) return;
        const quint64 total = task.operation == RemoteFileOperation::Upload
            ? static_cast<quint64>(QFileInfo(task.localPath).size())
            : static_cast<quint64>(QStringLiteral("玄壳演示下载\n远端路径：%1\n").arg(task.remotePath).toUtf8().size());
        m_transferQueue[index].completed = total / 2;
        m_transferQueue[index].total = total;
        publishTransferTask(m_transferQueue[index]);
        emit fileOperationProgress(task.operation, task.remotePath, total / 2, total);
    });
    QTimer::singleShot(70, this, [this, task] {
        const auto index = transferIndex(task.id);
        if (index < 0 || m_transferQueue[index].state != TransferState::Running) return;
        QString failure;
        quint64 total = 0;
        if (task.operation == RemoteFileOperation::Upload) {
            QFileInfo source(task.localPath);
            if (!source.isFile()) {
                failure = QStringLiteral("本地文件不存在");
            } else {
                QFile sourceFile(task.localPath);
                if (!sourceFile.open(QIODevice::ReadOnly)) {
                    failure = QStringLiteral("无法读取本地文件");
                }
                const auto parent = QFileInfo(task.remotePath).path();
                auto entries = demoEntriesFor(parent);
                entries.erase(std::remove_if(entries.begin(), entries.end(), [&task](const RemoteFileEntry &entry) {
                    return entry.path == task.remotePath;
                }), entries.end());
                RemoteFileEntry entry;
                entry.name = QFileInfo(task.remotePath).fileName();
                entry.path = task.remotePath;
                entry.size = static_cast<quint64>(source.size());
                entry.modifiedAt = QDateTime::currentDateTime();
                entry.permissions = 0100644;
                entry.owner = m_profile.user.isEmpty() ? QStringLiteral("root") : m_profile.user;
                entry.group = entry.owner;
                total = entry.size;
                entries.append(entry);
                m_demoFileOverrides.insert(parent, entries);
                if (failure.isEmpty()) m_demoFileContents.insert(task.remotePath, sourceFile.readAll());
            }
        } else {
            const auto parent = QFileInfo(task.remotePath).path();
            const auto entries = demoEntriesFor(parent);
            const auto found = std::find_if(entries.cbegin(), entries.cend(), [&task](const RemoteFileEntry &entry) {
                return entry.path == task.remotePath && !entry.directory;
            });
            if (found == entries.cend()) {
                failure = QStringLiteral("远端文件不存在");
            } else {
                QFile output(task.localPath);
                const auto content = m_demoFileContents.value(task.remotePath,
                    QStringLiteral("玄壳演示下载\n远端路径：%1\n").arg(task.remotePath).toUtf8());
                if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate) || output.write(content) != content.size()) {
                    failure = QStringLiteral("无法保存本地文件：%1").arg(output.errorString());
                }
                total = content.size();
            }
        }
        if (!failure.isEmpty()) {
            emit fileOperationFailed(task.operation, task.remotePath, failure);
            finishActiveTransfer(TransferState::Failed, failure);
            return;
        }
        m_transferQueue[index].completed = total;
        m_transferQueue[index].total = total;
        publishTransferTask(m_transferQueue[index]);
        emit fileOperationProgress(task.operation, task.remotePath, total, total);
        emit fileOperationFinished(task.operation, task.remotePath);
        finishActiveTransfer(TransferState::Completed);
    });
}

void SshSession::finishActiveTransfer(TransferState state, const QString &message)
{
    const auto index = transferIndex(m_activeTransferTaskId);
    if (index >= 0) {
        auto &task = m_transferQueue[index];
        task.state = state;
        task.message = message.isEmpty()
            ? (state == TransferState::Completed ? QStringLiteral("已完成") : QStringLiteral("已取消"))
            : message;
        publishTransferTask(task);
    }
    m_activeTransferTaskId = 0;
    QTimer::singleShot(0, this, &SshSession::startNextTransfer);
}

void SshSession::cancelTransfer(quint64 taskId)
{
    const auto index = transferIndex(taskId);
    if (index < 0) return;
    auto &task = m_transferQueue[index];
    if (task.state == TransferState::Queued) {
        task.state = TransferState::Canceled;
        task.message = QStringLiteral("已从队列取消");
        publishTransferTask(task);
        return;
    }
    if (task.state != TransferState::Running) return;
    task.message = QStringLiteral("正在取消…");
    publishTransferTask(task);
    if (m_demo) {
        emit fileOperationFailed(task.operation, task.remotePath, QStringLiteral("已取消，可再次提交以续传"));
        finishActiveTransfer(TransferState::Canceled, QStringLiteral("已取消，可再次提交以续传"));
    } else {
        m_worker->cancelTransfer(taskId);
    }
}

void SshSession::retryTransfer(quint64 taskId)
{
    const auto index = transferIndex(taskId);
    if (index < 0) return;
    auto &task = m_transferQueue[index];
    if (task.state != TransferState::Failed && task.state != TransferState::Canceled) return;
    if (task.operation == RemoteFileOperation::Upload && !QFileInfo(task.localPath).isFile()) {
        task.message = QStringLiteral("无法重试：本地文件不存在");
        publishTransferTask(task);
        return;
    }
    task.state = TransferState::Queued;
    task.message = QStringLiteral("等待重试");
    publishTransferTask(task);
    startNextTransfer();
}

void SshSession::publishTransferTask(const FileTransferTask &task)
{
    if (m_transferPersistenceEnabled && m_repository && !m_profile.id.isEmpty()) {
        m_repository->saveTransferTask(m_profile.id, task);
    }
    emit transferTaskChanged(task);
}

void SshSession::restoreTransferQueue()
{
    if (!m_transferPersistenceEnabled || !m_repository || m_profile.id.isEmpty()) return;
    auto tasks = m_repository->loadTransferTasks(m_profile.id, 100);
    if (tasks.isEmpty()) return;
    m_repository->clearTransferTasks(m_profile.id);
    for (auto &task : tasks) {
        task.id = nextFileRequestId();
        if (task.state == TransferState::Queued || task.state == TransferState::Running) {
            task.state = TransferState::Failed;
            task.message = QStringLiteral("上次退出前未完成，可点击重试并续传");
        }
        m_transferQueue.append(task);
        publishTransferTask(task);
    }
}

void SshSession::createDirectory(const QString &path)
{
    const auto requestId = nextFileRequestId();
    if (!m_connected) {
        emit fileOperationFailed(RemoteFileOperation::MakeDirectory, path, QStringLiteral("SSH 会话未连接"));
    } else if (!m_demo) {
        emit createDirectoryRequested(requestId, path);
    } else {
        const auto parent = QFileInfo(path).path();
        auto entries = demoEntriesFor(parent);
        const bool exists = std::any_of(entries.cbegin(), entries.cend(), [&path](const RemoteFileEntry &entry) { return entry.path == path; });
        if (exists) {
            emit fileOperationFailed(RemoteFileOperation::MakeDirectory, path, QStringLiteral("目标名称已存在"));
            return;
        }
        RemoteFileEntry entry;
        entry.name = QFileInfo(path).fileName();
        entry.path = path;
        entry.directory = true;
        entry.permissions = 0040755;
        entry.modifiedAt = QDateTime::currentDateTime();
        entry.owner = m_profile.user.isEmpty() ? QStringLiteral("root") : m_profile.user;
        entry.group = entry.owner;
        entries.prepend(entry);
        m_demoFileOverrides.insert(parent, entries);
        m_demoFileOverrides.insert(path, {});
        completeDemoOperation(requestId, RemoteFileOperation::MakeDirectory, path);
    }
}

void SshSession::renamePath(const QString &sourcePath, const QString &destinationPath)
{
    const auto requestId = nextFileRequestId();
    if (!m_connected) {
        emit fileOperationFailed(RemoteFileOperation::Rename, sourcePath, QStringLiteral("SSH 会话未连接"));
    } else if (!m_demo) {
        emit renamePathRequested(requestId, sourcePath, destinationPath);
    } else {
        const auto parent = QFileInfo(sourcePath).path();
        auto entries = demoEntriesFor(parent);
        const bool destinationExists = std::any_of(entries.cbegin(), entries.cend(), [&destinationPath](const RemoteFileEntry &entry) {
            return entry.path == destinationPath;
        });
        auto source = std::find_if(entries.begin(), entries.end(), [&sourcePath](const RemoteFileEntry &entry) { return entry.path == sourcePath; });
        if (source == entries.end() || destinationExists) {
            emit fileOperationFailed(RemoteFileOperation::Rename, sourcePath,
                source == entries.end() ? QStringLiteral("源文件不存在") : QStringLiteral("目标名称已存在"));
            return;
        }
        const bool directory = source->directory;
        source->name = QFileInfo(destinationPath).fileName();
        source->path = destinationPath;
        source->modifiedAt = QDateTime::currentDateTime();
        m_demoFileOverrides.insert(parent, entries);
        if (directory) {
            auto children = demoEntriesFor(sourcePath);
            for (auto &child : children) {
                child.path = destinationPath + QLatin1Char('/') + child.name;
            }
            m_demoFileOverrides.remove(sourcePath);
            m_demoFileOverrides.insert(destinationPath, children);
        } else if (m_demoFileContents.contains(sourcePath)) {
            m_demoFileContents.insert(destinationPath, m_demoFileContents.take(sourcePath));
        }
        completeDemoOperation(requestId, RemoteFileOperation::Rename, destinationPath);
    }
}

void SshSession::removePath(const QString &path, bool directory)
{
    const auto requestId = nextFileRequestId();
    if (!m_connected) {
        emit fileOperationFailed(RemoteFileOperation::Remove, path, QStringLiteral("SSH 会话未连接"));
    } else if (!m_demo) {
        emit removePathRequested(requestId, path, directory);
    } else {
        if (directory && !demoEntriesFor(path).isEmpty()) {
            emit fileOperationFailed(RemoteFileOperation::Remove, path, QStringLiteral("仅支持删除空目录"));
            return;
        }
        const auto parent = QFileInfo(path).path();
        auto entries = demoEntriesFor(parent);
        const auto before = entries.size();
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&path](const RemoteFileEntry &entry) {
            return entry.path == path;
        }), entries.end());
        if (entries.size() == before) {
            emit fileOperationFailed(RemoteFileOperation::Remove, path, QStringLiteral("目标不存在"));
            return;
        }
        m_demoFileOverrides.insert(parent, entries);
        m_demoFileOverrides.remove(path);
        m_demoFileContents.remove(path);
        completeDemoOperation(requestId, RemoteFileOperation::Remove, path);
    }
}

void SshSession::changePermissions(const QString &path, quint32 permissions, bool recursive, PermissionScope scope)
{
    const auto requestId = nextFileRequestId();
    if (!m_connected) {
        emit fileOperationFailed(RemoteFileOperation::ChangePermissions, path, QStringLiteral("SSH 会话未连接"));
        return;
    }
    if (!m_demo) {
        emit changePermissionsRequested(requestId, path, permissions & 07777U, recursive, scope);
        return;
    }

    bool found = false;
    std::function<void(const QString &, bool)> applyToPath;
    applyToPath = [this, permissions, recursive, scope, &found, &applyToPath](const QString &targetPath, bool selectedRoot) {
        const auto parent = QFileInfo(targetPath).path();
        auto siblings = demoEntriesFor(parent);
        auto target = std::find_if(siblings.begin(), siblings.end(), [&targetPath](const RemoteFileEntry &entry) {
            return entry.path == targetPath;
        });
        if (target == siblings.end()) return;
        found = true;
        const bool directory = target->directory;
        const bool apply = !recursive || scope == PermissionScope::FilesAndDirectories
            || (directory && scope == PermissionScope::DirectoriesOnly)
            || (!directory && scope == PermissionScope::FilesOnly);
        if (apply) {
            target->permissions = (target->permissions & ~07777U) | (permissions & 07777U);
            target->modifiedAt = QDateTime::currentDateTime();
            m_demoFileOverrides.insert(parent, siblings);
        }
        if (!recursive || !directory || target->symbolicLink) return;
        const auto children = demoEntriesFor(targetPath);
        for (const auto &child : children) applyToPath(child.path, false);
        Q_UNUSED(selectedRoot);
    };
    applyToPath(path, true);
    if (!found) {
        emit fileOperationFailed(RemoteFileOperation::ChangePermissions, path, QStringLiteral("目标不存在"));
        return;
    }
    completeDemoOperation(requestId, RemoteFileOperation::ChangePermissions, path);
}

void SshSession::completeDemoOperation(quint64 requestId, RemoteFileOperation operation, const QString &path)
{
    QTimer::singleShot(20, this, [this, requestId, operation, path] {
        if (static_cast<quint32>(requestId >> 32) != m_directoryGeneration || !m_connected || !m_demo) return;
        emit fileOperationFinished(operation, path);
    });
}

void SshSession::approveHostKey(bool approved)
{
    if (approved && !m_pendingFingerprint.isEmpty()) {
        if (m_repository) {
            m_repository->saveKnownHost(m_profile.host, m_profile.port, m_pendingAlgorithm, m_pendingFingerprint);
        }
    }
    emit hostKeyApprovalRequested(approved);
    m_pendingFingerprint.clear();
    m_pendingAlgorithm.clear();
}

void SshSession::connectDemo()
{
    emit connectionChanged(false, QStringLiteral("正在连接演示主机 %1…").arg(m_profile.host));
    QTimer::singleShot(260, this, [this] {
        if (!m_demo) {
            return;
        }
        m_connected = true;
        emit outputReceived(QStringLiteral(
            "Last login: %1 from 10.0.8.17\n"
            "当前为本地演示会话；通过新增/编辑主机并选择“真实 SSH”可建立远端连接。\n")
                                .arg(QDateTime::currentDateTime().toString(QStringLiteral("ddd MMM dd HH:mm:ss yyyy"))));
        emit connectionChanged(true, QStringLiteral("演示 SSH 已连接 · 28 ms"));
        emit promptChanged(QStringLiteral("%1@%2:/var/www/app# ").arg(m_profile.user, m_profile.name));
    });
}

void SshSession::executeDemo(const QString &command)
{
    if (!m_connected) {
        emit outputReceived(QStringLiteral("会话未连接。\n"));
        return;
    }
    emit outputReceived(demoResponseFor(command));
}

QString SshSession::demoResponseFor(const QString &command) const
{
    const auto trimmed = command.trimmed();
    if (trimmed.isEmpty()) return {};
    if (trimmed == QStringLiteral("clear")) return QStringLiteral("__CLEAR__");
    if (trimmed == QStringLiteral("pwd")) return QStringLiteral("/var/www/app\n");
    if (trimmed == QStringLiteral("whoami")) return m_profile.user + QLatin1Char('\n');
    if (trimmed == QStringLiteral("uptime")) return QStringLiteral("10:42:03 up 18 days, 2:41, 1 user, load average: 1.26, 1.08, 0.94\n");
    if (trimmed.startsWith(QStringLiteral("df"))) return QStringLiteral("Filesystem Size Used Avail Use% Mounted on\n/dev/vda1 200G 146G 54G 73% /\n");
    if (trimmed.startsWith(QStringLiteral("systemctl status nginx"))) return QStringLiteral("● nginx.service - A high performance web server\n     Active: active (running)\n");
    if (trimmed == QStringLiteral("help")) return QStringLiteral("演示命令：pwd、whoami、uptime、df -h、systemctl status nginx、clear\n");
    return QStringLiteral("bash: %1: 演示会话未实现该命令，可输入 help\n").arg(trimmed);
}

RemoteFileEntries SshSession::demoEntriesFor(const QString &path) const
{
    if (m_demoFileOverrides.contains(path)) return m_demoFileOverrides.value(path);
    auto makeEntry = [this, &path](const QString &name, bool directory, quint64 size, int minutesAgo) {
        RemoteFileEntry entry;
        entry.name = name;
        entry.path = path == QStringLiteral("/") ? QStringLiteral("/") + name : path + QLatin1Char('/') + name;
        entry.directory = directory;
        entry.size = size;
        entry.permissions = directory ? 0040755 : 0100644;
        entry.modifiedAt = QDateTime::currentDateTime().addSecs(-minutesAgo * 60);
        entry.owner = m_profile.user.isEmpty() ? QStringLiteral("root") : m_profile.user;
        entry.group = entry.owner;
        return entry;
    };

    if (path == QStringLiteral("/")) {
        return {makeEntry(QStringLiteral("etc"), true, 0, 90),
            makeEntry(QStringLiteral("home"), true, 0, 70),
            makeEntry(QStringLiteral("var"), true, 0, 32),
            makeEntry(QStringLiteral("README.txt"), false, 2048, 24)};
    }
    if (path == QStringLiteral("/var")) {
        return {makeEntry(QStringLiteral("log"), true, 0, 12),
            makeEntry(QStringLiteral("www"), true, 0, 18),
            makeEntry(QStringLiteral("tmp"), true, 0, 5)};
    }
    if (path == QStringLiteral("/var/www")) {
        return {makeEntry(QStringLiteral("app"), true, 0, 4), makeEntry(QStringLiteral("html"), true, 0, 80)};
    }
    if (path == QStringLiteral("/var/www/app")) {
        return {makeEntry(QStringLiteral("app"), true, 0, 22),
            makeEntry(QStringLiteral("config"), true, 0, 120),
            makeEntry(QStringLiteral("storage"), true, 0, 8),
            makeEntry(QStringLiteral("vendor"), true, 0, 145),
            makeEntry(QStringLiteral(".env.production"), false, 1843, 32),
            makeEntry(QStringLiteral("deploy.sh"), false, 3174, 18),
            makeEntry(QStringLiteral("docker-compose.yml"), false, 2458, 41),
            makeEntry(QStringLiteral("README.md"), false, 6352, 320)};
    }
    if (path.startsWith(QStringLiteral("/var/www/app/"))) {
        return {makeEntry(QStringLiteral("README.md"), false, 4096, 12),
            makeEntry(QStringLiteral("current"), true, 0, 3)};
    }
    return {};
}

} // namespace noxshell
