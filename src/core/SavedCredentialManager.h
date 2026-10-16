#pragma once
#include "SavedCredential.h"
#include <QObject>
#include <QVector>

namespace noxshell {
class ServerRepository;
class CredentialStore;
class SavedCredentialManager final : public QObject {
    Q_OBJECT
public:
    SavedCredentialManager(ServerRepository *repository, CredentialStore *store, QObject *parent = nullptr);
    bool save(SavedCredential &value, const QString &password, const SavedCredential *expected = nullptr);
    bool remove(const SavedCredential &value);
    [[nodiscard]] QString lastError() const { return m_error; }
signals:
    void changed();
private:
    ServerRepository *m_repository;
    CredentialStore *m_store;
    QString m_error;
};
}
