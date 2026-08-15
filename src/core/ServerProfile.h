#pragma once

#include <QString>
#include <QMetaType>

namespace noxshell {

enum class ServerState {
    Online,
    Warning,
    Offline,
};

enum class ConnectionMode {
    Demo,
    Ssh,
};

enum class AuthenticationMethod {
    Password,
    PrivateKey,
    SshAgent,
};

struct ServerProfile {
    QString name;
    QString host;
    QString user;
    QString os;
    QString group;
    ServerState state{ServerState::Offline};
    quint16 port{22};
    ConnectionMode connectionMode{ConnectionMode::Demo};
    AuthenticationMethod authentication{AuthenticationMethod::Password};
    QString password;
    QString privateKeyPath;
    QString publicKeyPath;
    QString keyPassphrase;
    QString expectedFingerprint;
    QString id;
    QString credentialRef;
};

inline QString stateText(ServerState state)
{
    switch (state) {
    case ServerState::Online:
        return QStringLiteral("在线");
    case ServerState::Warning:
        return QStringLiteral("警告");
    case ServerState::Offline:
        return QStringLiteral("离线");
    }
    return {};
}

} // namespace noxshell

Q_DECLARE_METATYPE(noxshell::ServerProfile)
