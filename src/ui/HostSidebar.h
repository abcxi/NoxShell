#pragma once

#include "../core/ServerProfile.h"

#include <QFrame>
#include <QVector>

class QListWidget;
class QListWidgetItem;
class QLineEdit;
class QMenu;

namespace noxshell::ui {

class HostSidebar final : public QFrame {
    Q_OBJECT

public:
    explicit HostSidebar(QVector<ServerProfile> servers = {}, QWidget *parent = nullptr);

    [[nodiscard]] QVector<ServerProfile> servers() const { return m_servers; }
    void selectFirstServer();
    bool selectServerById(const QString &serverId);
    void addServer(const ServerProfile &profile);
    bool updateServer(const ServerProfile &profile);
    bool setServerState(const QString &serverId, ServerState state);
    bool removeServer(const QString &id);

signals:
    void serverSelected(const ServerProfile &profile);
    void serverConnectRequested(const ServerProfile &profile);
    void serverEditRequested(const ServerProfile &profile);
    void serverDuplicateRequested(const ServerProfile &profile);
    void serverDeleteRequested(const ServerProfile &profile);
    void addServerRequested();
    void collapseRequested();

private:
    void populate();
    void applyFilter(const QString &text);
    [[nodiscard]] ServerProfile profileForItem(const QListWidgetItem *item) const;

    QLineEdit *m_search{};
    QListWidget *m_list{};
    QMenu *m_contextMenu{};
    QVector<ServerProfile> m_servers;
};

} // namespace noxshell::ui
