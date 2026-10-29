#pragma once
#include <QNetworkProxy>
#include <functional>

namespace noxshell {
// Negotiates on the native transport socket before libssh2 takes ownership.
bool negotiateProxyTunnel(qintptr socket, const QNetworkProxy &proxy, const QString &host,
    quint16 port, int timeoutMs, const std::function<bool()> &canceled, QString &error);
}
