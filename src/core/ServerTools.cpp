#include "ServerTools.h"
#include <QFile>
#include <QJsonDocument>
namespace noxshell::detail {
QByteArray serverToolCommand(const QJsonObject &options)
{
    const auto operation = options.value(QStringLiteral("operation")).toString();
    const bool packages = operation == "packages.search" || operation == "packages.plan"
        || operation == "packages.install" || operation == "packages.status";
    if (operation != "processes" && operation != "signal" && operation != "interfaces"
        && operation != "speed" && operation != "traffic" && !packages) return {};
    const auto json = QJsonDocument(options).toJson(QJsonDocument::Compact);
    if (json.size() > 2048) return {};
    QFile helper(packages ? QStringLiteral(":/tools/server_packages.py") : QStringLiteral(":/tools/server_tools.py"));
    if (!helper.open(QIODevice::ReadOnly)) return {};
    auto script = helper.readAll();
    script.replace("'", "'\\''");
    return "# NOXSHELL_SERVER_TOOLS_V1\nexec python3 -u -c '" + script + "' '" + json.toBase64() + "'";
}
}
