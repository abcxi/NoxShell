#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QVector>

namespace noxshell {

struct RemoteFileEntry {
    QString name;
    QString path;
    QString owner;
    QString group;
    quint64 size{};
    quint64 userId{};
    quint64 groupId{};
    QDateTime modifiedAt;
    bool directory{false};
    bool symbolicLink{false};
    bool ownerIdsValid{false};
    quint32 permissions{};
};

using RemoteFileEntries = QVector<RemoteFileEntry>;

enum class RemoteFileOperation {
    Upload,
    Download,
    MakeDirectory,
    Rename,
    Remove,
};

} // namespace noxshell

Q_DECLARE_METATYPE(noxshell::RemoteFileEntry)
Q_DECLARE_METATYPE(noxshell::RemoteFileEntries)
Q_DECLARE_METATYPE(noxshell::RemoteFileOperation)
