#pragma once

#include "../core/ServerProfile.h"

#include <QFrame>
#include <QStringList>
#include <QVector>

class QLineEdit;
class QMenu;
class QTreeWidget;
class QTreeWidgetItem;
class QAction;

namespace noxshell::ui {

class HostSidebar final : public QFrame {
    Q_OBJECT

public:
    explicit HostSidebar(QVector<ServerProfile> servers = {}, QStringList groups = {}, QWidget *parent = nullptr);

    [[nodiscard]] QVector<ServerProfile> servers() const { return m_servers; }
    void selectFirstServer();
    bool selectServerById(const QString &serverId);
    void addServer(const ServerProfile &profile);
    bool updateServer(const ServerProfile &profile);
    bool setServerState(const QString &serverId, ServerState state);
    bool removeServer(const QString &id);
    void addGroup(const QString &name);
    void renameGroup(const QString &oldName, const QString &newName);
    void removeGroup(const QString &name);
    bool moveServerToGroup(const QString &serverId, const QString &group);

signals:
    void serverSelected(const ServerProfile &profile);
    void serverConnectRequested(const ServerProfile &profile);
    void serverEditRequested(const ServerProfile &profile);
    void serverDuplicateRequested(const ServerProfile &profile);
    void serverDeleteRequested(const ServerProfile &profile);
    void serverGroupChanged(const ServerProfile &profile);
    void groupCreateRequested(const QString &name);
    void groupRenameRequested(const QString &oldName, const QString &newName);
    void groupDeleteRequested(const QString &name);
    void addServerRequested();
    void addServerInGroupRequested(const QString &group);
    void collapseRequested();

private:
    void populate();
    void applyFilter(const QString &text);
    [[nodiscard]] ServerProfile profileForItem(const QTreeWidgetItem *item) const;
    [[nodiscard]] QTreeWidgetItem *groupItem(const QString &group) const;
    [[nodiscard]] QString groupForItem(const QTreeWidgetItem *item) const;
    void showContextMenu(const QPoint &position);
    void rebuildMoveMenu();

    QLineEdit *m_search{};
    QTreeWidget *m_list{};
    QMenu *m_contextMenu{};
    QMenu *m_moveMenu{};
    QAction *m_connectAction{};
    QAction *m_editAction{};
    QAction *m_duplicateAction{};
    QAction *m_copyAddressAction{};
    QAction *m_deleteAction{};
    QAction *m_newGroupAction{};
    QAction *m_newConnectionAction{};
    QAction *m_renameGroupAction{};
    QAction *m_deleteGroupAction{};
    QVector<ServerProfile> m_servers;
    QStringList m_groups;
};

} // namespace noxshell::ui
