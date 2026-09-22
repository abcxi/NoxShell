// Two separately compiled executables test upgrades, not an in-process cache.
// Synthetic data only; no default vault, SSH connection or legacy Keychain read.
#include "../src/core/CredentialStore.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 2 && app.arguments().at(1) == "--identity") {
        std::puts(NOXSHELL_VAULT_BUILD);
        return 0;
    }
    if (argc != 5) return 2;
    const auto directory = app.arguments().at(2);
    const auto parent = QFileInfo(directory).dir().canonicalPath();
    if (!parent.startsWith("/private/tmp/noxshell-vault-test-")
        || QFileInfo(parent).path() != "/private/tmp" || QFileInfo(directory).fileName() != "vault") return 2;
    const auto operation = app.arguments().at(1), reference = app.arguments().at(3), suffix = app.arguments().at(4);
    noxshell::CredentialStore store(directory, noxshell::CredentialStore::LegacyReader{}, nullptr);
    const noxshell::CredentialSecret secret{QStringLiteral(" synthetic-only 中文 ! $() ") + suffix, QStringLiteral("test-passphrase")};
    if (operation == "write" || operation == "many") {
        const int count = operation == "many" ? 12 : 1;
        for (int i = 0; i < count; ++i) {
            const auto ref = operation == "many" ? reference + QString::number(i) : reference;
            bool ok = false;
            for (int attempt = 0; attempt < 100 && !ok; ++attempt) {
                ok = store.save(ref, secret);
                if (!ok) QThread::msleep(10);
            }
            if (!ok) return 3;
        }
        return 0;
    }
    if (operation == "read") {
        const auto saved = store.load(reference);
        return store.lastError().isEmpty() && saved.password == secret.password && saved.keyPassphrase == secret.keyPassphrase ? 0 : 4;
    }
    return 2;
}
