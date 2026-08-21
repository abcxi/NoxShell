#include "TransferQueuePanel.h"

#include "../core/SshSession.h"

#include <QFileInfo>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSizePolicy>
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

QString formattedBytes(quint64 bytes)
{
    constexpr double kib = 1024.0;
    constexpr double mib = kib * 1024.0;
    constexpr double gib = mib * 1024.0;
    if (bytes >= gib) return QStringLiteral("%1 GiB").arg(bytes / gib, 0, 'f', 1);
    if (bytes >= mib) return QStringLiteral("%1 MiB").arg(bytes / mib, 0, 'f', 1);
    if (bytes >= kib) return QStringLiteral("%1 KiB").arg(bytes / kib, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(bytes);
}

QString stateStyle(TransferState state)
{
    switch (state) {
    case TransferState::Running: return QStringLiteral("color:#006EFF;background:#E8F3FF;");
    case TransferState::Completed: return QStringLiteral("color:#008858;background:#E8F8F2;");
    case TransferState::Failed: return QStringLiteral("color:#C62828;background:#FFF0F0;");
    case TransferState::Canceled: return QStringLiteral("color:#7A5A43;background:#F7F1EC;");
    case TransferState::Queued: return QStringLiteral("color:#7A5A00;background:#FFF8DF;");
    }
    return {};
}
} // namespace

TransferQueuePanel::TransferQueuePanel(SshSession *session, QWidget *parent)
    : QFrame(parent)
    , m_session(session)
{
    setObjectName(QStringLiteral("transferQueuePanel"));
    setMinimumSize(520, 220);
    setMaximumHeight(360);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 12);
    layout->setSpacing(8);
    auto *header = new QWidget;
    header->setObjectName(QStringLiteral("transferQueueHeader"));
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(8);
    auto *title = new QLabel(QStringLiteral("传输队列"));
    title->setObjectName(QStringLiteral("transferQueueTitle"));
    m_summary = new QLabel(QStringLiteral("空闲"));
    m_summary->setObjectName(QStringLiteral("transferQueueSummary"));
    m_rateLimit = new QComboBox;
    m_rateLimit->setObjectName(QStringLiteral("transferRateLimit"));
    m_rateLimit->addItem(QStringLiteral("不限速"), qulonglong{0});
    m_rateLimit->addItem(QStringLiteral("1 MiB/s"), qulonglong{1 * 1024 * 1024});
    m_rateLimit->addItem(QStringLiteral("5 MiB/s"), qulonglong{5 * 1024 * 1024});
    m_rateLimit->addItem(QStringLiteral("10 MiB/s"), qulonglong{10 * 1024 * 1024});
    m_rateLimit->setFixedSize(104, 28);
    headerLayout->addWidget(title);
    headerLayout->addWidget(m_summary);
    headerLayout->addStretch();
    auto *rateTitle = new QLabel(QStringLiteral("限速"));
    rateTitle->setObjectName(QStringLiteral("transferRateTitle"));
    headerLayout->addWidget(rateTitle);
    headerLayout->addWidget(m_rateLimit);
    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("transferQueueList"));
    m_list->setMinimumHeight(170);
    m_list->setMaximumHeight(300);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_emptyLabel = new QLabel(QStringLiteral("暂无传输任务\n将本地文件拖入右侧目录即可上传"));
    m_emptyLabel->setObjectName(QStringLiteral("transferQueueEmpty"));
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(header);
    layout->addWidget(m_list);
    layout->addWidget(m_emptyLabel, 1);
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
    const bool isNewTask = !m_tasks.contains(task.id);
    m_tasks.insert(task.id, task);
    auto *item = m_items.value(task.id, nullptr);
    if (!item) {
        item = new QListWidgetItem(m_list);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(task.id));
        item->setSizeHint(QSize(0, 78));
        m_items.insert(task.id, item);
    }
    auto *row = m_list->itemWidget(item);
    if (!row) {
        row = new QWidget;
        row->setObjectName(QStringLiteral("transferTaskRow"));
        auto *rowLayout = new QVBoxLayout(row);
        rowLayout->setContentsMargins(10, 7, 8, 7);
        rowLayout->setSpacing(4);
        auto *top = new QHBoxLayout;
        top->setSpacing(7);
        auto *direction = new QLabel;
        direction->setObjectName(QStringLiteral("transferDirection"));
        direction->setFixedWidth(18);
        direction->setAlignment(Qt::AlignCenter);
        auto *name = new QLabel;
        name->setObjectName(QStringLiteral("transferName"));
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto *progress = new QProgressBar;
        progress->setObjectName(QStringLiteral("transferProgress"));
        progress->setFixedHeight(6);
        progress->setTextVisible(false);
        auto *state = new QLabel;
        state->setObjectName(QStringLiteral("transferState"));
        state->setAlignment(Qt::AlignCenter);
        state->setFixedSize(58, 22);
        auto *cancel = new QPushButton(QStringLiteral("取消"));
        cancel->setObjectName(QStringLiteral("transferCancel"));
        cancel->setFixedSize(48, 24);
        top->addWidget(direction);
        top->addWidget(name, 1);
        top->addWidget(state);
        top->addWidget(cancel);
        auto *details = new QHBoxLayout;
        details->setSpacing(8);
        auto *amount = new QLabel;
        amount->setObjectName(QStringLiteral("transferAmount"));
        amount->setFixedWidth(150);
        auto *path = new QLabel;
        path->setObjectName(QStringLiteral("transferPath"));
        path->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        details->addWidget(amount);
        details->addWidget(path, 1);
        rowLayout->addLayout(top);
        rowLayout->addWidget(progress);
        rowLayout->addLayout(details);
        connect(cancel, &QPushButton::clicked, this, [this, item] {
            const auto id = item->data(Qt::UserRole).toULongLong();
            const auto task = m_tasks.value(id);
            if (task.state == TransferState::Failed || task.state == TransferState::Canceled) m_session->retryTransfer(id);
            else m_session->cancelTransfer(id);
        });
        m_list->setItemWidget(item, row);
    }
    const bool upload = task.operation == RemoteFileOperation::Upload;
    row->findChild<QLabel *>(QStringLiteral("transferDirection"))->setText(upload ? QStringLiteral("↑") : QStringLiteral("↓"));
    auto *name = row->findChild<QLabel *>(QStringLiteral("transferName"));
    name->setText(QFileInfo(task.remotePath).fileName());
    name->setToolTip(upload ? task.localPath : task.remotePath);
    auto *progress = row->findChild<QProgressBar *>(QStringLiteral("transferProgress"));
    const int percent = task.total > 0 ? qBound(0, qRound(task.completed * 100.0 / task.total), 100) : 0;
    if (task.total > 0 || task.state != TransferState::Running) {
        progress->setRange(0, 100);
        progress->setValue(task.state == TransferState::Completed ? 100 : percent);
    } else {
        progress->setRange(0, 0);
    }
    auto *state = row->findChild<QLabel *>(QStringLiteral("transferState"));
    state->setText(stateText(task.state));
    state->setStyleSheet(stateStyle(task.state) + QStringLiteral("border-radius:4px;font-size:10px;font-weight:650;"));
    auto *amount = row->findChild<QLabel *>(QStringLiteral("transferAmount"));
    amount->setText(task.total > 0
        ? QStringLiteral("%1 / %2 · %3%").arg(formattedBytes(task.completed), formattedBytes(task.total)).arg(percent)
        : task.message);
    auto *path = row->findChild<QLabel *>(QStringLiteral("transferPath"));
    path->setText(upload ? QStringLiteral("目标  %1").arg(task.remotePath)
                         : QStringLiteral("保存至  %1").arg(task.localPath));
    path->setToolTip(path->text());
    auto *action = row->findChild<QPushButton *>(QStringLiteral("transferCancel"));
    const bool retryable = task.state == TransferState::Failed || task.state == TransferState::Canceled;
    action->setText(retryable ? QStringLiteral("重试") : QStringLiteral("取消"));
    action->setEnabled(retryable || task.state == TransferState::Queued || task.state == TransferState::Running);
    action->setVisible(task.state != TransferState::Completed);
    item->setToolTip(task.message.isEmpty() ? path->text() : task.message);
    updateSummary();
    if (isNewTask) emit taskAdded();
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
    const bool empty = m_tasks.isEmpty();
    m_list->setVisible(!empty);
    m_emptyLabel->setVisible(empty);
    emit summaryChanged(running + queued, m_tasks.size(), summary);
}

} // namespace noxshell::ui
