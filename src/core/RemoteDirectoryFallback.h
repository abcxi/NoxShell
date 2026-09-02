#pragma once

#include "RemoteFileEntry.h"

#include <QByteArray>
#include <QString>

namespace noxshell::detail {

[[nodiscard]] QByteArray fallbackHomeDirectoryCommand();
[[nodiscard]] QByteArray fallbackDirectoryListingCommand(const QString &path);
[[nodiscard]] bool parseFallbackDirectoryListing(const QString &path, const QByteArray &payload,
    RemoteFileEntries &entries, QString &failure);

} // namespace noxshell::detail
