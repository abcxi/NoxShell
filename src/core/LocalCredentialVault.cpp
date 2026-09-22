#include "LocalCredentialVault.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>
#include <QUuid>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <sys/file.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace noxshell {
namespace {
constexpr qsizetype kLimit = 4 * 1024 * 1024;
constexpr int kKeySize = 32, kNonceSize = 12, kTagSize = 16;
const QByteArray kHeader("NOXVAULT\1", 9);
const QByteArray kKeyHeader("NOXKEY01", 8);
QMutex vaultMutex;

struct FileDescriptor {
    int value{-1};
    ~FileDescriptor() { if (value >= 0) ::close(value); }
};

struct SensitiveBytes {
    QByteArray value;
    ~SensitiveBytes() { if (!value.isEmpty()) OPENSSL_cleanse(value.data(), value.size()); }
};

bool secureStat(int fd, bool directory)
{
    const auto security = ::filesec_init();
    if (!security) return false;
    const auto cleanup = qScopeGuard([&] { ::filesec_free(security); });
    struct stat st{};
    const bool privateMode = ::fstatx_np(fd, &st, security) == 0 && st.st_uid == ::geteuid()
        && (st.st_mode & 0777) == (directory ? 0700 : 0600)
        && (directory ? S_ISDIR(st.st_mode) : (S_ISREG(st.st_mode) && st.st_nlink == 1));
    if (!privateMode) return false;
    // On macOS an extended ACL can grant access despite 0600/0700 mode bits.
    // Reject it rather than silently editing permissions or inherited ACLs.
    int hasAcl = 0;
    if (::filesec_query_property(security, FILESEC_ACL, &hasAcl) != 0) return false;
    if (!hasAcl) return true;
    acl_t acl = nullptr;
    if (::filesec_get_property(security, FILESEC_ACL, &acl) != 0 || !acl) return false;
    acl_entry_t entry = nullptr;
    const bool emptyAcl = ::acl_get_entry(acl, ACL_FIRST_ENTRY, &entry) == -1 && errno == EINVAL;
    ::acl_free(acl);
    return emptyAcl;
}

// All IO is relative to a verified private directory descriptor. Never follow
// symlinks, open devices/FIFOs, or modify existing permissions to gain access.
class Transaction final {
public:
    explicit Transaction(const QString &directory)
    {
        if (!QDir::isAbsolutePath(directory)) { fail(QStringLiteral("凭据目录必须是绝对路径")); return; }
        const auto path = QFile::encodeName(QDir::cleanPath(directory));
        if (::mkdir(path.constData(), 0700) != 0 && errno != EEXIST) {
            fail(QStringLiteral("无法创建本地凭据目录")); return;
        }
        dir.value = ::open(path.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (dir.value < 0 || !secureStat(dir.value, true)) {
            fail(QStringLiteral("本地凭据目录不安全：需本人所有、权限 0700、无额外 ACL 且不是符号链接")); return;
        }
        lock.value = ::openat(dir.value, "vault.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
        if (lock.value < 0 || !secureStat(lock.value, false)) {
            fail(QStringLiteral("本地凭据锁文件不安全或不可写")); return;
        }
        if (::flock(lock.value, LOCK_EX | LOCK_NB) != 0) {
            fail(QStringLiteral("本地凭据库正在被另一实例使用，请稍后重试")); return;
        }
        QByteArray envelope;
        bool dataMissing = false, keyMissing = false;
        if (!read("credentials.enc", envelope, dataMissing) || !read("master.key", key.value, keyMissing)) return;
        if (keyMissing && !dataMissing) {
            fail(QStringLiteral("本地加密密钥丢失；请恢复原 master.key，未覆盖现有凭据")); return;
        }
        if (!keyMissing) {
            if (key.value.size() != kKeyHeader.size() + kKeySize || !key.value.startsWith(kKeyHeader)) {
                fail(QStringLiteral("本地加密密钥格式损坏，未覆盖现有凭据")); return;
            }
            key.value.remove(0, kKeyHeader.size());
        }
        if (dataMissing) {
            if (!keyMissing) fail(QStringLiteral("本地凭据数据文件丢失；请恢复匹配的备份，未创建空库覆盖"));
            return;
        }
        SensitiveBytes plain;
        if (!decrypt(envelope, plain.value)) {
            fail(QStringLiteral("本地凭据校验失败：密钥不匹配或文件损坏，未覆盖现有凭据")); return;
        }
        const auto doc = QJsonDocument::fromJson(plain.value);
        if (!doc.isObject() || doc.object().value("version").toInt() != 1
            || !doc.object().value("entries").isObject()) {
            fail(QStringLiteral("本地凭据内容格式无效，未覆盖现有凭据")); return;
        }
        entries = doc.object().value("entries").toObject();
        for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
            if (it.key().isEmpty() || it.key().size() > 4096 || (!it->isNull() && !it->isString())
                || (it->isString() && !QByteArray::fromBase64Encoding(it->toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors))) {
                fail(QStringLiteral("本地凭据记录格式无效，未覆盖现有凭据")); return;
            }
        }
    }

    bool commit()
    {
        if (!error.isEmpty()) return false;
        SensitiveBytes plain;
        plain.value = QJsonDocument(QJsonObject{{"version", 1}, {"entries", entries}}).toJson(QJsonDocument::Compact);
        if (plain.value.size() > kLimit - 128) return fail(QStringLiteral("本地凭据库超过大小限制，未覆盖现有凭据"));
        if (key.value.isEmpty()) {
            key.value.resize(kKeySize);
            if (RAND_priv_bytes(reinterpret_cast<unsigned char *>(key.value.data()), kKeySize) != 1)
                return fail(QStringLiteral("无法生成安全随机密钥"));
            SensitiveBytes encodedKey;
            encodedKey.value = kKeyHeader + key.value;
            if (!atomicWrite("master.key", encodedKey.value, true)) return false;
        }
        QByteArray envelope;
        if (!encrypt(plain.value, envelope)) return fail(QStringLiteral("本地凭据加密失败"));
        return atomicWrite("credentials.enc", envelope, false);
    }

    QString error;
    QJsonObject entries;

private:
    FileDescriptor dir, lock;
    SensitiveBytes key;

    bool fail(const QString &message) { error = message; return false; }
    bool read(const char *name, QByteArray &bytes, bool &missing)
    {
        FileDescriptor file;
        file.value = ::openat(dir.value, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (file.value < 0) {
            missing = errno == ENOENT;
            return missing || fail(QStringLiteral("无法安全读取本地凭据文件"));
        }
        if (!secureStat(file.value, false)) return fail(QStringLiteral("本地凭据文件需本人所有、权限 0600、无额外 ACL 且不是链接"));
        struct stat st{};
        if (::fstat(file.value, &st) != 0 || st.st_size > kLimit || st.st_size < 0)
            return fail(QStringLiteral("本地凭据文件大小无效"));
        bytes.resize(st.st_size);
        qsizetype offset = 0;
        while (offset < bytes.size()) {
            const auto count = ::read(file.value, bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return fail(QStringLiteral("本地凭据文件读取不完整"));
            offset += count;
        }
        char extra;
        if (::read(file.value, &extra, 1) != 0) return fail(QStringLiteral("本地凭据文件在读取时发生变化"));
        return true;
    }

    bool atomicWrite(const char *name, const QByteArray &bytes, bool createOnly)
    {
        const auto temp = QByteArray(".vault-") + QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
        FileDescriptor file;
        file.value = ::openat(dir.value, temp.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (file.value < 0) return fail(QStringLiteral("无法创建本地凭据临时文件"));
        const auto cleanup = qScopeGuard([&] { ::unlinkat(dir.value, temp.constData(), 0); });
        if (::fchmod(file.value, 0600) != 0) return fail(QStringLiteral("无法保护本地凭据文件权限"));
        qsizetype offset = 0;
        while (offset < bytes.size()) {
            const auto count = ::write(file.value, bytes.constData() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return fail(QStringLiteral("写入本地凭据失败，旧文件未替换"));
            offset += count;
        }
        if (::fsync(file.value) != 0) return fail(QStringLiteral("同步本地凭据失败，旧文件未替换"));
        // linkat implements exclusive creation for the key: never replace it.
        const int result = createOnly ? ::linkat(dir.value, temp.constData(), dir.value, name, 0)
                                      : ::renameat(dir.value, temp.constData(), dir.value, name);
        if (result != 0) return fail(QStringLiteral("提交本地凭据失败，旧文件未替换"));
        if (createOnly && ::unlinkat(dir.value, temp.constData(), 0) != 0)
            return fail(QStringLiteral("本地密钥提交未完成，请检查目录权限"));
        if (::fsync(dir.value) != 0) return fail(QStringLiteral("凭据已写入，但目录同步失败；请检查磁盘"));
        return true;
    }

    using Cipher = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
    bool encrypt(const QByteArray &plain, QByteArray &envelope)
    {
        QByteArray nonce(kNonceSize, '\0'), cipher(plain.size() + 16, '\0'), tag(kTagSize, '\0');
        if (RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), nonce.size()) != 1) return false;
        Cipher ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
        int len = 0, finalLen = 0;
        if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr,
                reinterpret_cast<const unsigned char *>(key.value.constData()),
                reinterpret_cast<const unsigned char *>(nonce.constData())) != 1
            || EVP_EncryptUpdate(ctx.get(), nullptr, &len,
                reinterpret_cast<const unsigned char *>(kHeader.constData()), kHeader.size()) != 1
            || EVP_EncryptUpdate(ctx.get(), reinterpret_cast<unsigned char *>(cipher.data()), &len,
                reinterpret_cast<const unsigned char *>(plain.constData()), plain.size()) != 1
            || EVP_EncryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(cipher.data()) + len, &finalLen) != 1
            || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data()) != 1) return false;
        cipher.resize(len + finalLen);
        envelope = kHeader + nonce + cipher + tag;
        return true;
    }

    bool decrypt(const QByteArray &envelope, QByteArray &plain)
    {
        if (!envelope.startsWith(kHeader) || envelope.size() < kHeader.size() + kNonceSize + kTagSize) return false;
        const int cipherSize = envelope.size() - kHeader.size() - kNonceSize - kTagSize;
        Cipher ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
        plain.resize(cipherSize + 16);
        int len = 0, finalLen = 0;
        auto tag = envelope.right(kTagSize);
        if (!ctx || EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr,
                reinterpret_cast<const unsigned char *>(key.value.constData()),
                reinterpret_cast<const unsigned char *>(envelope.constData() + kHeader.size())) != 1
            || EVP_DecryptUpdate(ctx.get(), nullptr, &len,
                reinterpret_cast<const unsigned char *>(kHeader.constData()), kHeader.size()) != 1
            || EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char *>(plain.data()), &len,
                reinterpret_cast<const unsigned char *>(envelope.constData() + kHeader.size() + kNonceSize), cipherSize) != 1
            || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, tag.size(), tag.data()) != 1
            || EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(plain.data()) + len, &finalLen) != 1) return false;
        plain.resize(len + finalLen); // Never parse/use plaintext before authentication succeeds.
        return true;
    }
};

bool validReference(const QString &reference)
{
    return !reference.trimmed().isEmpty() && reference.size() <= 4096;
}
} // namespace

LocalCredentialVault::LocalCredentialVault(QString directory) : m_directory(std::move(directory)) {}

QString LocalCredentialVault::defaultDirectory() { return QDir::home().filePath(QStringLiteral(".noxshell")); }

LocalCredentialVault::Result LocalCredentialVault::load(const QString &reference) const
{
    if (!validReference(reference)) return {Status::Error, {}, QStringLiteral("凭据引用无效")};
    QMutexLocker guard(&vaultMutex);
    Transaction tx(m_directory);
    if (!tx.error.isEmpty()) return {Status::Error, {}, tx.error};
    if (!tx.entries.contains(reference)) return {Status::Missing, {}, {}};
    const auto value = tx.entries.value(reference);
    if (value.isNull()) return {Status::Removed, {}, {}};
    return {Status::Found, QByteArray::fromBase64(value.toString().toLatin1()), {}};
}

bool LocalCredentialVault::save(const QString &reference, const QByteArray &payload, QString &error, bool onlyIfMissing) const
{
    error.clear();
    if (!validReference(reference) || payload.isEmpty() || payload.size() > 65536) {
        error = QStringLiteral("凭据引用或内容无效"); return false;
    }
    QMutexLocker guard(&vaultMutex);
    Transaction tx(m_directory);
    if (tx.error.isEmpty() && !(onlyIfMissing && tx.entries.contains(reference))) {
        tx.entries.insert(reference, QString::fromLatin1(payload.toBase64()));
        tx.commit();
    }
    error = tx.error;
    return error.isEmpty();
}

bool LocalCredentialVault::remove(const QString &reference, QString &error) const
{
    error.clear();
    if (!validReference(reference)) { error = QStringLiteral("凭据引用无效"); return false; }
    QMutexLocker guard(&vaultMutex);
    Transaction tx(m_directory);
    if (tx.error.isEmpty()) { tx.entries.insert(reference, QJsonValue::Null); tx.commit(); }
    error = tx.error;
    return error.isEmpty();
}
} // namespace noxshell
