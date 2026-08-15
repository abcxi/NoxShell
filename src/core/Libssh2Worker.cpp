#include "Libssh2Worker.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QRegularExpression>
#include <QTimer>
#include <QThread>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <array>
#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace noxshell {

namespace {
constexpr int kConnectTimeoutMs = 8000;
constexpr int kMetricsTimeoutMs = 3500;

// libssh2 的 keyboard-interactive 回调没有用户数据参数。每个 SSH worker
// 运行在自己的线程中，因此用 thread_local 保存本次认证输入，既能支持
// 多会话并发，也不会让一个主机的凭据进入另一个主机的回调。
thread_local QByteArray g_keyboardInteractiveUser;
thread_local QByteArray g_keyboardInteractivePassword;

LIBSSH2_USERAUTH_KBDINT_RESPONSE_FUNC(keyboardInteractiveResponse)
{
    Q_UNUSED(name);
    Q_UNUSED(name_len);
    Q_UNUSED(instruction);
    Q_UNUSED(instruction_len);
    Q_UNUSED(abstract);

    for (int index = 0; index < num_prompts; ++index) {
        const auto prompt = prompts && prompts[index].text
            ? QByteArray(reinterpret_cast<const char *>(prompts[index].text),
                  static_cast<qsizetype>(prompts[index].length)).toLower()
            : QByteArray{};
        const bool asksForUser = prompts && prompts[index].echo != 0
            && (prompt.contains("user") || prompt.contains("login"));
        const auto &value = asksForUser ? g_keyboardInteractiveUser : g_keyboardInteractivePassword;
        auto *text = static_cast<char *>(std::malloc(static_cast<size_t>(value.size()) + 1));
        if (!text) {
            responses[index].text = nullptr;
            responses[index].length = 0;
            continue;
        }
        if (!value.isEmpty()) std::memcpy(text, value.constData(), static_cast<size_t>(value.size()));
        text[value.size()] = '\0';
        responses[index].text = text;
        responses[index].length = static_cast<unsigned int>(value.size());
    }
}

constexpr auto kMetricsCommand =
    "export LC_ALL=C; "
    "printf '__CPU__\\n'; head -n 1 /proc/stat; "
    "printf '__MEM__\\n'; cat /proc/meminfo; "
    "printf '__LOAD__\\n'; cat /proc/loadavg; "
    "printf '__CORES__\\n'; (getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || printf '1\\n'); "
    "printf '__DISK__\\n'; (df -Pk -x tmpfs -x devtmpfs 2>/dev/null || df -Pk 2>/dev/null)";

QString nativePath(const QString &path)
{
    if (path.startsWith(QStringLiteral("~/"))) {
        return QDir::home().filePath(path.mid(2));
    }
    return path;
}

QString commitRemoteTemporaryFile(LIBSSH2_SFTP *sftp, const QByteArray &temporary,
    const QByteArray &destination, bool overwrite)
{
    if (!overwrite) {
        if (libssh2_sftp_rename_ex(sftp, temporary.constData(), temporary.size(),
                destination.constData(), destination.size(), 0) == 0) {
            return {};
        }
        return QStringLiteral("提交远端文件失败（SFTP %1）").arg(libssh2_sftp_last_error(sftp));
    }

    // OpenSSH 的 posix-rename 扩展能在目标存在时提供真正的原子替换，
    // 也是旧版 SFTP v3 服务器上最可靠的覆盖方式。
    if (libssh2_sftp_posix_rename_ex(sftp, temporary.constData(), temporary.size(),
            destination.constData(), destination.size()) == 0) {
        return {};
    }
    const auto posixError = libssh2_sftp_last_error(sftp);

    if (libssh2_sftp_rename_ex(sftp, temporary.constData(), temporary.size(),
            destination.constData(), destination.size(), LIBSSH2_SFTP_RENAME_OVERWRITE) == 0) {
        return {};
    }
    const auto overwriteError = libssh2_sftp_last_error(sftp);

    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    const bool destinationExists = libssh2_sftp_lstat(sftp, destination.constData(), &attributes) == 0;
    if (!destinationExists) {
        if (libssh2_sftp_rename_ex(sftp, temporary.constData(), temporary.size(),
                destination.constData(), destination.size(), 0) == 0) {
            return {};
        }
        return QStringLiteral("提交远端文件失败（POSIX SFTP %1，覆盖 SFTP %2，普通 SFTP %3）")
            .arg(posixError).arg(overwriteError).arg(libssh2_sftp_last_error(sftp));
    }

    // 极旧的 SFTP 服务端既没有 posix-rename，也拒绝 RENAME_OVERWRITE。
    // 先保留旧文件，再提交新文件；若提交失败则立刻回滚旧文件。
    const auto backup = temporary + QByteArrayLiteral(".backup");
    libssh2_sftp_unlink_ex(sftp, backup.constData(), backup.size());
    if (libssh2_sftp_rename_ex(sftp, destination.constData(), destination.size(),
            backup.constData(), backup.size(), 0) != 0) {
        return QStringLiteral("提交远端文件失败：服务器不支持覆盖重命名，且无法备份原文件（SFTP %1；POSIX %2，覆盖 %3）")
            .arg(libssh2_sftp_last_error(sftp)).arg(posixError).arg(overwriteError);
    }

    if (libssh2_sftp_rename_ex(sftp, temporary.constData(), temporary.size(),
            destination.constData(), destination.size(), 0) == 0) {
        libssh2_sftp_unlink_ex(sftp, backup.constData(), backup.size());
        return {};
    }

    const auto commitError = libssh2_sftp_last_error(sftp);
    const bool rolledBack = libssh2_sftp_rename_ex(sftp, backup.constData(), backup.size(),
        destination.constData(), destination.size(), 0) == 0;
    return rolledBack
        ? QStringLiteral("提交远端文件失败（SFTP %1），原文件已自动恢复").arg(commitError)
        : QStringLiteral("提交远端文件失败（SFTP %1），且原文件回滚失败（SFTP %2）；备份保留为 %3")
              .arg(commitError).arg(libssh2_sftp_last_error(sftp)).arg(QString::fromUtf8(backup));
}
} // namespace

Libssh2Worker::Libssh2Worker(QObject *parent)
    : QObject(parent)
    , m_readTimer(new QTimer(this))
{
    static const bool initialized = [] { return libssh2_init(0) == 0; }();
    Q_UNUSED(initialized);
    m_readTimer->setInterval(16);
    connect(m_readTimer, &QTimer::timeout, this, &Libssh2Worker::drainChannel);
}

Libssh2Worker::~Libssh2Worker()
{
    cleanup();
}

void Libssh2Worker::connectTo(const ServerProfile &profile)
{
    cleanup();
    m_readTimer->setParent(this);
    m_profile = profile;
    emit connectionChanged(false, QStringLiteral("TCP 连接 %1:%2…").arg(profile.host).arg(profile.port));

    QString socketError;
    if (!connectSocket(profile.host, profile.port, kConnectTimeoutMs, socketError)) {
        fail(QStringLiteral("TCP"), socketError);
        return;
    }

    m_session = libssh2_session_init();
    if (!m_session) {
        fail(QStringLiteral("SSH 初始化"), QStringLiteral("libssh2_session_init 失败"));
        return;
    }
    libssh2_session_set_blocking(m_session, 1);
    libssh2_session_set_timeout(m_session, kConnectTimeoutMs);
    emit connectionChanged(false, QStringLiteral("正在进行 SSH 握手…"));
    const auto handshakeResult = libssh2_session_handshake(m_session, static_cast<libssh2_socket_t>(m_socketDescriptor));
    if (handshakeResult != 0) {
        fail(QStringLiteral("SSH 握手"), lastSessionError());
        return;
    }

    const char *hostKey = nullptr;
    size_t hostKeyLength = 0;
    int hostKeyType = 0;
    hostKey = libssh2_session_hostkey(m_session, &hostKeyLength, &hostKeyType);
    Q_UNUSED(hostKeyType);
    if (!hostKey || hostKeyLength == 0) {
        fail(QStringLiteral("主机身份"), QStringLiteral("服务端未提供主机公钥"));
        return;
    }
    const auto hash = QCryptographicHash::hash(QByteArray(hostKey, static_cast<qsizetype>(hostKeyLength)), QCryptographicHash::Sha256);
    m_fingerprint = QStringLiteral("SHA256:") + QString::fromLatin1(hash.toBase64(QByteArray::OmitTrailingEquals));

    const auto expected = normalizeFingerprint(profile.expectedFingerprint);
    if (expected.isEmpty()) {
        m_waitingForHostKey = true;
        emit connectionChanged(false, QStringLiteral("等待确认主机指纹"));
        emit hostKeyVerificationRequired(m_fingerprint, hostKeyAlgorithm());
        return;
    }
    if (expected != normalizeFingerprint(m_fingerprint)) {
        fail(QStringLiteral("主机指纹"), QStringLiteral("指纹不匹配，连接已阻断。期望 %1，实际 %2").arg(profile.expectedFingerprint, m_fingerprint));
        return;
    }
    continueAuthentication();
}

void Libssh2Worker::approveHostKey(bool approved)
{
    if (!m_waitingForHostKey || !m_session) {
        return;
    }
    m_waitingForHostKey = false;
    if (!approved) {
        fail(QStringLiteral("主机指纹"), QStringLiteral("用户拒绝了未知主机指纹"));
        return;
    }
    continueAuthentication();
}

void Libssh2Worker::continueAuthentication()
{
    emit connectionChanged(false, QStringLiteral("正在认证 %1@%2…").arg(m_profile.user, m_profile.host));
    const auto advertisedMethods = advertisedAuthenticationMethods();
    QStringList attemptedMethods;
    QString localFailure;
    bool authenticated = false;
    switch (m_profile.authentication) {
    case AuthenticationMethod::Password: {
        if (m_profile.password.isEmpty()) {
            localFailure = QStringLiteral("密码为空，请重新输入或检查系统凭据库");
            break;
        }
        const bool methodsUnknown = advertisedMethods.isEmpty();
        if (methodsUnknown || advertisedMethods.contains(QStringLiteral("keyboard-interactive"))) {
            attemptedMethods.append(QStringLiteral("keyboard-interactive"));
            emit connectionChanged(false, QStringLiteral("正在使用键盘交互认证 %1@%2…").arg(m_profile.user, m_profile.host));
            authenticated = authenticateKeyboardInteractive();
        }
        if (!authenticated && (methodsUnknown || advertisedMethods.contains(QStringLiteral("password")))) {
            attemptedMethods.append(QStringLiteral("password"));
            emit connectionChanged(false, QStringLiteral("正在使用密码认证 %1@%2…").arg(m_profile.user, m_profile.host));
            authenticated = authenticatePassword();
        }
        if (!authenticated && attemptedMethods.isEmpty()) {
            localFailure = QStringLiteral("当前选择的是密码认证，但服务端没有开放 password 或 keyboard-interactive");
        }
        break;
    }
    case AuthenticationMethod::PrivateKey:
        attemptedMethods.append(QStringLiteral("publickey"));
        authenticated = authenticatePrivateKey();
        break;
    case AuthenticationMethod::SshAgent:
        attemptedMethods.append(QStringLiteral("publickey/agent"));
        authenticated = authenticateAgent();
        break;
    }
    if (!authenticated) {
        QString detail = localFailure.isEmpty() ? lastSessionError() : localFailure;
        if (!advertisedMethods.isEmpty()) {
            detail += QStringLiteral("；服务端允许：%1").arg(advertisedMethods.join(QStringLiteral("、")));
        }
        if (!attemptedMethods.isEmpty()) {
            detail += QStringLiteral("；客户端已尝试：%1").arg(attemptedMethods.join(QStringLiteral("、")));
        }
        fail(QStringLiteral("SSH 认证"), detail);
        return;
    }
    if (!openShell()) {
        fail(QStringLiteral("PTY/Shell"), lastSessionError());
        return;
    }
    m_connected = true;
    m_readTimer->start();
    emit connectionChanged(true, QStringLiteral("SSH 已连接 · %1 · %2").arg(hostKeyAlgorithm(), m_fingerprint));
    emit promptChanged(QStringLiteral("%1@%2:~$ ").arg(m_profile.user, m_profile.name));
}

bool Libssh2Worker::authenticatePassword()
{
    const auto user = m_profile.user.toUtf8();
    const auto password = m_profile.password.toUtf8();
    if (password.isEmpty()) {
        return false;
    }
    return libssh2_userauth_password_ex(m_session, user.constData(), static_cast<unsigned int>(user.size()), password.constData(), static_cast<unsigned int>(password.size()), nullptr) == 0;
}

bool Libssh2Worker::authenticateKeyboardInteractive()
{
    const auto user = m_profile.user.toUtf8();
    const auto password = m_profile.password.toUtf8();
    if (password.isEmpty()) return false;

    g_keyboardInteractiveUser = user;
    g_keyboardInteractivePassword = password;
    const auto result = libssh2_userauth_keyboard_interactive_ex(
        m_session,
        user.constData(), static_cast<unsigned int>(user.size()),
        keyboardInteractiveResponse);
    g_keyboardInteractivePassword.fill('\0');
    g_keyboardInteractivePassword.clear();
    g_keyboardInteractiveUser.clear();
    return result == 0;
}

QStringList Libssh2Worker::advertisedAuthenticationMethods() const
{
    if (!m_session) return {};
    const auto user = m_profile.user.toUtf8();
    const auto *methods = libssh2_userauth_list(
        m_session, user.constData(), static_cast<unsigned int>(user.size()));
    if (!methods) return {};

    auto result = QString::fromLatin1(methods).split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (auto &method : result) method = method.trimmed().toLower();
    result.removeDuplicates();
    return result;
}

bool Libssh2Worker::authenticatePrivateKey()
{
    const auto user = m_profile.user.toUtf8();
    const auto publicKey = nativePath(m_profile.publicKeyPath).toUtf8();
    const auto privateKey = nativePath(m_profile.privateKeyPath).toUtf8();
    const auto passphrase = m_profile.keyPassphrase.toUtf8();
    if (privateKey.isEmpty()) {
        return false;
    }
    return libssh2_userauth_publickey_fromfile_ex(
               m_session,
               user.constData(), static_cast<unsigned int>(user.size()),
               publicKey.isEmpty() ? nullptr : publicKey.constData(), privateKey.constData(),
               passphrase.isEmpty() ? nullptr : passphrase.constData()) == 0;
}

bool Libssh2Worker::authenticateAgent()
{
    LIBSSH2_AGENT *agent = libssh2_agent_init(m_session);
    if (!agent) return false;
    const auto user = m_profile.user.toUtf8();
    bool authenticated = false;
    if (libssh2_agent_connect(agent) == 0 && libssh2_agent_list_identities(agent) == 0) {
        struct libssh2_agent_publickey *identity = nullptr;
        struct libssh2_agent_publickey *previous = nullptr;
        while (libssh2_agent_get_identity(agent, &identity, previous) == 0) {
            if (libssh2_agent_userauth(agent, user.constData(), identity) == 0) {
                authenticated = true;
                break;
            }
            previous = identity;
        }
    }
    libssh2_agent_disconnect(agent);
    libssh2_agent_free(agent);
    return authenticated;
}

bool Libssh2Worker::openShell()
{
    m_channel = libssh2_channel_open_session(m_session);
    if (!m_channel) return false;
    if (libssh2_channel_request_pty_ex(m_channel, "xterm-256color", 14, nullptr, 0, 120, 36, 0, 0) != 0) return false;
    if (libssh2_channel_shell(m_channel) != 0) return false;
    libssh2_session_set_blocking(m_session, 0);
    return true;
}

void Libssh2Worker::execute(const QString &command)
{
    auto payload = command.toUtf8();
    payload.append('\n');
    sendInput(payload);
}

void Libssh2Worker::sendInput(const QByteArray &data)
{
    if (!m_connected || !m_channel) {
        emit outputReceived(QStringLiteral("会话未连接。\n"));
        return;
    }
    qsizetype offset = 0;
    while (offset < data.size()) {
        const auto written = libssh2_channel_write(m_channel, data.constData() + offset, static_cast<size_t>(data.size() - offset));
        if (written == LIBSSH2_ERROR_EAGAIN) {
            waitForSocket(100);
            continue;
        }
        if (written < 0) {
            emit outputReceived(QStringLiteral("命令发送失败：%1\n").arg(lastSessionError()));
            return;
        }
        offset += written;
    }
}

void Libssh2Worker::resizePty(int columns, int rows, int pixelWidth, int pixelHeight)
{
    if (!m_connected || !m_channel) return;
    libssh2_channel_request_pty_size_ex(m_channel, qMax(2, columns), qMax(2, rows), qMax(0, pixelWidth), qMax(0, pixelHeight));
}

void Libssh2Worker::collectMetrics(quint64 requestId)
{
    if (!m_connected || !m_session) {
        emit metricsCollectionFailed(requestId, QStringLiteral("SSH 会话未连接"));
        return;
    }

    m_readTimer->stop();
    libssh2_session_set_blocking(m_session, 1);
    libssh2_session_set_timeout(m_session, kMetricsTimeoutMs);

    LIBSSH2_CHANNEL *metricsChannel = libssh2_channel_open_session(m_session);
    QByteArray output;
    QByteArray errorOutput;
    QString failure;

    if (!metricsChannel) {
        failure = QStringLiteral("无法打开指标采集通道：%1").arg(lastSessionError());
    } else if (libssh2_channel_exec(metricsChannel, kMetricsCommand) != 0) {
        failure = QStringLiteral("无法执行指标采集命令：%1").arg(lastSessionError());
    } else {
        std::array<char, 8192> buffer{};
        auto readStream = [&](int stream, QByteArray &target) {
            for (;;) {
                const auto received = libssh2_channel_read_ex(metricsChannel, stream, buffer.data(), buffer.size());
                if (received > 0) {
                    target.append(buffer.data(), static_cast<qsizetype>(received));
                    continue;
                }
                if (received < 0 && received != LIBSSH2_ERROR_EAGAIN) {
                    failure = QStringLiteral("读取指标数据失败：%1").arg(lastSessionError());
                }
                break;
            }
        };
        readStream(0, output);
        readStream(1, errorOutput);
    }

    if (metricsChannel) {
        libssh2_channel_send_eof(metricsChannel);
        libssh2_channel_close(metricsChannel);
        libssh2_channel_free(metricsChannel);
    }
    libssh2_session_set_timeout(m_session, kConnectTimeoutMs);
    libssh2_session_set_blocking(m_session, 0);
    if (m_connected) m_readTimer->start();

    if (!failure.isEmpty()) {
        emit metricsCollectionFailed(requestId, failure);
        return;
    }
    if (output.isEmpty()) {
        const auto detail = QString::fromUtf8(errorOutput).trimmed();
        emit metricsCollectionFailed(requestId, detail.isEmpty() ? QStringLiteral("远端未返回指标数据") : detail);
        return;
    }
    emit metricsPayloadReceived(requestId, output);
}

void Libssh2Worker::listDirectory(quint64 requestId, const QString &path)
{
    if (!m_connected || !m_session) {
        emit directoryListingFailed(requestId, path, QStringLiteral("SSH 会话未连接"));
        return;
    }

    m_readTimer->stop();
    libssh2_session_set_blocking(m_session, 1);
    libssh2_session_set_timeout(m_session, kConnectTimeoutMs);

    LIBSSH2_SFTP *sftp = libssh2_sftp_init(m_session);
    LIBSSH2_SFTP_HANDLE *directory = nullptr;
    QString failure;
    RemoteFileEntries entries;
    const auto encodedPath = path.toUtf8();

    if (!sftp) {
        failure = QStringLiteral("无法初始化 SFTP：%1").arg(lastSessionError());
    } else {
        directory = libssh2_sftp_opendir(sftp, encodedPath.constData());
        if (!directory) {
            failure = QStringLiteral("无法打开目录 %1（SFTP %2）").arg(path).arg(libssh2_sftp_last_error(sftp));
        }
    }

    if (directory) {
        std::array<char, 4096> nameBuffer{};
        std::array<char, 8192> longEntryBuffer{};
        for (;;) {
            longEntryBuffer.fill('\0');
            LIBSSH2_SFTP_ATTRIBUTES attributes{};
            const auto received = libssh2_sftp_readdir_ex(
                directory,
                nameBuffer.data(), nameBuffer.size(),
                longEntryBuffer.data(), longEntryBuffer.size(),
                &attributes);
            if (received > 0) {
                const auto name = QString::fromUtf8(nameBuffer.data(), static_cast<qsizetype>(received));
                if (name == QStringLiteral(".") || name == QStringLiteral("..")) continue;
                RemoteFileEntry entry;
                entry.name = name;
                entry.path = path == QStringLiteral("/") ? QStringLiteral("/") + name : path + QLatin1Char('/') + name;
                if (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE) entry.size = attributes.filesize;
                if (attributes.flags & LIBSSH2_SFTP_ATTR_UIDGID) {
                    entry.userId = attributes.uid;
                    entry.groupId = attributes.gid;
                    entry.ownerIdsValid = true;
                    entry.owner = QString::number(entry.userId);
                    entry.group = QString::number(entry.groupId);
                }
                if (attributes.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) {
                    entry.modifiedAt = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(attributes.mtime));
                }
                if (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
                    entry.permissions = attributes.permissions;
                    entry.directory = LIBSSH2_SFTP_S_ISDIR(attributes.permissions);
                    entry.symbolicLink = LIBSSH2_SFTP_S_ISLNK(attributes.permissions);
                }
                const auto longEntry = QString::fromUtf8(longEntryBuffer.data()).trimmed();
                const auto fields = longEntry.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
                if (fields.size() >= 6) {
                    entry.owner = fields.at(2);
                    entry.group = fields.at(3);
                }
                entries.append(std::move(entry));
                continue;
            }
            if (received < 0) failure = QStringLiteral("读取目录失败：%1").arg(lastSessionError());
            break;
        }
    }

    if (directory) libssh2_sftp_closedir(directory);
    if (sftp) libssh2_sftp_shutdown(sftp);
    libssh2_session_set_timeout(m_session, kConnectTimeoutMs);
    libssh2_session_set_blocking(m_session, 0);
    if (m_connected) m_readTimer->start();

    if (!failure.isEmpty()) {
        emit directoryListingFailed(requestId, path, failure);
        return;
    }
    std::sort(entries.begin(), entries.end(), [](const RemoteFileEntry &left, const RemoteFileEntry &right) {
        if (left.directory != right.directory) return left.directory;
        return QString::localeAwareCompare(left.name, right.name) < 0;
    });
    emit directoryListed(requestId, path, entries);
}

void Libssh2Worker::resolveHomeDirectory(quint64 requestId)
{
    if (!m_connected || !m_session) {
        emit homeDirectoryResolutionFailed(requestId, QStringLiteral("SSH 会话未连接"));
        return;
    }
    m_readTimer->stop();
    libssh2_session_set_blocking(m_session, 1);
    libssh2_session_set_timeout(m_session, kConnectTimeoutMs);

    LIBSSH2_SFTP *sftp = libssh2_sftp_init(m_session);
    QString path;
    QString failure;
    if (!sftp) {
        failure = QStringLiteral("无法初始化 SFTP：%1").arg(lastSessionError());
    } else {
        std::array<char, 4096> buffer{};
        const auto length = libssh2_sftp_realpath(sftp, ".", buffer.data(), buffer.size());
        if (length > 0) path = QString::fromUtf8(buffer.data(), static_cast<qsizetype>(length));
        else failure = QStringLiteral("无法获取远端主目录（SFTP %1）").arg(libssh2_sftp_last_error(sftp));
    }
    if (sftp) libssh2_sftp_shutdown(sftp);
    libssh2_session_set_timeout(m_session, kConnectTimeoutMs);
    libssh2_session_set_blocking(m_session, 0);
    if (m_connected) m_readTimer->start();

    if (failure.isEmpty() && path.startsWith(QLatin1Char('/'))) emit homeDirectoryResolved(requestId, QDir::cleanPath(path));
    else emit homeDirectoryResolutionFailed(requestId,
        failure.isEmpty() ? QStringLiteral("服务端返回了无效的主目录") : failure);
}

bool Libssh2Worker::beginSftpOperation(quint64 requestId, RemoteFileOperation operation, const QString &path, LIBSSH2_SFTP *&sftp)
{
    sftp = nullptr;
    if (!m_connected || !m_session) {
        emit fileOperationFailed(requestId, operation, path, QStringLiteral("SSH 会话未连接"));
        return false;
    }
    m_readTimer->stop();
    libssh2_session_set_blocking(m_session, 1);
    libssh2_session_set_timeout(m_session, kConnectTimeoutMs);
    sftp = libssh2_sftp_init(m_session);
    if (sftp) return true;

    const auto failure = QStringLiteral("无法初始化 SFTP：%1").arg(lastSessionError());
    libssh2_session_set_blocking(m_session, 0);
    if (m_connected) m_readTimer->start();
    emit fileOperationFailed(requestId, operation, path, failure);
    return false;
}

void Libssh2Worker::endSftpOperation(LIBSSH2_SFTP *sftp)
{
    if (sftp) libssh2_sftp_shutdown(sftp);
    if (!m_session) return;
    libssh2_session_set_timeout(m_session, kConnectTimeoutMs);
    libssh2_session_set_blocking(m_session, 0);
    if (m_connected) m_readTimer->start();
}

void Libssh2Worker::uploadFile(quint64 requestId, const QString &localPath, const QString &remotePath, quint64 bytesPerSecond)
{
    QFile local(localPath);
    if (!local.open(QIODevice::ReadOnly)) {
        emit fileOperationFailed(requestId, RemoteFileOperation::Upload, remotePath,
            QStringLiteral("无法读取本地文件：%1").arg(local.errorString()));
        return;
    }

    LIBSSH2_SFTP *sftp = nullptr;
    if (!beginSftpOperation(requestId, RemoteFileOperation::Upload, remotePath, sftp)) return;
    m_cancelTransferId.store(0, std::memory_order_relaxed);
    const auto temporaryPath = remotePath + QStringLiteral(".noxshell.part");
    const auto temporaryBytes = temporaryPath.toUtf8();
    QString failure;
    const auto total = static_cast<quint64>(local.size());
    quint64 completed = 0;
    LIBSSH2_SFTP_ATTRIBUTES partialAttributes{};
    bool truncatePartial = false;
    if (libssh2_sftp_stat_ex(sftp, temporaryBytes.constData(), temporaryBytes.size(), LIBSSH2_SFTP_STAT, &partialAttributes) == 0
        && (partialAttributes.flags & LIBSSH2_SFTP_ATTR_SIZE)) {
        const auto partialSize = static_cast<quint64>(partialAttributes.filesize);
        if (partialSize <= total) completed = partialSize;
        else truncatePartial = true;
    }
    if (!local.seek(static_cast<qint64>(completed))) {
        failure = QStringLiteral("无法定位本地续传位置");
        completed = 0;
    }
    auto *remote = failure.isEmpty()
        ? libssh2_sftp_open_ex(sftp, temporaryBytes.constData(), temporaryBytes.size(),
              LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | (truncatePartial ? LIBSSH2_FXF_TRUNC : 0), 0644, LIBSSH2_SFTP_OPENFILE)
        : nullptr;
    if (remote) libssh2_sftp_seek64(remote, completed);
    QElapsedTimer progressTimer;
    progressTimer.start();
    QElapsedTimer rateTimer;
    rateTimer.start();
    const auto rateStart = completed;

    if (!remote) {
        failure = QStringLiteral("无法创建远端临时文件（SFTP %1）").arg(libssh2_sftp_last_error(sftp));
    } else {
        QByteArray buffer(64 * 1024, Qt::Uninitialized);
        while (!local.atEnd() && failure.isEmpty()) {
            if (m_cancelTransferId.load(std::memory_order_relaxed) == requestId) {
                failure = QStringLiteral("__CANCELED__");
                break;
            }
            const auto read = local.read(buffer.data(), buffer.size());
            if (read < 0) {
                failure = QStringLiteral("读取本地文件失败：%1").arg(local.errorString());
                break;
            }
            qsizetype offset = 0;
            while (offset < read) {
                if (m_cancelTransferId.load(std::memory_order_relaxed) == requestId) {
                    failure = QStringLiteral("__CANCELED__");
                    break;
                }
                const auto written = libssh2_sftp_write(remote, buffer.constData() + offset, static_cast<size_t>(read - offset));
                if (written <= 0) {
                    failure = QStringLiteral("写入远端文件失败（SFTP %1）").arg(libssh2_sftp_last_error(sftp));
                    break;
                }
                offset += written;
                completed += static_cast<quint64>(written);
                if (bytesPerSecond > 0) {
                    const auto expectedMs = static_cast<qint64>((completed - rateStart) * 1000 / bytesPerSecond);
                    const auto waitMs = expectedMs - rateTimer.elapsed();
                    if (waitMs > 0) QThread::msleep(static_cast<unsigned long>(qMin<qint64>(waitMs, 100)));
                }
                if (progressTimer.elapsed() >= 100 || completed == total) {
                    emit fileOperationProgress(requestId, RemoteFileOperation::Upload, remotePath, completed, total);
                    progressTimer.restart();
                }
            }
        }
    }
    if (remote) {
        if (failure.isEmpty() && libssh2_sftp_fsync(remote) != 0
            && libssh2_sftp_last_error(sftp) != LIBSSH2_FX_OP_UNSUPPORTED) {
            failure = QStringLiteral("同步远端文件失败（SFTP %1）").arg(libssh2_sftp_last_error(sftp));
        }
        libssh2_sftp_close(remote);
    }
    const auto destinationBytes = remotePath.toUtf8();
    if (failure.isEmpty()) failure = commitRemoteTemporaryFile(sftp, temporaryBytes, destinationBytes, true);
    endSftpOperation(sftp);

    if (failure == QStringLiteral("__CANCELED__")) emit fileOperationFailed(requestId, RemoteFileOperation::Upload, remotePath, QStringLiteral("已取消，可再次上传以续传"));
    else if (!failure.isEmpty()) emit fileOperationFailed(requestId, RemoteFileOperation::Upload, remotePath, failure);
    else emit fileOperationFinished(requestId, RemoteFileOperation::Upload, remotePath);
}

void Libssh2Worker::downloadFile(quint64 requestId, const QString &remotePath, const QString &localPath, quint64 bytesPerSecond)
{
    m_cancelTransferId.store(0, std::memory_order_relaxed);
    LIBSSH2_SFTP *sftp = nullptr;
    if (!beginSftpOperation(requestId, RemoteFileOperation::Download, remotePath, sftp)) return;
    const auto remoteBytes = remotePath.toUtf8();
    auto *remote = libssh2_sftp_open_ex(sftp, remoteBytes.constData(), remoteBytes.size(),
        LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
    const auto partialPath = localPath + QStringLiteral(".noxshell.part");
    QFile local(partialPath);
    QString failure;
    quint64 total = 0;
    quint64 completed = 0;

    if (!remote) {
        failure = QStringLiteral("无法打开远端文件（SFTP %1）").arg(libssh2_sftp_last_error(sftp));
    } else {
        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        if (libssh2_sftp_fstat(remote, &attributes) == 0 && (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE)) total = attributes.filesize;
        if (!local.open(QIODevice::ReadWrite)) failure = QStringLiteral("无法保存本地临时文件：%1").arg(local.errorString());
        if (failure.isEmpty()) {
            completed = qMin(total, static_cast<quint64>(local.size()));
            if (!local.resize(static_cast<qint64>(completed)) || !local.seek(static_cast<qint64>(completed))) {
                failure = QStringLiteral("无法定位本地续传位置");
            } else {
                libssh2_sftp_seek64(remote, completed);
            }
        }
    }

    QElapsedTimer progressTimer;
    progressTimer.start();
    QElapsedTimer rateTimer;
    rateTimer.start();
    const auto rateStart = completed;
    if (failure.isEmpty()) {
        std::array<char, 64 * 1024> buffer{};
        for (;;) {
            if (m_cancelTransferId.load(std::memory_order_relaxed) == requestId) {
                failure = QStringLiteral("__CANCELED__");
                break;
            }
            const auto received = libssh2_sftp_read(remote, buffer.data(), buffer.size());
            if (received == 0) break;
            if (received < 0) {
                failure = QStringLiteral("读取远端文件失败（SFTP %1）").arg(libssh2_sftp_last_error(sftp));
                break;
            }
            if (local.write(buffer.data(), received) != received) {
                failure = QStringLiteral("写入本地文件失败：%1").arg(local.errorString());
                break;
            }
            completed += static_cast<quint64>(received);
            if (bytesPerSecond > 0) {
                const auto expectedMs = static_cast<qint64>((completed - rateStart) * 1000 / bytesPerSecond);
                const auto waitMs = expectedMs - rateTimer.elapsed();
                if (waitMs > 0) QThread::msleep(static_cast<unsigned long>(qMin<qint64>(waitMs, 100)));
            }
            if (progressTimer.elapsed() >= 100 || (total > 0 && completed == total)) {
                emit fileOperationProgress(requestId, RemoteFileOperation::Download, remotePath, completed, total);
                progressTimer.restart();
            }
        }
    }
    if (remote) libssh2_sftp_close(remote);
    local.close();
    endSftpOperation(sftp);
    if (failure.isEmpty()) {
        QFile partial(partialPath);
        QSaveFile destination(localPath);
        if (!partial.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly)) {
            failure = QStringLiteral("无法提交本地文件");
        } else {
            std::array<char, 64 * 1024> buffer{};
            while (!partial.atEnd()) {
                const auto read = partial.read(buffer.data(), buffer.size());
                if (read < 0 || destination.write(buffer.data(), read) != read) {
                    failure = QStringLiteral("提交本地文件失败");
                    break;
                }
            }
            if (failure.isEmpty() && !destination.commit()) failure = QStringLiteral("提交本地文件失败：%1").arg(destination.errorString());
            else if (!failure.isEmpty()) destination.cancelWriting();
        }
        if (failure.isEmpty()) QFile::remove(partialPath);
    }

    if (failure == QStringLiteral("__CANCELED__")) emit fileOperationFailed(requestId, RemoteFileOperation::Download, remotePath, QStringLiteral("已取消，可再次下载以续传"));
    else if (!failure.isEmpty()) emit fileOperationFailed(requestId, RemoteFileOperation::Download, remotePath, failure);
    else emit fileOperationFinished(requestId, RemoteFileOperation::Download, remotePath);
}

void Libssh2Worker::readFile(quint64 requestId, const QString &remotePath, quint64 maxBytes)
{
    if (!m_connected || !m_session) {
        emit remoteFileReadFailed(requestId, remotePath, QStringLiteral("SSH 会话未连接"));
        return;
    }
    m_readTimer->stop();
    libssh2_session_set_blocking(m_session, 1);
    libssh2_session_set_timeout(m_session, kConnectTimeoutMs);

    LIBSSH2_SFTP *sftp = libssh2_sftp_init(m_session);
    LIBSSH2_SFTP_HANDLE *remote = nullptr;
    QByteArray data;
    QString failure;
    const auto path = remotePath.toUtf8();
    if (!sftp) {
        failure = QStringLiteral("无法初始化 SFTP：%1").arg(lastSessionError());
    } else {
        remote = libssh2_sftp_open_ex(sftp, path.constData(), path.size(), LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
        if (!remote) failure = QStringLiteral("无法打开远端文件（SFTP %1）").arg(libssh2_sftp_last_error(sftp));
    }
    if (remote) {
        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        if (libssh2_sftp_fstat(remote, &attributes) == 0 && (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE)) {
            if (attributes.filesize > maxBytes) {
                failure = QStringLiteral("文件超过可编辑大小限制（%1 MB）").arg(maxBytes / 1024 / 1024);
            } else {
                data.reserve(static_cast<qsizetype>(attributes.filesize));
            }
        }
        std::array<char, 64 * 1024> buffer{};
        while (failure.isEmpty()) {
            const auto received = libssh2_sftp_read(remote, buffer.data(), buffer.size());
            if (received == 0) break;
            if (received < 0) {
                failure = QStringLiteral("读取远端文件失败（SFTP %1）").arg(libssh2_sftp_last_error(sftp));
                break;
            }
            if (static_cast<quint64>(data.size() + received) > maxBytes) {
                failure = QStringLiteral("文件超过可编辑大小限制（%1 MB）").arg(maxBytes / 1024 / 1024);
                break;
            }
            data.append(buffer.data(), static_cast<qsizetype>(received));
        }
    }
    if (remote) libssh2_sftp_close(remote);
    if (sftp) libssh2_sftp_shutdown(sftp);
    libssh2_session_set_timeout(m_session, kConnectTimeoutMs);
    libssh2_session_set_blocking(m_session, 0);
    if (m_connected) m_readTimer->start();

    if (failure.isEmpty()) emit remoteFileRead(requestId, remotePath, data);
    else emit remoteFileReadFailed(requestId, remotePath, failure);
}

void Libssh2Worker::writeFile(quint64 requestId, const QString &remotePath, const QByteArray &data, bool overwrite)
{
    if (!m_connected || !m_session) {
        emit remoteFileWriteFailed(requestId, remotePath, QStringLiteral("SSH 会话未连接"));
        return;
    }
    m_readTimer->stop();
    libssh2_session_set_blocking(m_session, 1);
    libssh2_session_set_timeout(m_session, kConnectTimeoutMs);

    LIBSSH2_SFTP *sftp = libssh2_sftp_init(m_session);
    LIBSSH2_SFTP_HANDLE *remote = nullptr;
    QString failure;
    const auto destination = remotePath.toUtf8();
    const auto temporaryPath = remotePath + QStringLiteral(".noxshell-edit.part");
    const auto temporary = temporaryPath.toUtf8();
    LIBSSH2_SFTP_ATTRIBUTES existing{};
    bool destinationExists = false;
    long permissions = 0644;
    if (!sftp) {
        failure = QStringLiteral("无法初始化 SFTP：%1").arg(lastSessionError());
    } else {
        destinationExists = libssh2_sftp_lstat(sftp, destination.constData(), &existing) == 0;
        if (destinationExists && !overwrite) {
            failure = QStringLiteral("目标名称已存在");
        } else if (destinationExists && (existing.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)) {
            permissions = static_cast<long>(existing.permissions & 07777);
        }
    }
    if (failure.isEmpty() && sftp) {
        remote = libssh2_sftp_open_ex(sftp, temporary.constData(), temporary.size(),
            LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC, permissions, LIBSSH2_SFTP_OPENFILE);
        if (!remote) failure = QStringLiteral("无法创建远端临时文件（SFTP %1）").arg(libssh2_sftp_last_error(sftp));
    }
    qsizetype offset = 0;
    while (remote && offset < data.size() && failure.isEmpty()) {
        const auto written = libssh2_sftp_write(remote, data.constData() + offset, static_cast<size_t>(data.size() - offset));
        if (written <= 0) {
            failure = QStringLiteral("写入远端文件失败（SFTP %1）").arg(libssh2_sftp_last_error(sftp));
            break;
        }
        offset += static_cast<qsizetype>(written);
    }
    if (remote) {
        if (failure.isEmpty() && libssh2_sftp_fsync(remote) != 0
            && libssh2_sftp_last_error(sftp) != LIBSSH2_FX_OP_UNSUPPORTED) {
            failure = QStringLiteral("同步远端文件失败（SFTP %1）").arg(libssh2_sftp_last_error(sftp));
        }
        libssh2_sftp_close(remote);
    }
    if (failure.isEmpty()) failure = commitRemoteTemporaryFile(sftp, temporary, destination, overwrite);
    if (failure.isEmpty() && destinationExists && (existing.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)) {
        LIBSSH2_SFTP_ATTRIBUTES preserved{};
        preserved.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
        preserved.permissions = existing.permissions;
        libssh2_sftp_setstat(sftp, destination.constData(), &preserved);
    }
    if (!failure.isEmpty() && sftp) libssh2_sftp_unlink_ex(sftp, temporary.constData(), temporary.size());
    if (sftp) libssh2_sftp_shutdown(sftp);
    libssh2_session_set_timeout(m_session, kConnectTimeoutMs);
    libssh2_session_set_blocking(m_session, 0);
    if (m_connected) m_readTimer->start();

    if (failure.isEmpty()) emit remoteFileWritten(requestId, remotePath);
    else emit remoteFileWriteFailed(requestId, remotePath, failure);
}

void Libssh2Worker::cancelTransfer(quint64 requestId)
{
    m_cancelTransferId.store(requestId, std::memory_order_relaxed);
}

void Libssh2Worker::createDirectory(quint64 requestId, const QString &path)
{
    LIBSSH2_SFTP *sftp = nullptr;
    if (!beginSftpOperation(requestId, RemoteFileOperation::CreateDirectory, path, sftp)) return;
    const auto bytes = path.toUtf8();
    const int result = libssh2_sftp_mkdir_ex(sftp, bytes.constData(), bytes.size(), 0755);
    const auto sftpError = libssh2_sftp_last_error(sftp);
    endSftpOperation(sftp);
    if (result == 0) emit fileOperationFinished(requestId, RemoteFileOperation::CreateDirectory, path);
    else emit fileOperationFailed(requestId, RemoteFileOperation::CreateDirectory, path, QStringLiteral("创建目录失败（SFTP %1）").arg(sftpError));
}

void Libssh2Worker::renamePath(quint64 requestId, const QString &sourcePath, const QString &destinationPath)
{
    LIBSSH2_SFTP *sftp = nullptr;
    if (!beginSftpOperation(requestId, RemoteFileOperation::Rename, sourcePath, sftp)) return;
    const auto source = sourcePath.toUtf8();
    const auto destination = destinationPath.toUtf8();
    LIBSSH2_SFTP_ATTRIBUTES existing{};
    QString failure;
    if (libssh2_sftp_lstat(sftp, destination.constData(), &existing) == 0) {
        failure = QStringLiteral("目标名称已存在");
    } else if (libssh2_sftp_rename_ex(sftp, source.constData(), source.size(), destination.constData(), destination.size(), 0) != 0) {
        failure = QStringLiteral("重命名失败（SFTP %1）").arg(libssh2_sftp_last_error(sftp));
    }
    endSftpOperation(sftp);
    if (failure.isEmpty()) emit fileOperationFinished(requestId, RemoteFileOperation::Rename, destinationPath);
    else emit fileOperationFailed(requestId, RemoteFileOperation::Rename, sourcePath, failure);
}

void Libssh2Worker::removePath(quint64 requestId, const QString &path, bool directory)
{
    LIBSSH2_SFTP *sftp = nullptr;
    if (!beginSftpOperation(requestId, RemoteFileOperation::Remove, path, sftp)) return;
    const auto bytes = path.toUtf8();
    const int result = directory ? libssh2_sftp_rmdir_ex(sftp, bytes.constData(), bytes.size())
                                 : libssh2_sftp_unlink_ex(sftp, bytes.constData(), bytes.size());
    const auto sftpError = libssh2_sftp_last_error(sftp);
    endSftpOperation(sftp);
    if (result == 0) emit fileOperationFinished(requestId, RemoteFileOperation::Remove, path);
    else emit fileOperationFailed(requestId, RemoteFileOperation::Remove, path,
        directory ? QStringLiteral("删除目录失败；仅支持删除空目录（SFTP %1）").arg(sftpError)
                  : QStringLiteral("删除文件失败（SFTP %1）").arg(sftpError));
}

void Libssh2Worker::drainChannel()
{
    if (!m_channel) return;
    std::array<char, 8192> buffer{};
    for (int stream = 0; stream < 2; ++stream) {
        for (;;) {
            const auto received = libssh2_channel_read_ex(m_channel, stream, buffer.data(), buffer.size());
            if (received > 0) {
                const QByteArray chunk(buffer.data(), static_cast<qsizetype>(received));
                emit rawOutputReceived(chunk);
                continue;
            }
            break;
        }
    }
    if (libssh2_channel_eof(m_channel)) {
        emit connectionChanged(false, QStringLiteral("远端已关闭 SSH 会话"));
        cleanup();
    }
}

void Libssh2Worker::disconnectFromHost()
{
    const bool wasConnected = m_connected || m_session || m_socketDescriptor >= 0;
    cleanup();
    if (wasConnected) emit connectionChanged(false, QStringLiteral("SSH 已断开"));
}

void Libssh2Worker::cleanup()
{
    m_readTimer->stop();
    m_waitingForHostKey = false;
    m_connected = false;
    if (m_channel) {
        libssh2_channel_send_eof(m_channel);
        libssh2_channel_close(m_channel);
        libssh2_channel_free(m_channel);
        m_channel = nullptr;
    }
    if (m_session) {
        libssh2_session_disconnect_ex(m_session, SSH_DISCONNECT_BY_APPLICATION, "Client disconnect", "en");
        libssh2_session_free(m_session);
        m_session = nullptr;
    }
    closeSocket();
}

bool Libssh2Worker::connectSocket(const QString &host, quint16 port, int timeoutMs, QString &error)
{
    closeSocket();
#ifdef Q_OS_WIN
    static const bool winsockReady = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!winsockReady) {
        error = QStringLiteral("Windows Socket 初始化失败");
        return false;
    }
#endif

    const auto encodedHost = host.toUtf8();
    const auto encodedPort = QByteArray::number(port);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo *addresses = nullptr;
    const int lookupResult = getaddrinfo(encodedHost.constData(), encodedPort.constData(), &hints, &addresses);
    if (lookupResult != 0) {
#ifdef Q_OS_WIN
        error = QStringLiteral("无法解析主机 %1（错误 %2）").arg(host).arg(lookupResult);
#else
        error = QStringLiteral("无法解析主机 %1：%2").arg(host, QString::fromLocal8Bit(gai_strerror(lookupResult)));
#endif
        return false;
    }

    QElapsedTimer elapsed;
    elapsed.start();
    int lastSocketError = 0;
    for (auto *address = addresses; address; address = address->ai_next) {
        const int remaining = qMax(1, timeoutMs - static_cast<int>(elapsed.elapsed()));
        if (remaining <= 1 && elapsed.elapsed() >= timeoutMs) break;
        const auto descriptor = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
#ifdef Q_OS_WIN
        if (descriptor == INVALID_SOCKET) {
            lastSocketError = WSAGetLastError();
            continue;
        }
        u_long nonBlocking = 1;
        ioctlsocket(descriptor, FIONBIO, &nonBlocking);
        int result = ::connect(descriptor, address->ai_addr, static_cast<int>(address->ai_addrlen));
        if (result != 0 && WSAGetLastError() != WSAEWOULDBLOCK && WSAGetLastError() != WSAEINPROGRESS) {
            lastSocketError = WSAGetLastError();
            closesocket(descriptor);
            continue;
        }
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(descriptor, &writeSet);
        timeval timeout{remaining / 1000, (remaining % 1000) * 1000};
        result = select(0, nullptr, &writeSet, nullptr, &timeout);
        int connectionError = 0;
        int connectionErrorLength = sizeof(connectionError);
        if (result > 0) getsockopt(descriptor, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&connectionError), &connectionErrorLength);
        if (result <= 0 || connectionError != 0) {
            lastSocketError = connectionError != 0 ? connectionError : WSAGetLastError();
            closesocket(descriptor);
            continue;
        }
        nonBlocking = 0;
        ioctlsocket(descriptor, FIONBIO, &nonBlocking);
#else
        if (descriptor < 0) {
            lastSocketError = errno;
            continue;
        }
#ifdef Q_OS_MACOS
        int noSigPipe = 1;
        setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
#endif
        const int originalFlags = fcntl(descriptor, F_GETFL, 0);
        fcntl(descriptor, F_SETFL, originalFlags | O_NONBLOCK);
        int result = ::connect(descriptor, address->ai_addr, address->ai_addrlen);
        if (result != 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
            lastSocketError = errno;
            ::close(descriptor);
            continue;
        }
        pollfd socketPoll{descriptor, POLLOUT, 0};
        result = ::poll(&socketPoll, 1, remaining);
        int connectionError = 0;
        socklen_t connectionErrorLength = sizeof(connectionError);
        if (result > 0) getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &connectionError, &connectionErrorLength);
        if (result <= 0 || connectionError != 0) {
            lastSocketError = connectionError != 0 ? connectionError : (result == 0 ? ETIMEDOUT : errno);
            ::close(descriptor);
            continue;
        }
        fcntl(descriptor, F_SETFL, originalFlags & ~O_NONBLOCK);
#endif
        m_socketDescriptor = static_cast<qintptr>(descriptor);
        freeaddrinfo(addresses);
        return true;
    }
    freeaddrinfo(addresses);
#ifdef Q_OS_WIN
    error = QStringLiteral("连接 %1:%2 失败（Socket 错误 %3）").arg(host).arg(port).arg(lastSocketError);
#else
    error = QStringLiteral("连接 %1:%2 失败：%3").arg(host).arg(port).arg(QString::fromLocal8Bit(std::strerror(lastSocketError)));
#endif
    return false;
}

bool Libssh2Worker::waitForSocket(int timeoutMs) const
{
    if (m_socketDescriptor < 0 || !m_session) return false;
    const int directions = libssh2_session_block_directions(m_session);
#ifdef Q_OS_WIN
    const auto descriptor = static_cast<SOCKET>(m_socketDescriptor);
    fd_set readSet;
    fd_set writeSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    if (directions & LIBSSH2_SESSION_BLOCK_INBOUND) FD_SET(descriptor, &readSet);
    if (directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) FD_SET(descriptor, &writeSet);
    timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    return select(0, &readSet, &writeSet, nullptr, &timeout) > 0;
#else
    short events = 0;
    if (directions & LIBSSH2_SESSION_BLOCK_INBOUND) events |= POLLIN;
    if (directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) events |= POLLOUT;
    pollfd socketPoll{static_cast<int>(m_socketDescriptor), events, 0};
    return ::poll(&socketPoll, 1, timeoutMs) > 0;
#endif
}

void Libssh2Worker::closeSocket()
{
    if (m_socketDescriptor < 0) return;
#ifdef Q_OS_WIN
    closesocket(static_cast<SOCKET>(m_socketDescriptor));
#else
    ::close(static_cast<int>(m_socketDescriptor));
#endif
    m_socketDescriptor = -1;
}

void Libssh2Worker::fail(const QString &stage, const QString &detail)
{
    const auto message = QStringLiteral("%1失败：%2").arg(stage, detail.isEmpty() ? QStringLiteral("未知错误") : detail);
    cleanup();
    emit connectionChanged(false, message);
    emit outputReceived(message + QLatin1Char('\n'));
}

QString Libssh2Worker::lastSessionError() const
{
    if (!m_session) return QStringLiteral("会话不可用");
    char *message = nullptr;
    int length = 0;
    const int code = libssh2_session_last_error(m_session, &message, &length, 0);
    if (message && length > 0) return QStringLiteral("%1 (libssh2 %2)").arg(QString::fromUtf8(message, length)).arg(code);
    return QStringLiteral("libssh2 错误 %1").arg(code);
}

QString Libssh2Worker::hostKeyAlgorithm() const
{
    if (!m_session) return QStringLiteral("unknown");
    size_t length = 0;
    int type = 0;
    libssh2_session_hostkey(m_session, &length, &type);
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA: return QStringLiteral("RSA");
    case LIBSSH2_HOSTKEY_TYPE_DSS: return QStringLiteral("DSS");
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return QStringLiteral("ECDSA-256");
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return QStringLiteral("ECDSA-384");
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return QStringLiteral("ECDSA-521");
    case LIBSSH2_HOSTKEY_TYPE_ED25519: return QStringLiteral("ED25519");
    default: return QStringLiteral("unknown");
    }
}

QString Libssh2Worker::normalizeFingerprint(const QString &fingerprint)
{
    auto value = fingerprint.trimmed();
    if (!value.startsWith(QStringLiteral("SHA256:"), Qt::CaseInsensitive) && !value.isEmpty()) value.prepend(QStringLiteral("SHA256:"));
    return value;
}

} // namespace noxshell
