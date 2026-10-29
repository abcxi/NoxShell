#pragma once
#include <QByteArray>
#include <QJsonObject>
namespace noxshell::detail {
// Fixed helper with structured arguments; never accept an arbitrary shell command.
QByteArray serverToolCommand(const QJsonObject &options);
}
