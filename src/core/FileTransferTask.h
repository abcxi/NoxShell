#pragma once

#include "RemoteFileEntry.h"

#include <QMetaType>
#include <QString>

namespace noxshell {

enum class TransferState {
    Queued,
    Running,
    Completed,
    Failed,
    Canceled,
};

struct FileTransferTask {
    quint64 id{};
    RemoteFileOperation operation{RemoteFileOperation::Upload};
    QString localPath;
    QString remotePath;
    quint64 completed{};
    quint64 total{};
    TransferState state{TransferState::Queued};
    QString message;
};

} // namespace noxshell

Q_DECLARE_METATYPE(noxshell::TransferState)
Q_DECLARE_METATYPE(noxshell::FileTransferTask)
