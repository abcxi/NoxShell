#include "ServerDialog.h"

#include "../core/CredentialStore.h"
#include "../core/ServerRepository.h"
#include "../core/SshSession.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QProgressDialog>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>

namespace noxshell::ui {

namespace {
QString normalizedAsciiCredential(const QString &value, bool &converted, bool &rejected)
{
    QString result;
    result.reserve(value.size());
    for (const auto character : value) {
        const auto code = character.unicode();
        if (code >= 0x20 && code <= 0x7e) {
            result.append(character);
        } else if (code >= 0xff01 && code <= 0xff5e) {
            result.append(QChar(code - 0xfee0));
            converted = true;
        } else if (code == 0x3002 || code == 0xff61) {
            result.append(QLatin1Char('.'));
            converted = true;
        } else if (code == 0x3000 || code == 0x00a0) {
            result.append(QLatin1Char(' '));
            converted = true;
        } else if (code == 0x2018 || code == 0x2019) {
            result.append(QLatin1Char('\''));
            converted = true;
        } else if (code == 0x201c || code == 0x201d) {
            result.append(QLatin1Char('"'));
            converted = true;
        } else if (code == 0x2014) {
            result.append(QLatin1Char('-'));
            converted = true;
        } else if (code == 0x2026) {
            result.append(QStringLiteral("..."));
            converted = true;
        } else {
            rejected = true;
        }
    }
    return result;
}
} // namespace

ServerDialog::ServerDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("新增真实 SSH 主机"));
    setModal(true);
    setMinimumWidth(510);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);

    auto *notice = new QLabel(QStringLiteral("服务器配置保存在 SQLite；密码和私钥口令写入 macOS Keychain，不会进入数据库。"));
    notice->setWordWrap(true);
    notice->setStyleSheet(QStringLiteral("color:#6B7C91;background:#F4F7FA;border:1px solid #DFE6EF;padding:9px;border-radius:4px;"));
    layout->addWidget(notice);

    auto *form = new QFormLayout;
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(9);
    m_name = new QLineEdit;
    m_name->setPlaceholderText(QStringLiteral("例如 production-web"));
    m_host = new QLineEdit;
    m_host->setObjectName(QStringLiteral("hostEditor"));
    m_host->setPlaceholderText(QStringLiteral("IP 地址或域名"));
    m_port = new QSpinBox;
    m_port->setObjectName(QStringLiteral("portEditor"));
    m_port->setRange(1, 65535);
    m_port->setValue(22);
    m_user = new QLineEdit;
    m_user->setPlaceholderText(QStringLiteral("例如 root 或 ops"));
    m_group = new QLineEdit(QStringLiteral("我的主机"));
    m_os = new QLineEdit(QStringLiteral("linux"));
    m_authentication = new QComboBox;
    m_authentication->addItem(QStringLiteral("密码"), static_cast<int>(AuthenticationMethod::Password));
    m_authentication->addItem(QStringLiteral("私钥"), static_cast<int>(AuthenticationMethod::PrivateKey));
    m_authentication->addItem(QStringLiteral("SSH Agent"), static_cast<int>(AuthenticationMethod::SshAgent));
    m_fingerprint = new QLineEdit;
    m_fingerprint->setObjectName(QStringLiteral("fingerprintEditor"));
    m_fingerprint->setPlaceholderText(QStringLiteral("可选；SHA256:…，留空则首次连接时确认"));

    form->addRow(QStringLiteral("主机名称"), m_name);
    form->addRow(QStringLiteral("主机/IP"), m_host);
    form->addRow(QStringLiteral("SSH 端口"), m_port);
    form->addRow(QStringLiteral("用户名"), m_user);
    form->addRow(QStringLiteral("分组"), m_group);
    form->addRow(QStringLiteral("系统标识"), m_os);
    form->addRow(QStringLiteral("认证方式"), m_authentication);

    m_authPages = new QStackedWidget;
    auto *passwordPage = new QWidget;
    auto *passwordLayout = new QVBoxLayout(passwordPage);
    passwordLayout->setContentsMargins(0, 0, 0, 0);
    passwordLayout->setSpacing(4);
    auto *passwordRow = new QWidget;
    auto *passwordRowLayout = new QHBoxLayout(passwordRow);
    passwordRowLayout->setContentsMargins(0, 0, 0, 0);
    passwordRowLayout->setSpacing(6);
    m_password = new QLineEdit;
    m_password->setObjectName(QStringLiteral("passwordEditor"));
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData
        | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase | Qt::ImhLatinOnly);
    m_password->setPlaceholderText(QStringLiteral("SSH 密码"));
    m_passwordReveal = new QToolButton;
    m_passwordReveal->setObjectName(QStringLiteral("passwordRevealButton"));
    m_passwordReveal->setText(QStringLiteral("显示"));
    m_passwordReveal->setCheckable(true);
    m_passwordReveal->setToolTip(QStringLiteral("显示或隐藏当前输入的密码"));
    passwordRowLayout->addWidget(m_password, 1);
    passwordRowLayout->addWidget(m_passwordReveal);
    passwordLayout->addWidget(passwordRow);
    m_passwordHint = new QLabel;
    m_passwordHint->setObjectName(QStringLiteral("passwordSourceHint"));
    m_passwordHint->setWordWrap(true);
    m_passwordHint->setStyleSheet(QStringLiteral("color:#738297;font-size:11px;"));
    passwordLayout->addWidget(m_passwordHint);

    auto *keyPage = new QWidget;
    auto *keyForm = new QFormLayout(keyPage);
    keyForm->setContentsMargins(0, 0, 0, 0);
    auto *privateRow = new QWidget;
    auto *privateLayout = new QHBoxLayout(privateRow);
    privateLayout->setContentsMargins(0, 0, 0, 0);
    privateLayout->setSpacing(6);
    m_privateKey = new QLineEdit(QStringLiteral("~/.ssh/id_ed25519"));
    auto *browse = new QPushButton(QStringLiteral("选择…"));
    privateLayout->addWidget(m_privateKey, 1);
    privateLayout->addWidget(browse);
    m_publicKey = new QLineEdit;
    m_publicKey->setPlaceholderText(QStringLiteral("可选；例如 ~/.ssh/id_ed25519.pub"));
    m_passphrase = new QLineEdit;
    m_passphrase->setObjectName(QStringLiteral("passphraseEditor"));
    m_passphrase->setEchoMode(QLineEdit::Password);
    m_passphrase->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData
        | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase | Qt::ImhLatinOnly);
    m_passphrase->setPlaceholderText(QStringLiteral("私钥口令，可选"));
    keyForm->addRow(QStringLiteral("私钥"), privateRow);
    keyForm->addRow(QStringLiteral("公钥"), m_publicKey);
    keyForm->addRow(QStringLiteral("私钥口令"), m_passphrase);

    auto *agentPage = new QLabel(QStringLiteral("使用当前进程可访问的 SSH_AUTH_SOCK 中的身份。"));
    agentPage->setStyleSheet(QStringLiteral("color:#738297;padding:6px 0;"));
    m_authPages->addWidget(passwordPage);
    m_authPages->addWidget(keyPage);
    m_authPages->addWidget(agentPage);
    form->addRow(QStringLiteral("认证凭据"), m_authPages);
    form->addRow(QStringLiteral("已知指纹"), m_fingerprint);
    layout->addLayout(form);

    m_testStatus = new QLabel(QStringLiteral("保存前可先验证 TCP、SSH 握手、主机指纹和认证凭据。"));
    m_testStatus->setObjectName(QStringLiteral("connectionTestStatus"));
    m_testStatus->setStyleSheet(QStringLiteral("color:#738297;font-size:12px;"));
    m_testStatus->setWordWrap(true);
    layout->addWidget(m_testStatus);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Save);
    m_testButton = new QPushButton(QStringLiteral("连接测试"));
    m_testButton->setObjectName(QStringLiteral("dialogTestConnectionButton"));
    m_testButton->setEnabled(false);
    buttons->addButton(m_testButton, QDialogButtonBox::ActionRole);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存主机"));
    buttons->button(QDialogButtonBox::Save)->setObjectName(QStringLiteral("primaryButton"));
    layout->addWidget(buttons);

    connect(m_authentication, &QComboBox::currentIndexChanged, this, &ServerDialog::updateAuthenticationPage);
    connect(m_passwordReveal, &QToolButton::toggled, this, [this](bool visible) {
        m_password->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
        m_passwordReveal->setText(visible ? QStringLiteral("隐藏") : QStringLiteral("显示"));
    });
    connect(m_password, &QLineEdit::textChanged, this, &ServerDialog::updatePasswordHint);
    connect(m_password, &QLineEdit::textEdited, this, [this](const QString &text) {
        normalizeCredentialInput(m_password, text, true);
    });
    connect(m_passphrase, &QLineEdit::textEdited, this, [this](const QString &text) {
        normalizeCredentialInput(m_passphrase, text, false);
    });
    connect(browse, &QPushButton::clicked, this, &ServerDialog::choosePrivateKey);
    connect(m_testButton, &QPushButton::clicked, this, &ServerDialog::testConnection);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, &ServerDialog::validateAndAccept);
    updatePasswordHint();
    updateAuthenticationPage();
}

ServerDialog::ServerDialog(const ServerProfile &profile, QWidget *parent)
    : ServerDialog(parent)
{
    m_editing = true;
    m_original = profile;
    setWindowTitle(QStringLiteral("编辑 SSH 主机"));
    populate(profile);
    const auto clearFingerprintForChangedEndpoint = [this] {
        const bool endpointChanged = m_host->text().trimmed() != m_original.host
            || static_cast<quint16>(m_port->value()) != m_original.port;
        if (!endpointChanged || m_fingerprint->text().trimmed() != m_original.expectedFingerprint.trimmed()) return;
        m_fingerprint->clear();
        m_fingerprint->setPlaceholderText(QStringLiteral("地址已变更；首次连接时重新确认指纹"));
    };
    connect(m_host, &QLineEdit::textChanged, this, clearFingerprintForChangedEndpoint);
    connect(m_port, &QSpinBox::valueChanged, this, clearFingerprintForChangedEndpoint);
    m_password->setPlaceholderText(QStringLiteral("留空则沿用现有密码"));
    m_passphrase->setPlaceholderText(QStringLiteral("留空则沿用现有私钥口令"));
    if (auto *buttons = findChild<QDialogButtonBox *>()) {
        buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存修改"));
    }
    updatePasswordHint();
}

ServerProfile ServerDialog::profile() const
{
    ServerProfile result;
    result.name = m_name->text().trimmed();
    result.host = m_host->text().trimmed();
    result.port = static_cast<quint16>(m_port->value());
    result.user = m_user->text().trimmed();
    result.group = m_group->text().trimmed();
    result.os = m_os->text().trimmed();
    result.state = ServerState::Offline;
    result.connectionMode = ConnectionMode::Ssh;
    result.authentication = static_cast<AuthenticationMethod>(m_authentication->currentData().toInt());
    result.password = m_password->text();
    result.privateKeyPath = m_privateKey->text().trimmed();
    result.publicKeyPath = m_publicKey->text().trimmed();
    result.keyPassphrase = m_passphrase->text();
    result.expectedFingerprint = m_fingerprint->text().trimmed();
    result.id = m_original.id;
    result.credentialRef = m_original.credentialRef;
    return result;
}

void ServerDialog::setConnectionServices(ServerRepository *repository, CredentialStore *credentialStore)
{
    m_repository = repository;
    m_credentialStore = credentialStore;
    m_testButton->setEnabled(m_repository && m_credentialStore);
}

void ServerDialog::populate(const ServerProfile &profile)
{
    m_name->setText(profile.name);
    m_host->setText(profile.host);
    m_port->setValue(profile.port);
    m_user->setText(profile.user);
    m_group->setText(profile.group);
    m_os->setText(profile.os);
    const int index = m_authentication->findData(static_cast<int>(profile.authentication));
    if (index >= 0) m_authentication->setCurrentIndex(index);
    m_privateKey->setText(profile.privateKeyPath);
    m_publicKey->setText(profile.publicKeyPath);
    m_fingerprint->setText(profile.expectedFingerprint);
    updateAuthenticationPage();
}

void ServerDialog::updateAuthenticationPage()
{
    m_authPages->setCurrentIndex(m_authentication->currentIndex());
}

void ServerDialog::choosePrivateKey()
{
    const auto file = QFileDialog::getOpenFileName(this, QStringLiteral("选择 SSH 私钥"), QDir::homePath() + QStringLiteral("/.ssh"));
    if (!file.isEmpty()) m_privateKey->setText(file);
}

void ServerDialog::updatePasswordHint()
{
    if (!m_passwordHint || !m_password) return;
    m_passwordHint->setStyleSheet(QStringLiteral("color:#738297;font-size:11px;"));
    if (!m_password->text().isEmpty()) {
        m_passwordHint->setText(m_editing
            ? QStringLiteral("连接测试将使用当前输入；保存后替换旧密码。密码框固定为英文半角。")
            : QStringLiteral("连接测试和保存将使用当前输入。密码框固定为英文半角。"));
        return;
    }
    m_passwordHint->setText(m_editing
        ? QStringLiteral("密码不会从 Keychain 回填；留空将沿用已保存密码。重新输入时请使用英文半角。")
        : QStringLiteral("请输入 SSH 密码；中文/全角标点会自动转换为英文半角。"));
}

void ServerDialog::normalizeCredentialInput(QLineEdit *editor, const QString &text, bool showPasswordHint)
{
    if (!editor) return;
    bool converted = false;
    bool rejected = false;
    const auto normalized = normalizedAsciiCredential(text, converted, rejected);
    if (!converted && !rejected) return;

    bool prefixConverted = false;
    bool prefixRejected = false;
    const auto cursor = normalizedAsciiCredential(
        text.left(editor->cursorPosition()), prefixConverted, prefixRejected).size();
    editor->setText(normalized);
    editor->setCursorPosition(qMin(cursor, normalized.size()));
    if (!showPasswordHint || !m_passwordHint) return;

    const auto warning = rejected
        ? QStringLiteral("检测到中文或非英文字符：已忽略无法转换的内容，请使用英文输入法核对密码。")
        : QStringLiteral("已将中文/全角标点自动转换为英文半角，请点击“显示”核对。");
    const auto showWarning = [this, warning] {
        if (!m_passwordHint) return;
        m_passwordHint->setStyleSheet(QStringLiteral("color:#C65D00;font-size:11px;"));
        m_passwordHint->setText(warning);
    };
    showWarning();
    // QLineEdit 会在 textEdited 之后继续发送 textChanged；排队重设一次，避免通用提示覆盖转换警告。
    QTimer::singleShot(0, this, showWarning);
}

void ServerDialog::validateAndAccept()
{
    const auto value = profile();
    if (!validateProfile(value, m_editing)) return;
    accept();
}

bool ServerDialog::validateProfile(const ServerProfile &value, bool allowStoredPassword)
{
    if (value.name.isEmpty() || value.host.isEmpty() || value.user.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("字段不完整"), QStringLiteral("主机名称、主机/IP 和用户名不能为空。"));
        return false;
    }
    if (!allowStoredPassword && value.authentication == AuthenticationMethod::Password && value.password.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少密码"), QStringLiteral("密码认证需要输入 SSH 密码。"));
        return false;
    }
    if (value.authentication == AuthenticationMethod::PrivateKey && value.privateKeyPath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少私钥"), QStringLiteral("私钥认证需要选择私钥文件。"));
        return false;
    }
    return true;
}

bool ServerDialog::prepareConnectionTestProfile(ServerProfile &candidate)
{
    m_testUsesStoredPassword = false;
    if (!validateProfile(candidate, m_editing)) return false;

    CredentialSecret storedSecret;
    if (m_editing && m_credentialStore && !m_original.credentialRef.isEmpty()
        && (candidate.password.isEmpty() || candidate.keyPassphrase.isEmpty())) {
        storedSecret = m_credentialStore->load(m_original.credentialRef);
    }
    if (candidate.authentication == AuthenticationMethod::Password && candidate.password.isEmpty()) {
        candidate.password = storedSecret.password;
        if (candidate.password.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("无法测试"),
                QStringLiteral("未输入密码，且未能读取已保存的 SSH 密码。"));
            return false;
        }
        m_testUsesStoredPassword = true;
    }
    if (candidate.authentication == AuthenticationMethod::PrivateKey && candidate.keyPassphrase.isEmpty()) {
        candidate.keyPassphrase = storedSecret.keyPassphrase;
    }

    // 测试必须使用窗口内当前输入，清空引用可避免 SshSession 用 Keychain 旧值覆盖它。
    candidate.credentialRef.clear();
    candidate.connectionMode = ConnectionMode::Ssh;
    candidate.state = ServerState::Offline;
    return true;
}

void ServerDialog::testConnection()
{
    if (!m_repository || !m_credentialStore) return;
    auto candidate = profile();
    if (!prepareConnectionTestProfile(candidate)) return;

    m_testButton->setEnabled(false);
    const auto credentialSource = m_testUsesStoredPassword
        ? QStringLiteral("Keychain 已保存密码")
        : QStringLiteral("当前输入密码");
    m_testStatus->setText(QStringLiteral("正在测试 %1@%2:%3…（%4）")
        .arg(candidate.user, candidate.host).arg(candidate.port).arg(credentialSource));
    m_testStatus->setStyleSheet(QStringLiteral("color:#738297;font-size:12px;"));
    auto *progress = new QProgressDialog(
        QStringLiteral("正在测试 TCP、SSH 握手、主机指纹和认证…"),
        QStringLiteral("取消"), 0, 0, this);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->setWindowTitle(QStringLiteral("连接测试 · %1").arg(candidate.name));
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);

    auto *testSession = new SshSession(m_repository, nullptr, progress);
    testSession->setTransferPersistenceEnabled(false);
    connect(progress, &QProgressDialog::canceled, progress, [this, progress, testSession] {
        if (progress->property("connectionTestFinished").toBool()) return;
        progress->setProperty("connectionTestFinished", true);
        m_testStatus->setText(QStringLiteral("连接测试已取消。"));
        testSession->disconnectFromHost();
        progress->close();
    });
    connect(progress, &QDialog::finished, this, [this] {
        m_testButton->setEnabled(m_repository && m_credentialStore);
    });
    connect(testSession, &SshSession::hostKeyVerificationRequired, progress,
        [this, progress, testSession](const QString &target, const QString &fingerprint, const QString &algorithm) {
            const auto answer = QMessageBox::warning(this, QStringLiteral("连接测试：确认指纹"),
                QStringLiteral("目标：%1\n算法：%2\n指纹：%3\n\n请通过可信渠道核对后再接受。")
                    .arg(target, algorithm, fingerprint),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            const bool approved = answer == QMessageBox::Yes;
            if (approved) m_fingerprint->setText(fingerprint);
            testSession->approveHostKey(approved);
            if (!approved) progress->setLabelText(QStringLiteral("已拒绝未知主机指纹。"));
        });
    connect(testSession, &SshSession::connectionChanged, progress,
        [this, progress, testSession](bool connected, const QString &message) {
            if (progress->property("connectionTestFinished").toBool()) return;
            progress->setLabelText(message);
            if (connected) {
                progress->setProperty("connectionTestFinished", true);
                m_testStatus->setText(QStringLiteral("✓ 连接测试通过：%1").arg(message));
                m_testStatus->setStyleSheet(QStringLiteral("color:#008858;font-size:12px;"));
                testSession->disconnectFromHost();
                progress->close();
                QMessageBox::information(this, QStringLiteral("连接测试成功"), message);
            } else if (message.contains(QStringLiteral("失败")) || message.contains(QStringLiteral("阻断"))) {
                progress->setProperty("connectionTestFinished", true);
                auto displayMessage = message;
                if (m_testUsesStoredPassword && message.contains(QStringLiteral("SSH 认证失败"))) {
                    displayMessage += QStringLiteral("\n\n本次使用的是 Keychain 已保存密码。请在密码框重新输入其他客户端实际使用的密码后再测试；若其他客户端使用私钥，请切换认证方式。");
                }
                m_testStatus->setText(QStringLiteral("✕ 连接测试失败：%1").arg(displayMessage));
                m_testStatus->setStyleSheet(QStringLiteral("color:#D54941;font-size:12px;"));
                progress->close();
                QMessageBox::critical(this, QStringLiteral("连接测试失败"), displayMessage);
            }
        });
    testSession->connectTo(candidate);
    progress->show();
}

} // namespace noxshell::ui
