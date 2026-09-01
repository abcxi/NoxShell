#include "RdpLauncher.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <wincred.h>
#endif

namespace noxshell {

QString RdpLauncher::endpoint(const ServerProfile &profile)
{
    auto host = profile.host.trimmed();
    if (host.contains(QLatin1Char(':')) && !host.startsWith(QLatin1Char('['))) {
        host = QStringLiteral("[%1]").arg(host);
    }
    return QStringLiteral("%1:%2").arg(host).arg(profile.port == 0 ? 3389 : profile.port);
}

QString RdpLauncher::connectionFileContents(const ServerProfile &profile)
{
    QStringList lines{
        QStringLiteral("screen mode id:i:2"),
        QStringLiteral("session bpp:i:32"),
        QStringLiteral("full address:s:%1").arg(endpoint(profile)),
        QStringLiteral("prompt for credentials:i:%1").arg(profile.password.isEmpty() ? 1 : 0),
        QStringLiteral("authentication level:i:2"),
        QStringLiteral("enablecredsspsupport:i:1"),
        QStringLiteral("redirectclipboard:i:1"),
        QStringLiteral("audiomode:i:0"),
    };
    if (!profile.user.trimmed().isEmpty()) {
        lines.insert(3, QStringLiteral("username:s:%1").arg(profile.user.trimmed()));
    }
    return lines.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n");
}

namespace {
bool writeUtf16LeFile(const QString &path, const QString &contents, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("无法创建远程桌面配置：%1").arg(file.errorString());
        return false;
    }
    QByteArray encoded;
    encoded.reserve(2 + contents.size() * 2);
    encoded.append('\xFF');
    encoded.append('\xFE');
    for (const QChar character : contents) {
        const auto code = character.unicode();
        encoded.append(static_cast<char>(code & 0xFF));
        encoded.append(static_cast<char>((code >> 8) & 0xFF));
    }
    if (file.write(encoded) != encoded.size()) {
        if (error) *error = QStringLiteral("写入远程桌面配置失败：%1").arg(file.errorString());
        return false;
    }
    return true;
}

QString createConnectionFile(const ServerProfile &profile, QString *error)
{
    const auto directory = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                               .filePath(QStringLiteral("noxshell-rdp"));
    if (!QDir().mkpath(directory)) {
        if (error) *error = QStringLiteral("无法创建远程桌面临时目录。");
        return {};
    }
    const auto identity = profile.id.isEmpty() ? RdpLauncher::endpoint(profile) : profile.id;
    const auto digest = QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    const auto path = QDir(directory).filePath(QStringLiteral("noxshell-%1.rdp").arg(QString::fromLatin1(digest)));
    return writeUtf16LeFile(path, RdpLauncher::connectionFileContents(profile), error) ? path : QString{};
}

#ifdef Q_OS_WIN
bool cacheWindowsRdpCredential(const ServerProfile &profile, QString *error)
{
    if (profile.password.isEmpty()) return true;
    const auto target = QStringLiteral("TERMSRV/%1").arg(profile.host.trimmed());
    const auto passwordBytes = static_cast<DWORD>(profile.password.size() * sizeof(wchar_t));
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_DOMAIN_PASSWORD;
    credential.TargetName = const_cast<LPWSTR>(reinterpret_cast<const wchar_t *>(target.utf16()));
    credential.CredentialBlobSize = passwordBytes;
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(
        const_cast<ushort *>(profile.password.utf16()));
    credential.Persist = CRED_PERSIST_SESSION;
    auto user = profile.user.trimmed();
    credential.UserName = const_cast<LPWSTR>(reinterpret_cast<const wchar_t *>(user.utf16()));
    if (CredWriteW(&credential, 0)) return true;
    if (error) *error = QStringLiteral("写入临时 Windows RDP 凭据失败（错误 %1）。").arg(GetLastError());
    return false;
}
#endif
} // namespace

RdpLaunchSpec RdpLauncher::launchSpec(
    const ServerProfile &profile, RdpClientPlatform platform, const QString &linuxClient,
    const QString &connectionFile)
{
    RdpLaunchSpec spec;
    const auto target = endpoint(profile);
    switch (platform) {
    case RdpClientPlatform::Windows:
        spec.program = QStringLiteral("mstsc.exe");
        spec.arguments = connectionFile.isEmpty()
            ? QStringList{QStringLiteral("/v:%1").arg(target), QStringLiteral("/prompt")}
            : QStringList{connectionFile};
        break;
    case RdpClientPlatform::MacOS: {
        QByteArray uri("rdp://full%20address=s:");
        uri += QUrl::toPercentEncoding(target, QByteArray(":[]"));
        if (!profile.user.trimmed().isEmpty()) {
            uri += "&username=s:";
            uri += QUrl::toPercentEncoding(profile.user.trimmed(), QByteArray("@"));
        }
        uri += "&prompt%20for%20credentials%20on%20client=i:1";
        // Microsoft's legacy macOS RDP URI is intentionally not a normal
        // RFC URL (the attribute list directly follows rdp://). QUrl treats
        // its first colon as a malformed authority port, so pass the exact
        // documented value to Launch Services instead of letting Qt rewrite it.
        spec.program = QStringLiteral("/usr/bin/open");
        spec.arguments = connectionFile.isEmpty()
            ? QStringList{QStringLiteral("-b"), QStringLiteral("com.microsoft.rdc.macos"), QString::fromLatin1(uri)}
            : QStringList{QStringLiteral("-b"), QStringLiteral("com.microsoft.rdc.macos"), connectionFile};
        break;
    }
    case RdpClientPlatform::Linux:
        spec.program = linuxClient;
        spec.arguments = {
            QStringLiteral("/v:%1").arg(target),
            QStringLiteral("/dynamic-resolution"),
            QStringLiteral("/cert:tofu"),
        };
        if (!profile.user.trimmed().isEmpty()) {
            spec.arguments.append(QStringLiteral("/u:%1").arg(profile.user.trimmed()));
        }
        break;
    }
    return spec;
}

bool RdpLauncher::launch(const ServerProfile &profile, QString *error)
{
    if (profile.host.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("远程桌面地址不能为空。");
        return false;
    }

#if defined(Q_OS_WIN)
    const auto connectionFile = createConnectionFile(profile, error);
    if (connectionFile.isEmpty() || !cacheWindowsRdpCredential(profile, error)) return false;
    const auto spec = launchSpec(profile, RdpClientPlatform::Windows, {}, connectionFile);
    if (QProcess::startDetached(spec.program, spec.arguments)) return true;
    if (error) *error = QStringLiteral("无法启动 Windows 远程桌面连接（mstsc.exe）。");
#elif defined(Q_OS_MACOS)
    auto macProfile = profile;
    // Windows App does not expose an API for importing a plaintext password.
    // Keep its native credential prompt enabled even when NoxShell has a
    // Keychain password ready for the user to paste.
    macProfile.password.clear();
    const auto connectionFile = createConnectionFile(macProfile, error);
    if (connectionFile.isEmpty()) return false;
    const auto spec = launchSpec(profile, RdpClientPlatform::MacOS, {}, connectionFile);
    QProcess process;
    process.start(spec.program, spec.arguments);
    if (process.waitForFinished(10000) && process.exitStatus() == QProcess::NormalExit
        && process.exitCode() == 0) return true;
    if (error) {
        const auto detail = QString::fromUtf8(process.readAllStandardError()).trimmed();
        *error = detail.isEmpty()
            ? QStringLiteral("无法打开 Microsoft Windows App，请确认应用已经安装。")
            : QStringLiteral("无法打开 Microsoft Windows App：%1").arg(detail);
    }
#else
    auto client = QStandardPaths::findExecutable(QStringLiteral("xfreerdp3"));
    if (client.isEmpty()) client = QStandardPaths::findExecutable(QStringLiteral("xfreerdp"));
    if (client.isEmpty()) {
        if (error) *error = QStringLiteral("未找到 xfreerdp3 或 xfreerdp，请先安装 FreeRDP 客户端。");
        return false;
    }
    const auto spec = launchSpec(profile, RdpClientPlatform::Linux, client);
    if (QProcess::startDetached(spec.program, spec.arguments)) return true;
    if (error) *error = QStringLiteral("FreeRDP 客户端启动失败。");
#endif
    return false;
}

} // namespace noxshell
