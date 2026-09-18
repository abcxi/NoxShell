#pragma once

#include <QByteArray>
#include <QString>

namespace noxshell::detail {
// Read-only Linux disk-usage scan. stdin is a cancellation/liveness channel.
QByteArray directorySizeCommand(const QString &path);
bool parseDirectorySize(const QByteArray &output, quint64 &bytes);
}
