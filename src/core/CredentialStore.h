#pragma once

#include <QObject>
#include <QString>

namespace noxshell {

struct CredentialSecret {
    QString password;
    QString keyPassphrase;
};

class CredentialStore : public QObject {
    Q_OBJECT

public:
    explicit CredentialStore(QObject *parent = nullptr);

    virtual bool save(const QString &reference, const CredentialSecret &secret);
    // macOS operations never request system authorization UI, including writes.
    [[nodiscard]] virtual CredentialSecret load(const QString &reference);
    virtual bool remove(const QString &reference);
    [[nodiscard]] virtual QString lastError() const { return m_lastError; }
    [[nodiscard]] virtual bool authorizationRequired() const { return m_authorizationRequired; }

private:
    QString m_lastError;
    bool m_authorizationRequired{false};
};

} // namespace noxshell
