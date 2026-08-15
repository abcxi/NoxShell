#include "HostSidebar.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QIcon>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace noxshell::ui {

namespace {
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
    layout->setContentsMargins(8, 3, 8, 3);
    layout->setSpacing(8);
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

HostSidebar::HostSidebar(QVector<ServerProfile> servers, QWidget *parent)
    : QFrame(parent)
    , m_servers(std::move(servers))
{
    setObjectName(QStringLiteral("hostSidebar"));
    setFixedWidth(244);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 12, 9, 8);
    layout->setSpacing(8);

    auto *filterRow = new QHBoxLayout;
    filterRow->setContentsMargins(0, 0, 0, 0);
    filterRow->setSpacing(7);
    m_search = new QLineEdit;
    m_search->setObjectName(QStringLiteral("hostSearch"));
    m_search->setPlaceholderText(QStringLiteral("搜索名称或 IP"));
    m_search->setClearButtonEnabled(true);
    auto *addButton = new QPushButton(QStringLiteral("+"));
    addButton->setObjectName(QStringLiteral("hostAddButton"));
    addButton->setToolTip(QStringLiteral("新增 SSH 主机"));
    filterRow->addWidget(m_search, 1);
    filterRow->addWidget(addButton);
    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("hostList"));
    m_list->setSpacing(1);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);

    m_contextMenu = new QMenu(this);
    auto *connectAction = m_contextMenu->addAction(QStringLiteral("连接"));
    auto *editAction = m_contextMenu->addAction(QStringLiteral("编辑"));
    auto *duplicateAction = m_contextMenu->addAction(QStringLiteral("复制主机配置"));
    auto *copyAddressAction = m_contextMenu->addAction(QStringLiteral("复制连接地址"));
    m_contextMenu->addSeparator();
    auto *deleteAction = m_contextMenu->addAction(QStringLiteral("删除"));
    connectAction->setObjectName(QStringLiteral("hostConnectAction"));
    editAction->setObjectName(QStringLiteral("hostEditAction"));
    duplicateAction->setObjectName(QStringLiteral("hostDuplicateAction"));
    copyAddressAction->setObjectName(QStringLiteral("hostCopyAddressAction"));
    deleteAction->setObjectName(QStringLiteral("hostDeleteAction"));

    auto *credentialButton = new QPushButton(QStringLiteral("⚙  连接与凭据"));
    credentialButton->setFlat(true);
    credentialButton->setStyleSheet(QStringLiteral("text-align:left;color:#66768A;border-top:1px solid #DFE6EF;"));

    layout->addLayout(filterRow);
    layout->addWidget(m_list, 1);
    layout->addWidget(credentialButton);

    populate();

    connect(addButton, &QPushButton::clicked, this, &HostSidebar::addServerRequested);
    connect(m_search, &QLineEdit::textChanged, this, &HostSidebar::applyFilter);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0) {
            return;
        }
        const auto index = m_list->item(row)->data(Qt::UserRole).toInt();
        if (index >= 0 && index < m_servers.size()) {
            emit serverSelected(m_servers.at(index));
        }
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        const auto profile = profileForItem(item);
        if (profile.id.isEmpty()) return;
        emit serverConnectRequested(profile);
        emit collapseRequested();
    });
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        auto *item = m_list->itemAt(position);
        const auto profile = profileForItem(item);
        if (profile.id.isEmpty()) return;
        m_list->setCurrentItem(item);
        m_contextMenu->setProperty("serverProfile", QVariant::fromValue(profile));
        m_contextMenu->popup(m_list->viewport()->mapToGlobal(position));
    });
    const auto contextProfile = [this] {
        return m_contextMenu->property("serverProfile").value<ServerProfile>();
    };
    connect(connectAction, &QAction::triggered, this, [this, contextProfile] {
        emit serverConnectRequested(contextProfile());
    });
    connect(editAction, &QAction::triggered, this, [this, contextProfile] {
        emit serverEditRequested(contextProfile());
    });
    connect(duplicateAction, &QAction::triggered, this, [this, contextProfile] {
        emit serverDuplicateRequested(contextProfile());
    });
    connect(copyAddressAction, &QAction::triggered, this, [contextProfile] {
        const auto profile = contextProfile();
        QApplication::clipboard()->setText(QStringLiteral("%1@%2:%3").arg(profile.user, profile.host).arg(profile.port));
    });
    connect(deleteAction, &QAction::triggered, this, [this, contextProfile] {
        emit serverDeleteRequested(contextProfile());
    });

}

ServerProfile HostSidebar::profileForItem(const QListWidgetItem *item) const
{
    if (!item) return {};
    const auto index = item->data(Qt::UserRole).toInt();
    return index >= 0 && index < m_servers.size() ? m_servers.at(index) : ServerProfile{};
}

void HostSidebar::selectFirstServer()
{
    if (!m_servers.isEmpty() && m_list->count() > 0) {
        m_list->setCurrentRow(0);
    }
}

bool HostSidebar::selectServerById(const QString &serverId)
{
    for (int row = 0; row < m_list->count(); ++row) {
        auto *item = m_list->item(row);
        const auto index = item->data(Qt::UserRole).toInt();
        if (index < 0 || index >= m_servers.size() || m_servers.at(index).id != serverId) continue;
        const QSignalBlocker blocker(m_list);
        m_list->setCurrentRow(row);
        return true;
    }
    return false;
}

void HostSidebar::addServer(const ServerProfile &profile)
{
    m_servers.append(profile);
    populate();
    m_list->setCurrentRow(m_list->count() - 1);
}

bool HostSidebar::updateServer(const ServerProfile &profile)
{
    for (qsizetype index = 0; index < m_servers.size(); ++index) {
        if (m_servers.at(index).id == profile.id) {
            m_servers[index] = profile;
            populate();
            m_list->setCurrentRow(static_cast<int>(index));
            return true;
        }
    }
    return false;
}

bool HostSidebar::setServerState(const QString &serverId, ServerState state)
{
    for (qsizetype index = 0; index < m_servers.size(); ++index) {
        if (m_servers.at(index).id != serverId) continue;
        m_servers[index].state = state;
        return true;
    }
    return false;
}

bool HostSidebar::removeServer(const QString &id)
{
    for (qsizetype index = 0; index < m_servers.size(); ++index) {
        if (m_servers.at(index).id == id) {
            m_servers.removeAt(index);
            populate();
            if (!m_servers.isEmpty()) {
                m_list->setCurrentRow(qMin(static_cast<int>(index), m_list->count() - 1));
            }
            return true;
        }
    }
    return false;
}

void HostSidebar::populate()
{
    m_list->clear();
    if (m_servers.isEmpty()) {
        auto *item = new QListWidgetItem(QStringLiteral("暂无 SSH 主机\n点击搜索栏右侧 + 添加"), m_list);
        item->setFlags(Qt::NoItemFlags);
        item->setForeground(QColor(QStringLiteral("#8794A5")));
        item->setTextAlignment(Qt::AlignCenter);
        item->setSizeHint(QSize(218, 72));
        return;
    }
    for (qsizetype index = 0; index < m_servers.size(); ++index) {
        const auto &server = m_servers.at(index);
        auto *item = new QListWidgetItem(m_list);
        item->setData(Qt::UserRole, static_cast<int>(index));
        item->setToolTip(QStringLiteral("%1@%2:%3%4")
                .arg(server.user, server.host)
                .arg(server.port)
                .arg(server.group.isEmpty() ? QString{} : QStringLiteral(" · %1").arg(server.group)));
        item->setSizeHint(QSize(218, 42));
        m_list->setItemWidget(item, createHostItem(server));
    }
}

void HostSidebar::applyFilter(const QString &text)
{
    for (int row = 0; row < m_list->count(); ++row) {
        auto *item = m_list->item(row);
        const auto index = item->data(Qt::UserRole).toInt();
        if (index < 0 || index >= m_servers.size()) continue;
        const auto &server = m_servers.at(index);
        item->setHidden(!server.name.contains(text, Qt::CaseInsensitive)
                        && !server.host.contains(text, Qt::CaseInsensitive)
                        && !server.group.contains(text, Qt::CaseInsensitive));
    }
}

} // namespace noxshell::ui
