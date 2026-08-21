#include "HostSidebar.h"

#include <QApplication>
#include <QClipboard>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace noxshell::ui {

namespace {
constexpr int kItemKindRole = Qt::UserRole;
constexpr int kServerIndexRole = Qt::UserRole + 1;
constexpr int kGroupNameRole = Qt::UserRole + 2;
constexpr int kServerIdRole = Qt::UserRole + 3;
constexpr int kGroupItem = 1;
constexpr int kServerItem = 2;
const auto kUngroupedLabel = QStringLiteral("未分组");

QString displayGroupName(const QString &group)
{
    return group.isEmpty() ? kUngroupedLabel : group;
}

class HostTreeWidget final : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    std::function<void(const QString &, const QString &)> hostDropRequested;

protected:
    void dropEvent(QDropEvent *event) override
    {
        auto *source = currentItem();
        auto *target = itemAt(event->position().toPoint());
        if (!source || source->data(0, kItemKindRole).toInt() != kServerItem || !target) {
            event->ignore();
            return;
        }
        auto *group = target->data(0, kItemKindRole).toInt() == kGroupItem ? target : target->parent();
        if (!group || group->data(0, kItemKindRole).toInt() != kGroupItem) {
            event->ignore();
            return;
        }
        const auto serverId = source->data(0, kServerIdRole).toString();
        const auto groupName = group->data(0, kGroupNameRole).toString();
        if (!serverId.isEmpty() && hostDropRequested) hostDropRequested(serverId, groupName);
        event->acceptProposedAction();
    }
};

void updateHostItem(QWidget *widget, const ServerProfile &server)
{
    if (!widget) return;
    auto *name = widget->findChild<QLabel *>(QStringLiteral("hostItemName"));
    auto *address = widget->findChild<QLabel *>(QStringLiteral("hostItemAddress"));
    if (name) name->setText(server.name);
    if (address) address->setText(server.host);
}

QWidget *createHostItem(const ServerProfile &server)
{
    auto *widget = new QWidget;
    widget->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(4, 2, 7, 2);
    layout->setSpacing(7);
    auto *name = new QLabel;
    name->setObjectName(QStringLiteral("hostItemName"));
    name->setStyleSheet(QStringLiteral("color:#20344B;font-weight:650;"));
    auto *address = new QLabel;
    address->setObjectName(QStringLiteral("hostItemAddress"));
    address->setStyleSheet(QStringLiteral("color:#60748A;font-size:12px;"));
    layout->addWidget(name);
    layout->addWidget(address);
    layout->addStretch();
    updateHostItem(widget, server);
    return widget;
}
} // namespace

HostSidebar::HostSidebar(QVector<ServerProfile> servers, QStringList groups, QWidget *parent)
    : QFrame(parent)
    , m_servers(std::move(servers))
    , m_groups(std::move(groups))
{
    setObjectName(QStringLiteral("hostSidebar"));
    setFixedWidth(244);
    for (const auto &server : std::as_const(m_servers)) {
        const auto group = server.group.trimmed();
        if (!group.isEmpty() && !m_groups.contains(group)) m_groups.append(group);
    }
    m_groups.removeDuplicates();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 12, 9, 8);
    layout->setSpacing(8);
    auto *filterRow = new QHBoxLayout;
    filterRow->setContentsMargins(0, 0, 0, 0);
    filterRow->setSpacing(7);
    m_search = new QLineEdit;
    m_search->setObjectName(QStringLiteral("hostSearch"));
    m_search->setPlaceholderText(QStringLiteral("搜索名称、IP 或分组"));
    m_search->setClearButtonEnabled(true);
    auto *addButton = new QPushButton(QStringLiteral("+"));
    addButton->setObjectName(QStringLiteral("hostAddButton"));
    addButton->setToolTip(QStringLiteral("新增 SSH 主机"));
    filterRow->addWidget(m_search, 1);
    filterRow->addWidget(addButton);

    auto *tree = new HostTreeWidget;
    m_list = tree;
    m_list->setObjectName(QStringLiteral("hostList"));
    m_list->setHeaderHidden(true);
    m_list->setRootIsDecorated(true);
    m_list->setIndentation(14);
    m_list->setAnimated(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setDragEnabled(true);
    m_list->setAcceptDrops(true);
    m_list->setDropIndicatorShown(true);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setStyleSheet(QStringLiteral(
        "QTreeWidget{border:0;background:white;outline:0;}"
        "QTreeWidget::item{border-radius:3px;}"
        "QTreeWidget::item:selected{background:#E8F3FF;color:#0052D9;}"
        "QTreeWidget::branch{background:transparent;}"));
    tree->hostDropRequested = [this](const QString &serverId, const QString &group) {
        moveServerToGroup(serverId, group);
    };

    m_contextMenu = new QMenu(this);
    m_connectAction = m_contextMenu->addAction(QStringLiteral("连接"));
    m_editAction = m_contextMenu->addAction(QStringLiteral("编辑"));
    m_duplicateAction = m_contextMenu->addAction(QStringLiteral("复制主机配置"));
    m_copyAddressAction = m_contextMenu->addAction(QStringLiteral("复制连接地址"));
    m_moveMenu = m_contextMenu->addMenu(QStringLiteral("移动到分组"));
    m_contextMenu->addSeparator();
    m_newConnectionAction = m_contextMenu->addAction(QStringLiteral("新建连接"));
    m_newGroupAction = m_contextMenu->addAction(QStringLiteral("新建分组"));
    m_renameGroupAction = m_contextMenu->addAction(QStringLiteral("重命名分组"));
    m_deleteGroupAction = m_contextMenu->addAction(QStringLiteral("删除分组"));
    m_contextMenu->addSeparator();
    m_deleteAction = m_contextMenu->addAction(QStringLiteral("删除主机"));
    m_connectAction->setObjectName(QStringLiteral("hostConnectAction"));
    m_editAction->setObjectName(QStringLiteral("hostEditAction"));
    m_duplicateAction->setObjectName(QStringLiteral("hostDuplicateAction"));
    m_copyAddressAction->setObjectName(QStringLiteral("hostCopyAddressAction"));
    m_moveMenu->setObjectName(QStringLiteral("hostMoveGroupMenu"));
    m_newConnectionAction->setObjectName(QStringLiteral("hostNewConnectionAction"));
    m_newGroupAction->setObjectName(QStringLiteral("hostNewGroupAction"));
    m_renameGroupAction->setObjectName(QStringLiteral("hostRenameGroupAction"));
    m_deleteGroupAction->setObjectName(QStringLiteral("hostDeleteGroupAction"));
    m_deleteAction->setObjectName(QStringLiteral("hostDeleteAction"));

    auto *credentialButton = new QPushButton(QStringLiteral("⚙  连接与凭据"));
    credentialButton->setFlat(true);
    credentialButton->setStyleSheet(QStringLiteral("text-align:left;color:#66768A;border-top:1px solid #DFE6EF;"));
    layout->addLayout(filterRow);
    layout->addWidget(m_list, 1);
    layout->addWidget(credentialButton);
    populate();

    connect(addButton, &QPushButton::clicked, this, &HostSidebar::addServerRequested);
    connect(m_search, &QLineEdit::textChanged, this, &HostSidebar::applyFilter);
    connect(m_list, &QTreeWidget::currentItemChanged, this,
        [this](QTreeWidgetItem *current, QTreeWidgetItem *) {
            const auto profile = profileForItem(current);
            if (!profile.id.isEmpty()) emit serverSelected(profile);
        });
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        const auto profile = profileForItem(item);
        if (profile.id.isEmpty()) return;
        emit serverConnectRequested(profile);
        emit collapseRequested();
    });
    connect(m_list, &QTreeWidget::customContextMenuRequested, this, &HostSidebar::showContextMenu);

    const auto contextProfile = [this] {
        return m_contextMenu->property("serverProfile").value<ServerProfile>();
    };
    connect(m_connectAction, &QAction::triggered, this,
        [this, contextProfile] { emit serverConnectRequested(contextProfile()); });
    connect(m_editAction, &QAction::triggered, this,
        [this, contextProfile] { emit serverEditRequested(contextProfile()); });
    connect(m_duplicateAction, &QAction::triggered, this,
        [this, contextProfile] { emit serverDuplicateRequested(contextProfile()); });
    connect(m_copyAddressAction, &QAction::triggered, this, [contextProfile] {
        const auto profile = contextProfile();
        QApplication::clipboard()->setText(QStringLiteral("%1@%2:%3").arg(profile.user, profile.host).arg(profile.port));
    });
    connect(m_deleteAction, &QAction::triggered, this,
        [this, contextProfile] { emit serverDeleteRequested(contextProfile()); });
    connect(m_newConnectionAction, &QAction::triggered, this, [this] {
        emit addServerInGroupRequested(m_contextMenu->property("groupName").toString());
    });
    connect(m_newGroupAction, &QAction::triggered, this, [this] {
        bool accepted = false;
        const auto name = QInputDialog::getText(this, QStringLiteral("新建分组"), QStringLiteral("分组名称"),
            QLineEdit::Normal, {}, &accepted).trimmed();
        if (!accepted || name.isEmpty() || m_groups.contains(name)) return;
        emit groupCreateRequested(name);
    });
    connect(m_renameGroupAction, &QAction::triggered, this, [this] {
        const auto oldName = m_contextMenu->property("groupName").toString();
        if (oldName.isEmpty()) return;
        bool accepted = false;
        const auto name = QInputDialog::getText(this, QStringLiteral("重命名分组"), QStringLiteral("新名称"),
            QLineEdit::Normal, oldName, &accepted).trimmed();
        if (!accepted || name.isEmpty() || name == oldName || m_groups.contains(name)) return;
        emit groupRenameRequested(oldName, name);
    });
    connect(m_deleteGroupAction, &QAction::triggered, this, [this] {
        const auto group = m_contextMenu->property("groupName").toString();
        if (group.isEmpty()) return;
        const auto answer = QMessageBox::question(this, QStringLiteral("删除分组"),
            QStringLiteral("删除分组“%1”吗？组内主机将移动到“未分组”，主机配置不会被删除。").arg(group),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes) emit groupDeleteRequested(group);
    });
}

ServerProfile HostSidebar::profileForItem(const QTreeWidgetItem *item) const
{
    if (!item || item->data(0, kItemKindRole).toInt() != kServerItem) return {};
    const auto index = item->data(0, kServerIndexRole).toInt();
    return index >= 0 && index < m_servers.size() ? m_servers.at(index) : ServerProfile{};
}

QTreeWidgetItem *HostSidebar::groupItem(const QString &group) const
{
    for (int row = 0; row < m_list->topLevelItemCount(); ++row) {
        auto *item = m_list->topLevelItem(row);
        if (item->data(0, kItemKindRole).toInt() == kGroupItem
            && item->data(0, kGroupNameRole).toString() == group) return item;
    }
    return nullptr;
}

QString HostSidebar::groupForItem(const QTreeWidgetItem *item) const
{
    if (!item) return {};
    const auto *group = item->data(0, kItemKindRole).toInt() == kGroupItem ? item : item->parent();
    return group ? group->data(0, kGroupNameRole).toString() : QString{};
}

void HostSidebar::selectFirstServer()
{
    for (int groupIndex = 0; groupIndex < m_list->topLevelItemCount(); ++groupIndex) {
        auto *group = m_list->topLevelItem(groupIndex);
        if (group->childCount() > 0) {
            m_list->setCurrentItem(group->child(0));
            return;
        }
    }
}

bool HostSidebar::selectServerById(const QString &serverId)
{
    for (QTreeWidgetItemIterator iterator(m_list); *iterator; ++iterator) {
        auto *item = *iterator;
        if (profileForItem(item).id != serverId) continue;
        const QSignalBlocker blocker(m_list);
        m_list->setCurrentItem(item);
        return true;
    }
    return false;
}

void HostSidebar::addServer(const ServerProfile &profile)
{
    m_servers.append(profile);
    if (!profile.group.trimmed().isEmpty() && !m_groups.contains(profile.group.trimmed()))
        m_groups.append(profile.group.trimmed());
    populate();
    selectServerById(profile.id);
}

bool HostSidebar::updateServer(const ServerProfile &profile)
{
    for (qsizetype index = 0; index < m_servers.size(); ++index) {
        if (m_servers.at(index).id != profile.id) continue;
        m_servers[index] = profile;
        if (!profile.group.trimmed().isEmpty() && !m_groups.contains(profile.group.trimmed()))
            m_groups.append(profile.group.trimmed());
        populate();
        selectServerById(profile.id);
        return true;
    }
    return false;
}

bool HostSidebar::setServerState(const QString &serverId, ServerState state)
{
    for (auto &server : m_servers) {
        if (server.id != serverId) continue;
        server.state = state;
        return true;
    }
    return false;
}

bool HostSidebar::removeServer(const QString &id)
{
    for (qsizetype index = 0; index < m_servers.size(); ++index) {
        if (m_servers.at(index).id != id) continue;
        m_servers.removeAt(index);
        populate();
        selectFirstServer();
        return true;
    }
    return false;
}

void HostSidebar::addGroup(const QString &name)
{
    const auto normalized = name.trimmed();
    if (normalized.isEmpty() || m_groups.contains(normalized)) return;
    m_groups.append(normalized);
    populate();
}

void HostSidebar::renameGroup(const QString &oldName, const QString &newName)
{
    const auto oldNormalized = oldName.trimmed();
    const auto newNormalized = newName.trimmed();
    if (oldNormalized.isEmpty() || newNormalized.isEmpty()) return;
    const auto index = m_groups.indexOf(oldNormalized);
    if (index >= 0) m_groups[index] = newNormalized;
    for (auto &server : m_servers) if (server.group == oldNormalized) server.group = newNormalized;
    m_groups.removeDuplicates();
    populate();
}

void HostSidebar::removeGroup(const QString &name)
{
    const auto normalized = name.trimmed();
    m_groups.removeAll(normalized);
    for (auto &server : m_servers) if (server.group == normalized) server.group.clear();
    populate();
}

bool HostSidebar::moveServerToGroup(const QString &serverId, const QString &group)
{
    for (auto &server : m_servers) {
        if (server.id != serverId) continue;
        const auto normalized = group.trimmed();
        if (server.group == normalized) return true;
        server.group = normalized;
        if (!normalized.isEmpty() && !m_groups.contains(normalized)) m_groups.append(normalized);
        const auto updated = server;
        populate();
        selectServerById(serverId);
        emit serverGroupChanged(updated);
        return true;
    }
    return false;
}

void HostSidebar::populate()
{
    const auto selected = profileForItem(m_list->currentItem()).id;
    m_list->clear();
    if (m_servers.isEmpty() && m_groups.isEmpty()) {
        auto *item = new QTreeWidgetItem(m_list, {QStringLiteral("暂无 SSH 主机 · 右键可新建分组")});
        item->setFlags(Qt::NoItemFlags);
        item->setForeground(0, QColor(QStringLiteral("#8794A5")));
        item->setTextAlignment(0, Qt::AlignCenter);
        item->setSizeHint(0, QSize(218, 72));
        return;
    }
    QStringList groups = m_groups;
    const bool hasUngrouped = std::any_of(m_servers.cbegin(), m_servers.cend(),
        [](const ServerProfile &server) { return server.group.trimmed().isEmpty(); });
    if (hasUngrouped) groups.prepend(QString{});
    groups.removeDuplicates();
    for (const auto &groupName : groups) {
        auto *group = new QTreeWidgetItem(m_list);
        group->setData(0, kItemKindRole, kGroupItem);
        group->setData(0, kGroupNameRole, groupName);
        group->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        group->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled);
        group->setExpanded(true);
        group->setForeground(0, QColor(QStringLiteral("#42566D")));
        auto font = group->font(0);
        font.setBold(true);
        group->setFont(0, font);
        for (qsizetype index = 0; index < m_servers.size(); ++index) {
            const auto &server = m_servers.at(index);
            if (server.group.trimmed() != groupName) continue;
            auto *item = new QTreeWidgetItem(group);
            item->setData(0, kItemKindRole, kServerItem);
            item->setData(0, kServerIndexRole, static_cast<int>(index));
            item->setData(0, kServerIdRole, server.id);
            item->setToolTip(0, QStringLiteral("%1@%2:%3 · %4")
                    .arg(server.user, server.host).arg(server.port).arg(displayGroupName(groupName)));
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
            item->setSizeHint(0, QSize(198, 38));
            m_list->setItemWidget(item, 0, createHostItem(server));
        }
        group->setText(0, QStringLiteral("%1  %2").arg(displayGroupName(groupName)).arg(group->childCount()));
    }
    if (!selected.isEmpty()) selectServerById(selected);
    applyFilter(m_search->text());
}

void HostSidebar::applyFilter(const QString &text)
{
    const auto filter = text.trimmed();
    for (int row = 0; row < m_list->topLevelItemCount(); ++row) {
        auto *group = m_list->topLevelItem(row);
        if (group->data(0, kItemKindRole).toInt() != kGroupItem) continue;
        const bool groupMatch = displayGroupName(group->data(0, kGroupNameRole).toString())
                                    .contains(filter, Qt::CaseInsensitive);
        bool visibleChild = false;
        for (int childIndex = 0; childIndex < group->childCount(); ++childIndex) {
            auto *item = group->child(childIndex);
            const auto profile = profileForItem(item);
            const bool matches = filter.isEmpty() || groupMatch
                || profile.name.contains(filter, Qt::CaseInsensitive)
                || profile.host.contains(filter, Qt::CaseInsensitive);
            item->setHidden(!matches);
            visibleChild |= matches;
        }
        group->setHidden(!filter.isEmpty() && !groupMatch && !visibleChild);
        if (!filter.isEmpty() && (groupMatch || visibleChild)) group->setExpanded(true);
    }
}

void HostSidebar::rebuildMoveMenu()
{
    m_moveMenu->clear();
    const auto profile = m_contextMenu->property("serverProfile").value<ServerProfile>();
    const auto addTarget = [this, &profile](const QString &label, const QString &group) {
        auto *action = m_moveMenu->addAction(label);
        action->setCheckable(true);
        action->setChecked(profile.group.trimmed() == group);
        action->setEnabled(!action->isChecked());
        connect(action, &QAction::triggered, this, [this, id = profile.id, group] {
            moveServerToGroup(id, group);
        });
    };
    addTarget(kUngroupedLabel, QString{});
    for (const auto &group : std::as_const(m_groups)) addTarget(group, group);
}

void HostSidebar::showContextMenu(const QPoint &position)
{
    auto *item = m_list->itemAt(position);
    const auto profile = profileForItem(item);
    const bool isHost = !profile.id.isEmpty();
    const bool isGroup = item && item->data(0, kItemKindRole).toInt() == kGroupItem;
    const auto group = isGroup ? groupForItem(item) : QString{};
    if (item) m_list->setCurrentItem(item);
    m_contextMenu->setProperty("serverProfile", QVariant::fromValue(profile));
    m_contextMenu->setProperty("groupName", group);
    for (auto *action : {m_connectAction, m_editAction, m_duplicateAction, m_copyAddressAction,
             m_deleteAction, m_moveMenu->menuAction()}) action->setVisible(isHost);
    m_newGroupAction->setVisible(!isHost);
    m_newConnectionAction->setVisible(isGroup);
    m_renameGroupAction->setVisible(isGroup && !group.isEmpty());
    m_deleteGroupAction->setVisible(isGroup && !group.isEmpty());
    if (isHost) rebuildMoveMenu();
    m_contextMenu->popup(m_list->viewport()->mapToGlobal(position));
}

} // namespace noxshell::ui
