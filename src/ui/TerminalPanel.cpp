#include "TerminalPanel.h"
#include <QCheckBox>

#include "../core/SshSession.h"
#include "../core/ServerRepository.h"
#include "CommandHistoryPanel.h"
#include "CredentialInput.h"
#include "TerminalView.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QStackedLayout>
#include <QToolButton>
#include <QVBoxLayout>

namespace noxshell::ui {

namespace {
bool isConnectionProgress(const QString &message)
{
    return message.startsWith(QStringLiteral("正在"))
        || message.startsWith(QStringLiteral("TCP 连接"))
        || message.startsWith(QStringLiteral("等待确认"));
}
} // namespace

TerminalPanel::TerminalPanel(SshSession *session, ServerRepository *repository, QWidget *parent)
    : QFrame(parent)
    , m_session(session)
{
    setStyleSheet(QStringLiteral("TerminalPanel{border:1px solid #203349;border-radius:5px;background:#0C1825;}"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_output = new TerminalView;
    auto *outputContainer = new QWidget;
    outputContainer->setObjectName(QStringLiteral("terminalOutputContainer"));
    auto *outputStack = new QStackedLayout(outputContainer);
    // Keep the terminal canvas away from the panel edge without consuming
    // space from the tab bar, command input, or connection status rows.
    outputStack->setContentsMargins(0, 0, 0, 0);
    outputStack->setStackingMode(QStackedLayout::StackAll);
    outputStack->addWidget(m_output);

    m_loadingOverlay = new QWidget;
    m_loadingOverlay->setObjectName(QStringLiteral("terminalLoadingOverlay"));
    auto *overlayLayout = new QVBoxLayout(m_loadingOverlay);
    overlayLayout->setContentsMargins(24, 24, 24, 24);
    overlayLayout->addStretch();
    auto *centerRow = new QHBoxLayout;
    centerRow->addStretch();
    auto *loadingCard = new QFrame;
    loadingCard->setObjectName(QStringLiteral("terminalLoadingCard"));
    loadingCard->setMinimumWidth(330);
    loadingCard->setMaximumWidth(440);
    auto *loadingLayout = new QVBoxLayout(loadingCard);
    loadingLayout->setContentsMargins(22, 17, 22, 17);
    loadingLayout->setSpacing(9);
    auto *loadingTitle = new QLabel(QStringLiteral("正在建立 SSH 连接"));
    loadingTitle->setObjectName(QStringLiteral("terminalLoadingTitle"));
    m_loadingDetail = new QLabel(QStringLiteral("准备连接…"));
    m_loadingDetail->setObjectName(QStringLiteral("terminalLoadingDetail"));
    m_loadingDetail->setAlignment(Qt::AlignCenter);
    m_loadingDetail->setWordWrap(true);
    m_loadingDetail->setTextFormat(Qt::PlainText);
    auto *loadingProgress = new QProgressBar;
    loadingProgress->setObjectName(QStringLiteral("terminalLoadingProgress"));
    loadingProgress->setRange(0, 0);
    loadingProgress->setTextVisible(false);
    loadingProgress->setFixedHeight(4);
    loadingLayout->addWidget(loadingTitle, 0, Qt::AlignCenter);
    loadingLayout->addWidget(m_loadingDetail);
    loadingLayout->addWidget(loadingProgress);
    m_passwordForm = new QWidget;
    auto *passwordLayout = new QVBoxLayout(m_passwordForm);
    passwordLayout->setContentsMargins(0, 0, 0, 0);
    m_connectionPassword = new QLineEdit;
    m_connectionPassword->setObjectName(QStringLiteral("terminalConnectionPassword"));
    m_connectionPassword->setEchoMode(QLineEdit::Password);
    m_rememberPassword = new QCheckBox(QStringLiteral(
#ifdef Q_OS_MACOS
        "记住密码（本地加密，升级保留）"
#else
        "记住密码（系统加密保存）"
#endif
    ));
    m_rememberPassword->setObjectName(QStringLiteral("terminalRememberPassword"));
    m_rememberPassword->setChecked(true);
    m_passwordConnectButton = new QPushButton(QStringLiteral("连接"));
    m_passwordConnectButton->setObjectName(QStringLiteral("terminalPasswordConnectButton"));
    passwordLayout->addWidget(m_connectionPassword);
    auto *passwordInputHint = new QLabel;
    configureAsciiCredentialInput(m_connectionPassword, passwordInputHint);
    passwordLayout->addWidget(passwordInputHint);
    passwordLayout->addWidget(m_rememberPassword);
    passwordLayout->addWidget(m_passwordConnectButton);
    m_passwordForm->hide();
    loadingLayout->addWidget(m_passwordForm);
    connect(m_passwordConnectButton, &QPushButton::clicked, this, [this] {
        if (m_session->profile().authentication == AuthenticationMethod::Password
            && m_connectionPassword->text().isEmpty()) {
            m_connectionPassword->setFocus();
            return;
        }
        const auto password = m_connectionPassword->text();
        m_connectionPassword->clear();
        m_session->connectWithPassword(password, m_rememberPassword->isChecked());
    });
    connect(m_connectionPassword, &QLineEdit::returnPressed, m_passwordConnectButton, &QPushButton::click);
    connect(m_session, &SshSession::passwordRequired, this,
        [this, loadingTitle, loadingProgress](const QString &reason) {
            const auto profile = m_session->profile();
            const bool privateKey = profile.authentication == AuthenticationMethod::PrivateKey;
            loadingTitle->setText(privateKey ? QStringLiteral("输入 SSH 私钥口令") : QStringLiteral("输入 SSH 密码"));
            m_loadingDetail->setText(QStringLiteral("%1@%2:%3\n%4")
                .arg(profile.user, profile.host).arg(profile.port).arg(reason));
            m_connectionPassword->setPlaceholderText(privateKey ? QStringLiteral("私钥口令（无口令可留空）") : QStringLiteral("服务器的 SSH 密码"));
            loadingProgress->hide();
            m_passwordForm->show();
            m_loadingOverlay->show();
            m_loadingOverlay->raise();
            m_connectionPassword->setFocus();
        });
    connect(m_session, &SshSession::connectionChanged, this,
        [this, loadingTitle, loadingProgress](bool, const QString &) {
            m_passwordForm->hide();
            m_connectionPassword->clear();
            loadingTitle->setText(QStringLiteral("正在建立 SSH 连接"));
            loadingProgress->show();
        });
    centerRow->addWidget(loadingCard);
    centerRow->addStretch();
    overlayLayout->addLayout(centerRow);
    overlayLayout->addStretch();
    outputStack->addWidget(m_loadingOverlay);
    m_loadingOverlay->hide();

    m_commandHistory = new CommandHistoryPanel(repository);
    m_commandHistory->hide();
    auto *closeTransientShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    closeTransientShortcut->setObjectName(QStringLiteral("terminalCloseTransientShortcut"));
    closeTransientShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    closeTransientShortcut->setEnabled(false);

    auto *inputRow = new QWidget;
    inputRow->setObjectName(QStringLiteral("terminalInputRow"));
    auto *inputLayout = new QHBoxLayout(inputRow);
    inputLayout->setContentsMargins(0, 0, 5, 0);
    inputLayout->setSpacing(3);
    m_input = new QLineEdit;
    m_input->setObjectName(QStringLiteral("terminalInput"));
    m_input->setPlaceholderText(QStringLiteral("输入命令并按 Enter（输入 help 查看原型命令）"));
    m_input->setEnabled(false);
    m_output->setEnabled(false);
    m_input->installEventFilter(this);
    m_historyButton = new QToolButton;
    m_historyButton->setObjectName(QStringLiteral("commandHistoryButton"));
    m_historyButton->setIcon(QIcon(QStringLiteral(":/assets/command-history.svg")));
    m_historyButton->setIconSize(QSize(18, 18));
    m_historyButton->setFixedSize(29, 29);
    m_historyButton->setCheckable(true);
    m_historyButton->setToolTip(QStringLiteral("命令历史与收藏"));
    m_fileWorkspaceButton = new QToolButton;
    m_fileWorkspaceButton->setObjectName(QStringLiteral("fileWorkspaceToggleButton"));
    m_fileWorkspaceButton->setIcon(QIcon(QStringLiteral(":/assets/file-panel-hide.svg")));
    m_fileWorkspaceButton->setIconSize(QSize(18, 18));
    m_fileWorkspaceButton->setFixedSize(29, 29);
    m_fileWorkspaceButton->setToolTip(QStringLiteral("隐藏文件管理"));
    inputLayout->addWidget(m_input, 1);
    inputLayout->addWidget(m_historyButton);
    inputLayout->addWidget(m_fileWorkspaceButton);

    m_input->setFixedHeight(35);
    inputRow->setFixedHeight(35);
    layout->addWidget(outputContainer, 1);
    layout->addWidget(m_commandHistory);
    layout->addWidget(inputRow);

    connect(m_input, &QLineEdit::textEdited, m_output, &TerminalView::clearSelection);
    connect(m_input, &QLineEdit::returnPressed, this, &TerminalPanel::submitCommand);
    connect(m_session, &SshSession::outputReceived, this, [this](const QString &text) {
        if (m_session->profile().connectionMode == ConnectionMode::Demo || text.startsWith(QStringLiteral("TCP失败"))
            || text.startsWith(QStringLiteral("SSH")) || text == QStringLiteral("__CLEAR__")) {
            appendOutput(text);
        }
    });
    connect(m_session, &SshSession::rawOutputReceived, m_output, &TerminalView::feedData);
    connect(m_output, &TerminalView::inputGenerated, m_session, &SshSession::sendInput);
    connect(m_output, &TerminalView::commandSubmitted, this, [this](const QString &command) {
        rememberCommand(command);
        emit commandSubmitted(command);
    });
    connect(m_historyButton, &QToolButton::toggled, this, [this](bool visible) {
        if (visible) m_commandHistory->refresh();
        m_commandHistory->setVisible(visible);
        m_historyButton->setToolTip(visible ? QStringLiteral("收起命令历史")
                                            : QStringLiteral("命令历史与收藏"));
    });
    connect(m_historyButton, &QToolButton::toggled,
        closeTransientShortcut, &QShortcut::setEnabled);
    connect(closeTransientShortcut, &QShortcut::activated, this, [this] {
        if (m_commandHistory && m_commandHistory->isVisible()) m_historyButton->setChecked(false);
    });
    connect(m_commandHistory, &CommandHistoryPanel::closeRequested, this, [this] {
        m_historyButton->setChecked(false);
    });
    connect(m_commandHistory, &CommandHistoryPanel::commandSelected, this, [this](const QString &command) {
        // Selecting from history is an editing action, not execution. Replace
        // any draft already in the input, then close the history drawer so the
        // user can review or modify the selected command before pressing Enter.
        m_input->setText(command);
        m_input->setFocus();
        m_input->setCursorPosition(m_input->text().size());
        m_historyButton->setChecked(false);
    });
    connect(m_commandHistory, &CommandHistoryPanel::commandExecutionRequested, this, [this](const QString &command) {
        if (!m_session || !m_session->isConnected()) return;
        m_input->setText(command);
        submitCommand();
    });
    connect(m_fileWorkspaceButton, &QToolButton::clicked, this, &TerminalPanel::fileWorkspaceToggleRequested);
    connect(m_output, &TerminalView::terminalSizeChanged, this,
        [this](int columns, int rows, int pixelWidth, int pixelHeight) {
            m_session->resizeTerminal(columns, rows, pixelWidth, pixelHeight);
        });
    connect(m_session, &SshSession::promptChanged, this, [this](const QString &prompt) {
        m_prompt = prompt;
        m_input->setPlaceholderText(prompt);
        m_input->setFocus();
    });
    connect(m_session, &SshSession::connectionChanged, this, [this](bool connected, const QString &message) {
        m_input->setEnabled(connected);
        m_output->setEnabled(connected);
        if (connected) {
            hideConnectionLoading();
            m_session->resizeTerminal(m_output->columns(), m_output->rows(), m_output->width(), m_output->height());
            m_output->setFocus();
        } else if (isConnectionProgress(message)) {
            showConnectionLoading(message);
        } else {
            hideConnectionLoading();
        }
    });
}

void TerminalPanel::setServer(const ServerProfile &profile)
{
    m_passwordForm->hide();
    m_connectionPassword->clear();
    m_profile = profile;
    m_commandHistory->setServerId(profile.id);
    m_output->clear();
    m_prompt.clear();
    m_input->clear();
    m_input->setEnabled(false);
    m_output->setEnabled(false);
    hideConnectionLoading();
}

void TerminalPanel::setFileWorkspaceVisible(bool visible)
{
    if (!m_fileWorkspaceButton) return;
    m_fileWorkspaceButton->setIcon(QIcon(visible ? QStringLiteral(":/assets/file-panel-hide.svg")
                                                   : QStringLiteral(":/assets/file-panel-show.svg")));
    m_fileWorkspaceButton->setToolTip(visible ? QStringLiteral("隐藏文件管理")
                                              : QStringLiteral("显示文件管理"));
}

void TerminalPanel::connectToServer(const ServerProfile &profile)
{
    if ((m_session->isConnected() || m_session->isConnecting()) && m_session->profile().id == profile.id) {
        m_output->setFocus();
        return;
    }
    setServer(profile);
    showConnectionLoading(QStringLiteral("正在连接 %1@%2:%3…").arg(profile.user, profile.host).arg(profile.port));
    m_session->connectTo(profile);
}

void TerminalPanel::updateServer(const ServerProfile &profile)
{
    // Connected sessions retain the profile snapshot used for their SSH
    // handshake. A saved host edit applies only to a newly opened session.
    if (!m_session->isConnected()) setServer(profile);
}

bool TerminalPanel::isConnected() const
{
    return m_session && m_session->isConnected();
}

bool TerminalPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_input
        && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const bool interruptShortcut = keyEvent->key() == Qt::Key_C
            && (keyEvent->modifiers().testFlag(Qt::ControlModifier)
                || keyEvent->modifiers().testFlag(Qt::MetaModifier));
        if (interruptShortcut) {
            if (event->type() == QEvent::ShortcutOverride) {
                keyEvent->accept();
                return true;
            }
            if (m_input->hasSelectedText()) {
                m_input->copy();
            } else if (m_session && m_session->isConnected()) {
                m_input->clear();
                m_output->clearSelection();
                m_session->sendInput(QByteArray(1, '\x03'));
                m_output->setFocus();
            }
            keyEvent->accept();
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void TerminalPanel::clearTerminal()
{
    if (!m_output) return;
    m_output->clear();
    if (m_session && m_session->isConnected()) {
        // 与真实终端 Ctrl+L 一致：本地清除滚屏历史，再由远端 Shell 重绘提示符。
        m_session->sendInput(QByteArray(1, '\x0c'));
    }
}

void TerminalPanel::showConnectionLoading(const QString &message)
{
    if (!m_loadingOverlay || !m_loadingDetail) return;
    m_loadingDetail->setText(message);
    m_loadingOverlay->show();
    m_loadingOverlay->raise();
}

void TerminalPanel::hideConnectionLoading()
{
    if (m_loadingOverlay) m_loadingOverlay->hide();
}

void TerminalPanel::submitCommand()
{
    const auto command = m_input->text();
    m_output->clearSelection();
    m_input->clear();
    rememberCommand(command);
    emit commandSubmitted(command);
    if (m_session->profile().connectionMode == ConnectionMode::Demo) {
        m_output->feedText(m_prompt + command + QLatin1Char('\n'));
    }
    m_session->execute(command);
    // Interactive commands such as top/vim keep running in the PTY. Give the
    // raw terminal focus so Ctrl/Cmd+C and subsequent keys reach that process.
    m_output->setFocus();
}

void TerminalPanel::rememberCommand(const QString &command)
{
    if (m_commandHistory) m_commandHistory->recordCommand(command);
}

void TerminalPanel::appendOutput(const QString &text)
{
    if (text == QStringLiteral("__CLEAR__")) {
        m_output->clear();
        return;
    }
    m_output->feedText(text);
}

} // namespace noxshell::ui
