#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <functional>

namespace noxshell {

struct CredentialSecret {
    QString password;
    QString keyPassphrase;
};

class CredentialStore : public QObject {
    Q_OBJECT

public:
    explicit CredentialStore(QObject *parent = nullptr);
#ifdef Q_OS_MACOS
    // Explicit storage/dependency injection for isolated tests, never an env override.
    using LegacyReader = std::function<bool(const QString &, QByteArray &, QString &, bool &)>;
    CredentialStore(QString vaultDirectory, QObject *parent);
    CredentialStore(QString vaultDirectory, LegacyReader legacyReader, QObject *parent = nullptr);
#endif

    virtual bool save(const QString &reference, const CredentialSecret &secret);
    // macOS operations never request system authorization UI, including writes.
    [[nodiscard]] virtual CredentialSecret load(const QString &reference);
    virtual bool remove(const QString &reference);
    [[nodiscard]] virtual QString lastError() const { return m_lastError; }
    [[nodiscard]] virtual bool authorizationRequired() const { return m_authorizationRequired; }

private:
#ifdef Q_OS_MACOS
    QString m_vaultDirectory;
    LegacyReader m_legacyReader;
#endif
    QString m_lastError;
    bool m_authorizationRequired{false};
};

} // namespace noxshell
