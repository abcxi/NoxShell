#include "CredentialManagerPanel.h"
#include "CredentialInput.h"
#include "../core/ServerRepository.h"
#include "../core/SavedCredentialManager.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace noxshell::ui {
CredentialManagerPanel::CredentialManagerPanel(ServerRepository *repository, CredentialStore *store, QWidget *parent)
    : QWidget(parent), m_repository(repository), m_manager(new SavedCredentialManager(repository, store, this))
{
    setObjectName(QStringLiteral("credentialManagerPanel"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 16, 0, 0);
    auto *hint = new QLabel(QStringLiteral("保存常用账号密码；新增或编辑连接时可直接选择。修改账密会应用于关联连接的下次登录。"));
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("recentLoginHint"));
    layout->addWidget(hint);
    auto *actions = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("＋ 新增账密"));
    add->setObjectName(QStringLiteral("addSavedCredential"));
    add->setEnabled(repository && store);
    m_edit = new QPushButton(QStringLiteral("编辑"));
    m_edit->setObjectName(QStringLiteral("editSavedCredential"));
    m_remove = new QPushButton(QStringLiteral("删除"));
    m_remove->setObjectName(QStringLiteral("deleteSavedCredential"));
    actions->addWidget(add); actions->addWidget(m_edit); actions->addWidget(m_remove); actions->addStretch();
    layout->addLayout(actions);
    m_list = new QTreeWidget;
    m_list->setObjectName(QStringLiteral("savedCredentialList"));
    m_list->setHeaderLabels({QStringLiteral("名称"), QStringLiteral("用户名"), QStringLiteral("密码"), QStringLiteral("关联连接")});
    m_list->setRootIsDecorated(false);
    m_list->setAlternatingRowColors(true);
    m_list->setUniformRowHeights(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_list->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    layout->addWidget(m_list, 1);
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("credentialManagerStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    connect(add, &QPushButton::clicked, this, [this] { edit(true); });
    connect(m_edit, &QPushButton::clicked, this, [this] { edit(false); });
    connect(m_remove, &QPushButton::clicked, this, &CredentialManagerPanel::removeSelected);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this] { edit(false); });
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, [this] {
        m_edit->setEnabled(m_list->currentItem()); m_remove->setEnabled(m_list->currentItem());
    });
    connect(m_manager, &SavedCredentialManager::changed, this, [this] {
        refresh();
        if (!m_manager->lastError().isEmpty()) m_status->setText(m_manager->lastError());
        emit credentialsChanged();
    });
    refresh();
}

void CredentialManagerPanel::refresh()
{
    if (!m_repository) {
        m_edit->setEnabled(false); m_remove->setEnabled(false);
        m_status->setText(QStringLiteral("密码管理暂不可用"));
        return;
    }
    const auto selected = m_list->currentItem() ? m_list->currentItem()->data(0, Qt::UserRole).toString() : QString{};
    m_entries = m_repository->loadSavedCredentials();
    const auto error = m_repository->lastError();
    m_list->clear();
    const auto servers = m_repository->loadServers();
    QHash<QString, int> uses;
    for (const auto &server : servers) if (!server.credentialRef.isEmpty()) ++uses[server.credentialRef];
    for (const auto &entry : m_entries) {
        auto *item = new QTreeWidgetItem(m_list, {entry.name, entry.user, QStringLiteral("●●●●●●  已保存"), QString::number(uses.value(entry.credentialRef))});
        item->setData(0, Qt::UserRole, entry.id);
        if (entry.id == selected) m_list->setCurrentItem(item);
    }
    m_edit->setEnabled(m_list->currentItem()); m_remove->setEnabled(m_list->currentItem());
    m_status->setText(!error.isEmpty() ? error : m_entries.isEmpty()
        ? QStringLiteral("还没有账密，点击“新增账密”添加常用账号。")
        : QStringLiteral("共 %1 项 · 密码已加密保存").arg(m_entries.size()));
}

void CredentialManagerPanel::edit(bool adding)
{
    SavedCredential original;
    if (!adding) {
        if (!m_list->currentItem()) return;
        const auto id = m_list->currentItem()->data(0, Qt::UserRole).toString();
        for (const auto &entry : m_entries) if (entry.id == id) original = entry;
        if (original.id.isEmpty()) return;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("savedCredentialDialog"));
    dialog.setWindowTitle(adding ? QStringLiteral("新增账密") : QStringLiteral("编辑账密"));
    dialog.setMinimumWidth(430);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *name = new QLineEdit(original.name);
    name->setObjectName(QStringLiteral("savedCredentialName"));
    name->setPlaceholderText(QStringLiteral("例如 生产环境 root"));
    auto *user = new QLineEdit(original.user);
    user->setObjectName(QStringLiteral("savedCredentialUser"));
    auto *password = new QLineEdit;
    password->setObjectName(QStringLiteral("savedCredentialPassword"));
    password->setEchoMode(QLineEdit::Password);
    password->setPlaceholderText(adding ? QStringLiteral("输入密码") : QStringLiteral("●●●●●● 已保存 · 留空保留，输入替换"));
    form->addRow(QStringLiteral("名称"), name);
    form->addRow(QStringLiteral("用户名"), user);
    form->addRow(QStringLiteral("密码"), password);
    auto *inputHint = new QLabel;
    configureAsciiCredentialInput(password, inputHint);
    form->addRow(inputHint);
    layout->addLayout(form);
    auto *status = new QLabel;
    status->setObjectName(QStringLiteral("savedCredentialError"));
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        auto value = original;
        value.name = name->text(); value.user = user->text();
        if (!m_manager->save(value, password->text(), adding ? nullptr : &original)) {
            status->setText(m_manager->lastError()); return;
        }
        password->clear();
        dialog.accept();
    });
    dialog.exec();
}

void CredentialManagerPanel::removeSelected()
{
    if (!m_list->currentItem()) return;
    const auto id = m_list->currentItem()->data(0, Qt::UserRole).toString();
    for (const auto &entry : m_entries) {
        if (entry.id != id) continue;
        if (QMessageBox::question(this, QStringLiteral("删除账密"),
                QStringLiteral("确认删除“%1”？被连接使用的账密需先解除关联。").arg(entry.name),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
        // changed() refreshes m_entries synchronously, so pass an owned copy.
        const auto owned = entry;
        if (!m_manager->remove(owned)) m_status->setText(m_manager->lastError());
        return;
    }
}
}
