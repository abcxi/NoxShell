#include "TransferQueuePanel.h"

#include "../core/SshSession.h"

#include <QFileInfo>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

namespace noxshell::ui {

namespace {
QString stateText(TransferState state)
{
    switch (state) {
    case TransferState::Queued: return QStringLiteral("排队中");
    case TransferState::Running: return QStringLiteral("传输中");
    case TransferState::Completed: return QStringLiteral("已完成");
    case TransferState::Failed: return QStringLiteral("失败");
    case TransferState::Canceled: return QStringLiteral("已取消");
    }
    return {};
}
} // namespace

TransferQueuePanel::TransferQueuePanel(SshSession *session, QWidget *parent)
    : QFrame(parent)
    , m_session(session)
{
    setObjectName(QStringLiteral("transferQueuePanel"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *header = new QWidget;
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(10, 3, 8, 3);
    auto *title = new QLabel(QStringLiteral("传输队列"));
    title->setStyleSheet(QStringLiteral("font-weight:650;font-size:11px;"));
    m_summary = new QLabel(QStringLiteral("空闲"));
    m_summary->setObjectName(QStringLiteral("mutedLabel"));
    m_toggle = new QPushButton(QStringLiteral("展开"));
    m_toggle->setFlat(true);
    m_toggle->setFixedHeight(24);
    m_toggle->setObjectName(QStringLiteral("transferQueueToggle"));
    m_rateLimit = new QComboBox;
    m_rateLimit->setObjectName(QStringLiteral("transferRateLimit"));
    m_rateLimit->addItem(QStringLiteral("不限速"), qulonglong{0});
    m_rateLimit->addItem(QStringLiteral("1 MiB/s"), qulonglong{1 * 1024 * 1024});
    m_rateLimit->addItem(QStringLiteral("5 MiB/s"), qulonglong{5 * 1024 * 1024});
    m_rateLimit->addItem(QStringLiteral("10 MiB/s"), qulonglong{10 * 1024 * 1024});
    m_rateLimit->setFixedWidth(94);
    headerLayout->addWidget(title);
    headerLayout->addWidget(m_summary);
    headerLayout->addStretch();
    headerLayout->addWidget(m_rateLimit);
    headerLayout->addWidget(m_toggle);
    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("transferQueueList"));
    m_list->setFixedHeight(122);
    m_list->hide();
    layout->addWidget(header);
    layout->addWidget(m_list);

    connect(m_toggle, &QPushButton::clicked, this, [this] {
        m_list->setVisible(!m_list->isVisible());
        m_toggle->setText(m_list->isVisible() ? QStringLiteral("收起") : QStringLiteral("展开"));
        if (window()) window()->adjustSize();
    });
    connect(m_session, &SshSession::transferTaskChanged, this, &TransferQueuePanel::updateTask);
    connect(m_session, &SshSession::transferQueueReset, this, [this] {
        m_tasks.clear();
        m_items.clear();
        m_list->clear();
        updateSummary();
    });
    connect(m_rateLimit, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_session->setTransferRateLimit(m_rateLimit->itemData(index).toULongLong());
    });
    updateSummary();
}

int TransferQueuePanel::taskCount() const
{
    return m_tasks.size();
}

void TransferQueuePanel::updateTask(const FileTransferTask &task)
{
    m_tasks.insert(task.id, task);
    auto *item = m_items.value(task.id, nullptr);
    if (!item) {
        item = new QListWidgetItem(m_list);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(task.id));
        item->setSizeHint(QSize(0, 42));
        m_items.insert(task.id, item);
    }
    auto *row = m_list->itemWidget(item);
    if (!row) {
        row = new QWidget;
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 3, 6, 3);
        rowLayout->setSpacing(7);
        auto *name = new QLabel;
        name->setObjectName(QStringLiteral("transferName"));
        auto *progress = new QProgressBar;
        progress->setObjectName(QStringLiteral("transferProgress"));
        progress->setFixedHeight(5);
        progress->setTextVisible(false);
        auto *state = new QLabel;
        state->setObjectName(QStringLiteral("transferState"));
        state->setFixedWidth(52);
        auto *cancel = new QPushButton(QStringLiteral("取消"));
        cancel->setObjectName(QStringLiteral("transferCancel"));
        cancel->setFixedSize(44, 25);
        rowLayout->addWidget(name, 1);
        rowLayout->addWidget(progress, 1);
        rowLayout->addWidget(state);
        rowLayout->addWidget(cancel);
        connect(cancel, &QPushButton::clicked, this, [this, item] {
            const auto id = item->data(Qt::UserRole).toULongLong();
            const auto task = m_tasks.value(id);
            if (task.state == TransferState::Failed || task.state == TransferState::Canceled) m_session->retryTransfer(id);
            else m_session->cancelTransfer(id);
        });
        m_list->setItemWidget(item, row);
    }
    row->findChild<QLabel *>(QStringLiteral("transferName"))->setText(
        (task.operation == RemoteFileOperation::Upload ? QStringLiteral("↑ ") : QStringLiteral("↓ ")) + QFileInfo(task.remotePath).fileName());
    auto *progress = row->findChild<QProgressBar *>(QStringLiteral("transferProgress"));
    progress->setRange(task.total > 0 ? 0 : 0, task.total > 0 ? 100 : 0);
    if (task.total > 0) progress->setValue(qRound(task.completed * 100.0 / task.total));
    row->findChild<QLabel *>(QStringLiteral("transferState"))->setText(stateText(task.state));
    auto *action = row->findChild<QPushButton *>(QStringLiteral("transferCancel"));
    const bool retryable = task.state == TransferState::Failed || task.state == TransferState::Canceled;
    action->setText(retryable ? QStringLiteral("重试") : QStringLiteral("取消"));
    action->setEnabled(retryable || task.state == TransferState::Queued || task.state == TransferState::Running);
    item->setToolTip(task.message);
    updateSummary();
}

void TransferQueuePanel::updateSummary()
{
    int running = 0;
    int queued = 0;
    for (const auto &task : std::as_const(m_tasks)) {
        running += task.state == TransferState::Running;
        queued += task.state == TransferState::Queued;
    }
    const auto summary = running || queued
            ? QStringLiteral("活动 %1 · 排队 %2").arg(running).arg(queued)
            : m_tasks.isEmpty() ? QStringLiteral("空闲") : QStringLiteral("全部完成");
    m_summary->setText(summary);
    emit summaryChanged(running + queued, m_tasks.size(), summary);
}

} // namespace noxshell::ui
