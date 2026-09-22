#include "../src/core/LocalCredentialVault.h"
#include "../src/core/CredentialStore.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace noxshell;
namespace {
QByteArray contents(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
bool writeFile(const QString &path, const QByteArray &data) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(data) == data.size();
}
QString probe(const char *build) { return QCoreApplication::applicationDirPath() + "/NoxShellVaultProbe" + build; }
QByteArray legacyPayload() {
    return QJsonDocument(QJsonObject{{"password", "synthetic-old"}, {"keyPassphrase", "synthetic-key"}}).toJson().toBase64();
}
}

class LocalVaultTest final : public QObject {
    Q_OBJECT
private slots:
    void stablePathAndRoundTrip()
    {
        const auto productionPath = LocalCredentialVault::defaultDirectory();
        QCoreApplication::setApplicationName("renamed-upgrade.app");
        QCoreApplication::setApplicationVersion("999");
        QCOMPARE(LocalCredentialVault::defaultDirectory(), productionPath);
        QCOMPARE(productionPath, QDir::home().filePath(".noxshell")); // No IO to this path.
        QTemporaryDir temporary;
        const auto path = temporary.filePath("vault");
        CredentialStore store(path, CredentialStore::LegacyReader{}, nullptr);
        CredentialSecret value{QStringLiteral(" leading spaces 中文 \"' $(); \n"), QStringLiteral("keyphrase 空格 ")};
        QVERIFY2(store.save("server/test", value), qPrintable(store.lastError()));
        const auto first = contents(path + "/credentials.enc"), key = contents(path + "/master.key");
        QVERIFY(!first.contains(value.password.toUtf8()));
        QVERIFY(!first.contains(value.keyPassphrase.toUtf8()));
        QVERIFY(!first.contains("server/test"));
        QCOMPARE(key.size(), 40);
        QVERIFY(store.save("server/test", value));
        QVERIFY(first != contents(path + "/credentials.enc"));
        QCOMPARE(key, contents(path + "/master.key"));
        CredentialStore restarted(path, CredentialStore::LegacyReader{}, nullptr);
        QCOMPARE(restarted.load("server/test").password, value.password);
        QCOMPARE(restarted.load("server/test").keyPassphrase, value.keyPassphrase);
        struct stat st{};
        QVERIFY(::stat(QFile::encodeName(path).constData(), &st) == 0);
        QCOMPARE(st.st_mode & 0777, 0700);
        for (const auto &name : {"master.key", "credentials.enc", "vault.lock"}) {
            QVERIFY(::stat(QFile::encodeName(path + '/' + name).constData(), &st) == 0);
            QCOMPARE(st.st_mode & 0777, 0600);
        }
        QVERIFY(restarted.remove("server/test"));
        QVERIFY(restarted.load("server/test").password.isEmpty());
        QCOMPARE(LocalCredentialVault(path).load("server/test").status, LocalCredentialVault::Status::Removed);
    }

    void differentBuildsAndConcurrentInstances()
    {
        QTemporaryDir temporary("/private/tmp/noxshell-vault-test-XXXXXX");
        QVERIFY(temporary.isValid());
        const auto path = temporary.filePath("vault");
        QVERIFY(QCryptographicHash::hash(contents(probe("A")), QCryptographicHash::Sha256)
            != QCryptographicHash::hash(contents(probe("B")), QCryptographicHash::Sha256));
        const auto run = [&](const char *build, const QString &operation, const QString &ref, const QString &value) {
            QProcess p;
            p.start(probe(build), {operation, path, ref, value});
            return p.waitForFinished(10000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
        };
        QVERIFY(run("A", "write", "upgrade", "A"));
        QVERIFY(run("B", "read", "upgrade", "A"));
        QVERIFY(run("B", "write", "upgrade", "B"));
        QVERIFY(run("A", "read", "upgrade", "B"));
        QVERIFY(run("B", "read", "upgrade", "B"));
        QProcess a, b;
        a.start(probe("A"), {"many", path, "a-", "A"});
        b.start(probe("B"), {"many", path, "b-", "B"});
        QVERIFY(a.waitForFinished(10000)); QVERIFY(b.waitForFinished(10000));
        QCOMPARE(a.exitCode(), 0); QCOMPARE(b.exitCode(), 0);
        CredentialStore store(path, CredentialStore::LegacyReader{}, nullptr);
        for (int i = 0; i < 12; ++i) {
            QVERIFY(store.load("a-" + QString::number(i)).password.endsWith('A'));
            QVERIFY(store.load("b-" + QString::number(i)).password.endsWith('B'));
        }
    }

    void corruptFilesFailClosed_data()
    {
        QTest::addColumn<QString>("damage");
        for (const auto &value : {"missing-key", "missing-data", "truncated-key", "wrong-key", "header", "nonce", "cipher", "tag", "truncated", "oversize", "permissions", "directory-permissions", "acl"})
            QTest::newRow(value) << QString::fromLatin1(value);
    }
    void corruptFilesFailClosed()
    {
        QFETCH(QString, damage);
        QTemporaryDir temporary;
        const auto path = temporary.filePath("vault");
        CredentialStore store(path, CredentialStore::LegacyReader{}, nullptr);
        QVERIFY(store.save("test", {"synthetic", {}}));
        const auto keyPath = path + "/master.key", dataPath = path + "/credentials.enc";
        if (damage == "missing-key") QVERIFY(QFile::remove(keyPath));
        else if (damage == "missing-data") QVERIFY(QFile::remove(dataPath));
        else if (damage == "truncated-key") QVERIFY(writeFile(keyPath, "bad"));
        else if (damage == "wrong-key") {
            auto key = contents(keyPath); key[10] = char(key[10] ^ 1); QVERIFY(writeFile(keyPath, key));
        } else if (damage == "permissions") QVERIFY(::chmod(QFile::encodeName(dataPath).constData(), 0644) == 0);
        else if (damage == "directory-permissions") QVERIFY(::chmod(QFile::encodeName(path).constData(), 0755) == 0);
        else if (damage == "acl") QCOMPARE(QProcess::execute("/bin/chmod", {"+a", "everyone allow read", dataPath}), 0);
        else {
            auto data = contents(dataPath);
            if (damage == "truncated") data.truncate(20);
            else if (damage == "oversize") data.resize(4 * 1024 * 1024 + 1, 'x');
            else { const int offset = damage == "header" ? 0 : damage == "nonce" ? 10 : damage == "cipher" ? 25 : data.size() - 1;
                data[offset] = char(data[offset] ^ 1); }
            QVERIFY(writeFile(dataPath, data));
        }
        const auto keyBefore = contents(keyPath), dataBefore = contents(dataPath);
        int legacyCalls = 0;
        CredentialStore reading(path, [&](const QString &, QByteArray &, QString &, bool &) { ++legacyCalls; return false; });
        QVERIFY(reading.load("test").password.isEmpty());
        QVERIFY(!reading.lastError().isEmpty());
        QVERIFY(!reading.save("new", {"replacement", {}}));
        QVERIFY(!reading.remove("test"));
        QCOMPARE(legacyCalls, 0);
        QCOMPARE(contents(keyPath), keyBefore);
        QCOMPARE(contents(dataPath), dataBefore);
        if (damage == "missing-key") QVERIFY(!QFile::exists(keyPath));
        if (damage == "missing-data") QVERIFY(!QFile::exists(dataPath));
    }

    void symlinkAndHardlinkRejected_data()
    {
        QTest::addColumn<QString>("name");
        for (const auto &value : {"master.key", "credentials.enc", "vault.lock", "directory", "hardlink"}) QTest::newRow(value) << QString::fromLatin1(value);
    }
    void symlinkAndHardlinkRejected()
    {
        QFETCH(QString, name);
        QTemporaryDir temporary;
        auto path = temporary.filePath("vault");
        CredentialStore store(path, CredentialStore::LegacyReader{}, nullptr);
        QVERIFY(store.save("test", {"original", {}}));
        const auto oldData = contents(path + "/credentials.enc");
        if (name == "directory") {
            const auto alias = temporary.filePath("alias");
            QVERIFY(QFile::link(path, alias)); path = alias;
        } else if (name == "hardlink") {
            QVERIFY(::link(QFile::encodeName(path + "/master.key").constData(), QFile::encodeName(temporary.filePath("copy-key")).constData()) == 0);
        } else {
            const auto original = path + '/' + name;
            const auto moved = temporary.filePath(name);
            QVERIFY(QFile::rename(original, moved)); QVERIFY(QFile::link(moved, original));
        }
        CredentialStore untrusted(path, CredentialStore::LegacyReader{}, nullptr);
        QVERIFY(untrusted.load("test").password.isEmpty());
        QVERIFY(!untrusted.save("test", {"replacement", {}}));
        QCOMPARE(contents(path + "/credentials.enc"), oldData);
    }

    void migrationIsLazyAndDoesNotResurrectOrOverwrite()
    {
        QTemporaryDir temporary;
        const auto path = temporary.filePath("vault");
        int reads = 0;
        CredentialStore store(path, [&](const QString &, QByteArray &payload, QString &, bool &) {
            ++reads; payload = legacyPayload(); return true;
        });
        QCOMPARE(reads, 0);
        QCOMPARE(store.load("legacy").password, QStringLiteral("synthetic-old"));
        QCOMPARE(reads, 1);
        QCOMPARE(store.load("legacy").keyPassphrase, QStringLiteral("synthetic-key"));
        QCOMPARE(reads, 1);
        QVERIFY(store.remove("legacy"));
        QVERIFY(store.load("legacy").password.isEmpty());
        QCOMPARE(reads, 1);
        CredentialStore concurrent(path, [&](const QString &reference, QByteArray &payload, QString &, bool &) {
            CredentialStore writer(path, CredentialStore::LegacyReader{}, nullptr);
            if (!writer.save(reference, {"newer", {}})) return false;
            payload = legacyPayload(); return true;
        });
        QCOMPARE(concurrent.load("race").password, QStringLiteral("newer"));
        CredentialStore deleting(path, [&](const QString &reference, QByteArray &payload, QString &, bool &) {
            CredentialStore writer(path, CredentialStore::LegacyReader{}, nullptr);
            if (!writer.remove(reference)) return false;
            payload = legacyPayload(); return true;
        });
        QVERIFY(deleting.load("deleted-race").password.isEmpty());
        QCOMPARE(LocalCredentialVault(path).load("deleted-race").status, LocalCredentialVault::Status::Removed);
        CredentialStore denied(path, [&](const QString &, QByteArray &, QString &error, bool &authorization) {
            ++reads; error = "synthetic-denied"; authorization = true; return false;
        });
        QVERIFY(denied.load("unreadable").password.isEmpty());
        QVERIFY(denied.authorizationRequired());
        QVERIFY(denied.save("unreadable", {"entered-once", {}}));
        QVERIFY(!denied.authorizationRequired());
        QCOMPARE(denied.load("unreadable").password, QStringLiteral("entered-once"));
        QCOMPARE(reads, 2);
    }

    void invalidLegacyPayloadDoesNotCreateCredentials()
    {
        QTemporaryDir temporary;
        const auto path = temporary.filePath("vault");
        CredentialStore store(path, [](const QString &, QByteArray &payload, QString &, bool &) {
            payload = "invalid-legacy-data"; return true;
        });
        QVERIFY(store.load("test").password.isEmpty());
        QVERIFY(!store.lastError().isEmpty());
        QVERIFY(!QFile::exists(path + "/master.key"));
        QVERIFY(!QFile::exists(path + "/credentials.enc"));
    }

    void lockedVaultAndInvalidInputPreserveData()
    {
        QTemporaryDir temporary;
        const auto path = temporary.filePath("vault");
        CredentialStore store(path, CredentialStore::LegacyReader{}, nullptr);
        QVERIFY(store.save("test", {"original", {}}));
        const auto before = contents(path + "/credentials.enc");
        const int fd = ::open(QFile::encodeName(path + "/vault.lock").constData(), O_RDWR);
        QVERIFY(fd >= 0);
        const auto cleanup = qScopeGuard([&] { ::close(fd); });
        QVERIFY(::flock(fd, LOCK_EX | LOCK_NB) == 0);
        QVERIFY(!store.save("test", {"replacement", {}}));
        QVERIFY(store.lastError().contains(QStringLiteral("另一实例")));
        QCOMPARE(contents(path + "/credentials.enc"), before);
        QVERIFY(::flock(fd, LOCK_UN) == 0);
        QVERIFY(!store.save("", {"replacement", {}}));
        QVERIFY(!store.save("test", {QString(70000, 'x'), {}}));
        QCOMPARE(contents(path + "/credentials.enc"), before);
        QCOMPARE(store.load("test").password, QStringLiteral("original"));
    }
};
QTEST_GUILESS_MAIN(LocalVaultTest)
#include "LocalVaultTest.moc"
