#pragma once

#include <QObject>
#include <QString>

namespace noxshell {

struct CredentialSecret {
    QString password;
    QString keyPassphrase;
};

class CredentialStore final : public QObject {
    Q_OBJECT

public:
    explicit CredentialStore(QObject *parent = nullptr);

    bool save(const QString &reference, const CredentialSecret &secret);
    [[nodiscard]] CredentialSecret load(const QString &reference);
    bool remove(const QString &reference);
    [[nodiscard]] QString lastError() const { return m_lastError; }

private:
    QString m_lastError;
};

} // namespace noxshell

