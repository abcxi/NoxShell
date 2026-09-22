#pragma once

#include <QByteArray>
#include <QString>

namespace noxshell {

// macOS local, same-user vault. The random key is deliberately independent of
// bundle identity. This does NOT protect against code running as the same user.
class LocalCredentialVault final {
public:
    enum class Status { Found, Missing, Removed, Error };
    struct Result { Status status; QByteArray payload; QString error; };

    explicit LocalCredentialVault(QString directory);
    static QString defaultDirectory();
    Result load(const QString &reference) const;
    bool save(const QString &reference, const QByteArray &payload, QString &error,
        bool onlyIfMissing = false) const;
    // An encrypted tombstone prevents a removed legacy item being re-imported.
    bool remove(const QString &reference, QString &error) const;

private:
    QString m_directory;
};

} // namespace noxshell
