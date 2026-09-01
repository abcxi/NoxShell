#include "CommandHistoryPanel.h"

#include "../core/ServerRepository.h"

#include <QAction>
#include <QDateTime>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QTabBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace noxshell::ui {

CommandHistoryPanel::CommandHistoryPanel(ServerRepository *repository, QWidget *parent)
    : QFrame(parent)
    , m_repository(repository)
{
    setObjectName(QStringLiteral("commandHistoryPanel"));
    setMinimumHeight(190);
    setMaximumHeight(260);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 9);
    layout->setSpacing(6);

    auto *header = new QHBoxLayout;
    header->setSpacing(6);
    auto *title = new QLabel(QStringLiteral("命令记录"));
    title->setObjectName(QStringLiteral("commandHistoryTitle"));
    auto *scope = new QLabel(QStringLiteral("本机共享"));
    scope->setObjectName(QStringLiteral("commandHistoryScope"));
    scope->setToolTip(QStringLiteral("历史和收藏保存在当前设备，可在所有 SSH 主机中使用"));
    m_tabs = new QTabBar;
    m_tabs->setObjectName(QStringLiteral("commandHistoryTabs"));
    m_tabs->setExpanding(false);
    m_tabs->addTab(QStringLiteral("历史"));
    m_tabs->addTab(QStringLiteral("收藏"));
    m_clearButton = new QToolButton;
    m_clearButton->setObjectName(QStringLiteral("commandHistoryClearButton"));
    m_clearButton->setText(QStringLiteral("清空"));
    m_clearButton->setToolTip(QStringLiteral("清空当前列表"));
    auto *closeButton = new QToolButton;
    closeButton->setObjectName(QStringLiteral("commandHistoryCloseButton"));
    closeButton->setText(QString::fromUtf8("×"));
    closeButton->setToolTip(QStringLiteral("收起命令记录"));
    header->addWidget(title);
    header->addWidget(scope);
    header->addWidget(m_tabs);
    header->addStretch();
    header->addWidget(m_clearButton);
    header->addWidget(closeButton);
    layout->addLayout(header);

    m_tree = new QTreeWidget;
    m_tree->setObjectName(QStringLiteral("commandHistoryList"));
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({QStringLiteral(""), QStringLiteral("命令"),
        QStringLiteral("备注"), QStringLiteral("来源主机"), QStringLiteral("最近执行")});
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_tree->header()->resizeSection(0, 34);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    layout->addWidget(m_tree, 1);

    m_menu = new QMenu(this);
    m_executeAction = m_menu->addAction(QStringLiteral("执行命令"));
    m_favoriteAction = m_menu->addAction(QStringLiteral("收藏"));
    m_noteAction = m_menu->addAction(QStringLiteral("修改备注"));
    m_menu->addSeparator();
    m_deleteAction = m_menu->addAction(QStringLiteral("删除记录"));
    m_executeAction->setObjectName(QStringLiteral("commandHistoryExecuteAction"));
    m_favoriteAction->setObjectName(QStringLiteral("commandHistoryFavoriteAction"));
    m_noteAction->setObjectName(QStringLiteral("commandHistoryNoteAction"));
    m_deleteAction->setObjectName(QStringLiteral("commandHistoryDeleteAction"));

    connect(m_tabs, &QTabBar::currentChanged, this, [this] { refresh(); });
    connect(m_clearButton, &QToolButton::clicked, this, &CommandHistoryPanel::clearCurrentView);
    connect(closeButton, &QToolButton::clicked, this, &CommandHistoryPanel::closeRequested);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int column) {
        if (!item) return;
        if (column == 2) {
            editCurrentNote();
            return;
        }
        emit commandSelected(item->text(1));
    });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        auto *item = m_tree->itemAt(position);
        const int row = item ? m_tree->indexOfTopLevelItem(item) : -1;
        if (!prepareItemActions(row)) return;
        const auto platform = QGuiApplication::platformName();
        if (platform != QStringLiteral("offscreen") && platform != QStringLiteral("minimal")) {
            m_menu->popup(m_tree->viewport()->mapToGlobal(position));
        }
    });
    connect(m_executeAction, &QAction::triggered, this, [this] {
        const auto command = currentCommand();
        if (!command.isEmpty()) emit commandExecutionRequested(command);
    });
    connect(m_favoriteAction, &QAction::triggered, this, [this] {
        const auto id = currentEntryId();
        if (id > 0 && m_repository && m_repository->setCommandFavorite(id, !currentFavorite())) refresh();
    });
    connect(m_noteAction, &QAction::triggered, this, &CommandHistoryPanel::editCurrentNote);
    connect(m_deleteAction, &QAction::triggered, this, [this] {
        const auto id = currentEntryId();
        if (id > 0 && m_repository && m_repository->deleteCommandHistory(id)) refresh();
    });
}

void CommandHistoryPanel::setServerId(const QString &serverId)
{
    if (m_serverId == serverId) return;
    m_serverId = serverId;
    refresh();
}

void CommandHistoryPanel::recordCommand(const QString &command)
{
    if (!m_repository || m_serverId.isEmpty() || command.trimmed().isEmpty()) return;
    if (m_repository->recordCommand(m_serverId, command) && isVisible()) refresh();
}

void CommandHistoryPanel::refresh()
{
    m_tree->clear();
    if (!m_repository) return;
    const bool favoritesOnly = m_tabs->currentIndex() == 1;
    const auto entries = m_repository->loadCommandHistory(favoritesOnly, 300);
    for (const auto &entry : entries) {
        auto *item = new QTreeWidgetItem(m_tree, {
            entry.favorite ? QStringLiteral("★") : QStringLiteral("☆"),
            entry.command,
            entry.note,
            entry.serverName.isEmpty() ? QStringLiteral("已删除主机") : entry.serverName,
            entry.executedAt.toString(QStringLiteral("MM-dd HH:mm:ss")),
        });
        item->setData(0, Qt::UserRole, entry.id);
        item->setData(0, Qt::UserRole + 1, entry.favorite);
        item->setToolTip(1, QStringLiteral("双击填入命令；右键可执行或管理"));
        item->setToolTip(2, QStringLiteral("双击修改备注"));
        item->setToolTip(3, QStringLiteral("最近一次执行该命令的主机"));
    }
    m_clearButton->setText(favoritesOnly ? QStringLiteral("清空收藏") : QStringLiteral("清空历史"));
    m_clearButton->setToolTip(favoritesOnly
        ? QStringLiteral("取消全部收藏标记，命令仍保留在历史中")
        : QStringLiteral("清空未收藏的历史记录，已收藏命令保持不变"));
}

bool CommandHistoryPanel::prepareItemActions(int row)
{
    if (row < 0 || row >= m_tree->topLevelItemCount()) return false;
    m_tree->setCurrentItem(m_tree->topLevelItem(row));
    m_favoriteAction->setText(currentFavorite() ? QStringLiteral("取消收藏") : QStringLiteral("收藏"));
    return true;
}

void CommandHistoryPanel::editCurrentNote()
{
    auto *item = m_tree->currentItem();
    const auto id = currentEntryId();
    if (!item || id <= 0 || !m_repository) return;
    bool accepted = false;
    const auto note = QInputDialog::getText(this, QStringLiteral("命令备注"),
        QStringLiteral("备注"), QLineEdit::Normal, item->text(2), &accepted);
    if (accepted && m_repository->setCommandNote(id, note)) refresh();
}

void CommandHistoryPanel::clearCurrentView()
{
    if (!m_repository || m_tree->topLevelItemCount() == 0) return;
    const bool favoritesOnly = m_tabs->currentIndex() == 1;
    const auto question = favoritesOnly
        ? QStringLiteral("取消当前设备的全部命令收藏？命令仍会保留在历史中。")
        : QStringLiteral("清空当前设备的全部未收藏历史？已收藏命令及其备注会保留。");
    if (QMessageBox::question(this, QStringLiteral("确认清空"), question,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    const bool saved = favoritesOnly ? m_repository->clearCommandFavorites()
                                     : m_repository->clearCommandHistory();
    if (saved) refresh();
}

qint64 CommandHistoryPanel::currentEntryId() const
{
    return m_tree->currentItem() ? m_tree->currentItem()->data(0, Qt::UserRole).toLongLong() : 0;
}

QString CommandHistoryPanel::currentCommand() const
{
    return m_tree->currentItem() ? m_tree->currentItem()->text(1) : QString{};
}

bool CommandHistoryPanel::currentFavorite() const
{
    return m_tree->currentItem() && m_tree->currentItem()->data(0, Qt::UserRole + 1).toBool();
}

} // namespace noxshell::ui
