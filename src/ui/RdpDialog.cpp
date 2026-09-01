#include "RdpDialog.h"

#include <QComboBox>
#include <QAction>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QIcon>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QSpinBox>
#include <QVBoxLayout>

namespace noxshell::ui {

namespace {
const auto kNoGroupLabel = QStringLiteral("不设置分组（可选）");

void selectGroup(QComboBox *combo, const QString &group)
{
    if (!combo) return;
    const auto normalized = group.trimmed();
    const int index = combo->findData(normalized);
    combo->setCurrentIndex(index >= 0 ? index : 0);
}
} // namespace

RdpDialog::RdpDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("rdpServerDialog"));
    setWindowTitle(QStringLiteral("新增 Windows 远程桌面"));
    setModal(true);
    setMinimumWidth(510);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);

    auto *notice = new QLabel(QStringLiteral(
        "连接配置保存在 SQLite；Windows 密码写入系统凭据库，不会进入数据库。"
        "macOS 打开 Windows App 时会临时复制密码，便于在客户端粘贴。"));
    notice->setObjectName(QStringLiteral("serverStorageNotice"));
    notice->setWordWrap(true);
    layout->addWidget(notice);

    auto *form = new QFormLayout;
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(9);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_name = new QLineEdit;
    m_name->setObjectName(QStringLiteral("rdpNameEditor"));
    m_name->setPlaceholderText(QStringLiteral("例如 Windows 办公电脑"));
    m_host = new QLineEdit;
    m_host->setObjectName(QStringLiteral("rdpHostEditor"));
    m_host->setPlaceholderText(QStringLiteral("IP 地址或域名"));
    m_port = new QSpinBox;
    m_port->setObjectName(QStringLiteral("rdpPortEditor"));
    m_port->setRange(1, 65535);
    m_port->setValue(3389);
    m_port->setFixedWidth(92);
    m_user = new QLineEdit;
    m_user->setObjectName(QStringLiteral("rdpUserEditor"));
    m_user->setPlaceholderText(QStringLiteral("可选；例如 Administrator 或 DOMAIN\\user"));
    m_password = new QLineEdit;
    m_password->setObjectName(QStringLiteral("rdpPasswordEditor"));
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData
        | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    m_password->setPlaceholderText(QStringLiteral("Windows 登录密码（可选）"));
    m_passwordReveal = m_password->addAction(
        QIcon(QStringLiteral(":/assets/eye.svg")), QLineEdit::TrailingPosition);
    m_passwordReveal->setObjectName(QStringLiteral("rdpPasswordRevealAction"));
    m_passwordReveal->setCheckable(true);
    m_passwordReveal->setToolTip(QStringLiteral("显示密码"));
    m_group = new QComboBox;
    m_group->setObjectName(QStringLiteral("rdpGroupEditor"));
    m_group->setMaxVisibleItems(12);
    m_group->addItem(kNoGroupLabel, QString{});

    for (auto *editor : {static_cast<QWidget *>(m_name), static_cast<QWidget *>(m_host),
             static_cast<QWidget *>(m_port), static_cast<QWidget *>(m_user),
             static_cast<QWidget *>(m_password), static_cast<QWidget *>(m_group)}) {
        editor->setMinimumHeight(32);
        editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    auto *endpointRow = new QWidget;
    endpointRow->setObjectName(QStringLiteral("rdpEndpointEditorRow"));
    auto *endpointLayout = new QHBoxLayout(endpointRow);
    endpointLayout->setContentsMargins(0, 0, 0, 0);
    endpointLayout->setSpacing(8);
    auto *portLabel = new QLabel(QStringLiteral("端口"));
    portLabel->setObjectName(QStringLiteral("portInlineLabel"));
    endpointLayout->addWidget(m_host, 1);
    endpointLayout->addWidget(portLabel);
    endpointLayout->addWidget(m_port);

    form->addRow(QStringLiteral("连接名称"), m_name);
    form->addRow(QStringLiteral("电脑/IP"), endpointRow);
    form->addRow(QStringLiteral("Windows 用户"), m_user);
    form->addRow(QStringLiteral("Windows 密码"), m_password);
    form->addRow(QStringLiteral("分组（可选）"), m_group);
    layout->addLayout(form);

    auto *hint = new QLabel(QStringLiteral(
        "Windows 将调用 mstsc；macOS 将调用 Microsoft Windows App。"
        "首次连接时请在系统客户端中输入密码并确认服务器证书。"));
    hint->setObjectName(QStringLiteral("connectionTestStatus"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Save);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存远程桌面"));
    buttons->button(QDialogButtonBox::Save)->setObjectName(QStringLiteral("primaryButton"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, &RdpDialog::validateAndAccept);
    connect(m_passwordReveal, &QAction::toggled, this, [this](bool visible) {
        m_password->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
        m_passwordReveal->setIcon(QIcon(visible
            ? QStringLiteral(":/assets/eye-off.svg") : QStringLiteral(":/assets/eye.svg")));
        m_passwordReveal->setToolTip(visible ? QStringLiteral("隐藏密码") : QStringLiteral("显示密码"));
    });
}

RdpDialog::RdpDialog(const ServerProfile &profile, QWidget *parent)
    : RdpDialog(parent)
{
    m_original = profile;
    const auto group = profile.group.trimmed();
    if (!group.isEmpty() && m_group->findData(group) < 0) m_group->addItem(group, group);
    setWindowTitle(QStringLiteral("编辑 Windows 远程桌面"));
    populate(profile);
    m_password->setPlaceholderText(QStringLiteral("留空则沿用已保存密码"));
    if (auto *buttons = findChild<QDialogButtonBox *>()) {
        buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存修改"));
    }
}

ServerProfile RdpDialog::profile() const
{
    ServerProfile result = m_original;
    result.name = m_name->text().trimmed();
    result.host = m_host->text().trimmed();
    result.port = static_cast<quint16>(m_port->value());
    result.user = m_user->text().trimmed();
    result.group = m_group->currentData().toString().trimmed();
    result.os = QStringLiteral("windows");
    result.state = ServerState::Offline;
    result.connectionMode = ConnectionMode::Rdp;
    result.authentication = AuthenticationMethod::Password;
    result.password = m_password->text();
    result.privateKeyPath.clear();
    result.publicKeyPath.clear();
    result.keyPassphrase.clear();
    result.expectedFingerprint.clear();
    return result;
}

void RdpDialog::setAvailableGroups(const QStringList &groups)
{
    const auto current = m_group->currentData().toString().trimmed();
    m_group->clear();
    m_group->addItem(kNoGroupLabel, QString{});
    for (const auto &group : groups) {
        const auto normalized = group.trimmed();
        if (!normalized.isEmpty() && m_group->findData(normalized) < 0) m_group->addItem(normalized, normalized);
    }
    selectGroup(m_group, current);
}

void RdpDialog::setInitialGroup(const QString &group)
{
    selectGroup(m_group, group);
}

void RdpDialog::populate(const ServerProfile &profile)
{
    m_name->setText(profile.name);
    m_host->setText(profile.host);
    m_port->setValue(profile.port == 0 ? 3389 : profile.port);
    m_user->setText(profile.user);
    selectGroup(m_group, profile.group);
}

void RdpDialog::validateAndAccept()
{
    const auto value = profile();
    if (value.name.isEmpty() || value.host.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("字段不完整"),
            QStringLiteral("连接名称和电脑/IP 不能为空。"));
        return;
    }
    accept();
}

} // namespace noxshell::ui
