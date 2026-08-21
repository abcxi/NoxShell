#pragma once

#include "../core/FileTransferTask.h"

#include <QFrame>
#include <QHash>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QComboBox;

namespace noxshell {
class SshSession;
}

namespace noxshell::ui {

class TransferQueuePanel final : public QFrame {
    Q_OBJECT

public:
    explicit TransferQueuePanel(SshSession *session, QWidget *parent = nullptr);

    [[nodiscard]] int taskCount() const;

signals:
    void summaryChanged(int activeCount, int totalCount, const QString &summary);
    void taskAdded();

private:
    void updateTask(const FileTransferTask &task);
    void updateSummary();

    SshSession *m_session{};
    QLabel *m_summary{};
    QLabel *m_emptyLabel{};
    QListWidget *m_list{};
    QComboBox *m_rateLimit{};
    QHash<quint64, QListWidgetItem *> m_items;
    QHash<quint64, FileTransferTask> m_tasks;
};

} // namespace noxshell::ui
