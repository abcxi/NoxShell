#pragma once
#include <QString>

namespace noxshell {
// Metadata only. Passwords remain in CredentialStore, never in SQLite.
struct SavedCredential {
    QString id;
    QString name;
    QString user;
    QString credentialRef;
};
}
