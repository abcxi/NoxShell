#pragma once

#include "ServerProfile.h"

#include <QString>
#include <QStringList>

namespace noxshell {

enum class RdpClientPlatform {
    Windows,
    MacOS,
    Linux,
};

struct RdpLaunchSpec {
    QString program;
    QStringList arguments;
};

class RdpLauncher final {
public:
    [[nodiscard]] static QString endpoint(const ServerProfile &profile);
    [[nodiscard]] static QString connectionFileContents(const ServerProfile &profile);
    [[nodiscard]] static RdpLaunchSpec launchSpec(
        const ServerProfile &profile, RdpClientPlatform platform, const QString &linuxClient = {},
        const QString &connectionFile = {});
    static bool launch(const ServerProfile &profile, QString *error = nullptr);
};

} // namespace noxshell
