#include "CredentialStore.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <wincred.h>
#endif

namespace noxshell {

namespace {
constexpr auto kServiceName = "com.noxshell.ops.ssh";

QByteArray encodeSecret(const CredentialSecret &secret)
{
    QJsonObject object;
    object.insert(QStringLiteral("password"), secret.password);
    object.insert(QStringLiteral("keyPassphrase"), secret.keyPassphrase);
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toBase64();
}

CredentialSecret decodeSecret(const QByteArray &value)
{
    CredentialSecret secret;
    const auto document = QJsonDocument::fromJson(QByteArray::fromBase64(value.trimmed()));
    if (!document.isObject()) return secret;
    secret.password = document.object().value(QStringLiteral("password")).toString();
    secret.keyPassphrase = document.object().value(QStringLiteral("keyPassphrase")).toString();
    return secret;
}

#ifdef Q_OS_LINUX
QString secretToolPath()
{
    return QStandardPaths::findExecutable(QStringLiteral("secret-tool"));
}

QString processFailure(QProcess &process)
{
    const auto error = QString::fromUtf8(process.readAllStandardError()).trimmed();
    return error.isEmpty() ? process.errorString() : error;
}
#endif

#ifdef Q_OS_MACOS
bool readMacCredential(const char *service, const QString &reference, QByteArray &payload, QString &error)
{
    QProcess process;
    process.start(QStringLiteral("/usr/bin/security"), {
        QStringLiteral("find-generic-password"), QStringLiteral("-s"), QString::fromLatin1(service),
        QStringLiteral("-a"), reference, QStringLiteral("-w"),
    });
    if (!process.waitForFinished(10000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        error = QString::fromUtf8(process.readAllStandardError()).trimmed();
        return false;
    }
    payload = process.readAllStandardOutput();
    return true;
}
#elif defined(Q_OS_WIN)
bool readWindowsCredential(const char *service, const QString &reference, QByteArray &payload, DWORD &error)
{
    const auto target = QStringLiteral("%1/%2").arg(QString::fromLatin1(service), reference);
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(reinterpret_cast<const wchar_t *>(target.utf16()), CRED_TYPE_GENERIC, 0, &credential)) {
        error = GetLastError();
        return false;
    }
    payload = QByteArray(reinterpret_cast<const char *>(credential->CredentialBlob),
        static_cast<qsizetype>(credential->CredentialBlobSize));
    CredFree(credential);
    return true;
}
#elif defined(Q_OS_LINUX)
bool readLinuxCredential(const QString &executable, const char *service, const QString &reference,
    QByteArray &payload, QString &error)
{
    QProcess process;
    process.start(executable, {QStringLiteral("lookup"), QStringLiteral("service"), QString::fromLatin1(service),
        QStringLiteral("account"), reference});
    if (!process.waitForFinished(10000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        error = processFailure(process);
        return false;
    }
    payload = process.readAllStandardOutput();
    return true;
}
#endif
} // namespace

CredentialStore::CredentialStore(QObject *parent)
    : QObject(parent)
{
}

bool CredentialStore::save(const QString &reference, const CredentialSecret &secret)
{
    m_lastError.clear();
    if (reference.trimmed().isEmpty()) {
        m_lastError = QStringLiteral("凭据引用不能为空");
        return false;
    }
#ifdef Q_OS_MACOS
    QProcess process;
    const auto value = QString::fromLatin1(encodeSecret(secret));
    process.start(QStringLiteral("/usr/bin/security"), {
        QStringLiteral("add-generic-password"), QStringLiteral("-U"),
        QStringLiteral("-s"), QString::fromLatin1(kServiceName),
        QStringLiteral("-a"), reference,
        QStringLiteral("-w"), value,
    });
    if (!process.waitForFinished(10000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        m_lastError = QStringLiteral("写入 macOS Keychain 失败：%1").arg(QString::fromUtf8(process.readAllStandardError()).trimmed());
        return false;
    }
    return true;
#elif defined(Q_OS_WIN)
    const auto target = QStringLiteral("%1/%2").arg(QString::fromLatin1(kServiceName), reference);
    const auto payload = encodeSecret(secret);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(reinterpret_cast<const wchar_t *>(target.utf16()));
    credential.CredentialBlobSize = static_cast<DWORD>(payload.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(payload.constData()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<LPWSTR>(L"NoxShell");
    if (!CredWriteW(&credential, 0)) {
        m_lastError = QStringLiteral("写入 Windows Credential Manager 失败（错误 %1）").arg(GetLastError());
        return false;
    }
    return true;
#elif defined(Q_OS_LINUX)
    const auto executable = secretToolPath();
    if (executable.isEmpty()) {
        m_lastError = QStringLiteral("Linux Secret Service 不可用：请安装 libsecret-tools 并启动密钥环服务");
        return false;
    }
    QProcess process;
    process.start(executable, {QStringLiteral("store"), QStringLiteral("--label=玄壳 SSH"),
        QStringLiteral("service"), QString::fromLatin1(kServiceName), QStringLiteral("account"), reference});
    if (!process.waitForStarted(5000)) {
        m_lastError = QStringLiteral("启动 secret-tool 失败：%1").arg(process.errorString());
        return false;
    }
    process.write(encodeSecret(secret));
    process.closeWriteChannel();
    if (!process.waitForFinished(10000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        m_lastError = QStringLiteral("写入 Linux Secret Service 失败：%1").arg(processFailure(process));
        return false;
    }
    return true;
#else
    Q_UNUSED(reference);
    Q_UNUSED(secret);
    m_lastError = QStringLiteral("当前平台尚未实现系统凭据库");
    return false;
#endif
}

CredentialSecret CredentialStore::load(const QString &reference)
{
    m_lastError.clear();
    if (reference.trimmed().isEmpty()) {
        m_lastError = QStringLiteral("凭据引用不能为空");
        return {};
    }
#ifdef Q_OS_MACOS
    QByteArray payload;
    QString error;
    if (!readMacCredential(kServiceName, reference, payload, error)) {
        m_lastError = QStringLiteral("读取 macOS Keychain 失败：%1").arg(error);
        return {};
    }
    return decodeSecret(payload);
#elif defined(Q_OS_WIN)
    QByteArray payload;
    DWORD error = ERROR_SUCCESS;
    if (!readWindowsCredential(kServiceName, reference, payload, error)) {
        m_lastError = QStringLiteral("读取 Windows Credential Manager 失败（错误 %1）").arg(error);
        return {};
    }
    return decodeSecret(payload);
#elif defined(Q_OS_LINUX)
    const auto executable = secretToolPath();
    if (executable.isEmpty()) {
        m_lastError = QStringLiteral("Linux Secret Service 不可用：请安装 libsecret-tools 并启动密钥环服务");
        return {};
    }
    QByteArray payload;
    QString error;
    if (!readLinuxCredential(executable, kServiceName, reference, payload, error)) {
        m_lastError = QStringLiteral("读取 Linux Secret Service 失败：%1").arg(error);
        return {};
    }
    return decodeSecret(payload);
#else
    Q_UNUSED(reference);
    m_lastError = QStringLiteral("当前平台尚未实现系统凭据库");
    return {};
#endif
}

bool CredentialStore::remove(const QString &reference)
{
    m_lastError.clear();
    if (reference.trimmed().isEmpty()) return true;
#ifdef Q_OS_MACOS
    QProcess process;
    process.start(QStringLiteral("/usr/bin/security"), {
        QStringLiteral("delete-generic-password"), QStringLiteral("-s"), QString::fromLatin1(kServiceName),
        QStringLiteral("-a"), reference,
    });
    if (!process.waitForFinished(10000) || process.exitStatus() != QProcess::NormalExit) {
        m_lastError = QStringLiteral("删除 macOS Keychain 凭据失败：%1").arg(process.errorString());
        return false;
    }
    if (process.exitCode() != 0 && process.exitCode() != 44) {
        m_lastError = QStringLiteral("删除 macOS Keychain 凭据失败：%1")
                          .arg(QString::fromUtf8(process.readAllStandardError()).trimmed());
        return false;
    }
    return true;
#elif defined(Q_OS_WIN)
    const auto target = QStringLiteral("%1/%2").arg(QString::fromLatin1(kServiceName), reference);
    if (!CredDeleteW(reinterpret_cast<const wchar_t *>(target.utf16()), CRED_TYPE_GENERIC, 0)) {
        const auto error = GetLastError();
        if (error == ERROR_NOT_FOUND) return true;
        m_lastError = QStringLiteral("删除 Windows Credential Manager 凭据失败（错误 %1）").arg(error);
        return false;
    }
    return true;
#elif defined(Q_OS_LINUX)
    const auto executable = secretToolPath();
    if (executable.isEmpty()) {
        m_lastError = QStringLiteral("Linux Secret Service 不可用：请安装 libsecret-tools 并启动密钥环服务");
        return false;
    }
    QProcess process;
    process.start(executable, {QStringLiteral("clear"), QStringLiteral("service"), QString::fromLatin1(kServiceName),
        QStringLiteral("account"), reference});
    if (!process.waitForFinished(10000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        m_lastError = QStringLiteral("删除 Linux Secret Service 凭据失败：%1").arg(processFailure(process));
        return false;
    }
    return true;
#else
    Q_UNUSED(reference);
    m_lastError = QStringLiteral("当前平台尚未实现系统凭据库");
    return false;
#endif
}

} // namespace noxshell
