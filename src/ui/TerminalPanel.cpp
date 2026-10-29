#include "TerminalPanel.h"
#include "../core/AppSettings.h"
#include <QCompleter>
#include <QStringListModel>
#include <QAbstractItemView>
#include "AppTheme.h"
#include "ServerToolsDialog.h"
#include "SoftwareInstallDialog.h"
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
#include <QShowEvent>
#include <QVBoxLayout>

namespace noxshell::ui {

namespace {
class HistoryCompleter final : public QCompleter {
public:
    using QCompleter::QCompleter;
protected:
    bool eventFilter(QObject *object,QEvent *event) override {
        if(event->type()==QEvent::KeyPress && popup()->isVisible()) {
            auto *key=static_cast<QKeyEvent*>(event);
            const bool tab=AppSettings::value("terminal/completionAccept")=="Tab";
            if(tab && (key->key()==Qt::Key_Return || key->key()==Qt::Key_Enter)) {
                popup()->hide();return false;
            }
            if(tab && key->key()==Qt::Key_Tab) {
                if(!popup()->currentIndex().isValid())popup()->setCurrentIndex(completionModel()->index(0,0));
                QKeyEvent accept(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);
                return QCompleter::eventFilter(object,&accept);
            }
            const bool ctrl=AppSettings::value("terminal/completionCycle")=="ctrl";
            if(ctrl && key->modifiers()==Qt::ControlModifier && (key->key()==Qt::Key_N || key->key()==Qt::Key_P)) {
                QKeyEvent cycle(QEvent::KeyPress,key->key()==Qt::Key_N?Qt::Key_Down:Qt::Key_Up,Qt::NoModifier);
                return QCompleter::eventFilter(object,&cycle);
            }
            if(ctrl && (key->key()==Qt::Key_Up || key->key()==Qt::Key_Down))return true;
        }
        return QCompleter::eventFilter(object,event);
    }
};
bool isConnectionProgress(const QString &message)
{
    return message.startsWith(QStringLiteral("正在"))
        || message.startsWith(QStringLiteral("TCP 连接"))
        || message.startsWith(QStringLiteral("等待确认"));
}
} // namespace

TerminalPanel::TerminalPanel(SshSession *session, ServerRepository *repository, QWidget *parent)
    : QFrame(parent)
    , m_repository(repository)
    , m_session(session)
{
    setObjectName(QStringLiteral("terminalPanel"));

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
    m_input->setAccessibleName(QStringLiteral("命令输入"));
    m_input->setEnabled(false);
    m_output->setEnabled(false);
    m_input->installEventFilter(this);
    m_completionModel=new QStringListModel(this);
    m_completer=new HistoryCompleter(m_completionModel,this);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setMaxVisibleItems(8);
    connect(AppSettings::instance(),&AppSettings::changed,this,[this](const QString &key){
        if(key.startsWith("terminal/"))refreshCompletions();
    });
    refreshCompletions();
    m_historyButton = new QToolButton;
    m_historyButton->setObjectName(QStringLiteral("commandHistoryButton"));
    m_historyButton->setProperty("terminalFooterButton", true);
    m_historyButton->setProperty("terminalIconResource", QStringLiteral(":/assets/command-history.svg"));
    m_historyButton->setIcon(terminalToolIcon(QStringLiteral(":/assets/command-history.svg")));
    m_historyButton->setIconSize(QSize(18, 18));
    m_historyButton->setFixedSize(29, 29);
    m_historyButton->setCheckable(true);
    m_historyButton->setToolTip(QStringLiteral("命令历史与收藏"));
    m_fileWorkspaceButton = new QToolButton;
    m_fileWorkspaceButton->setObjectName(QStringLiteral("fileWorkspaceToggleButton"));
    m_fileWorkspaceButton->setProperty("terminalFooterButton", true);
    m_fileWorkspaceButton->setProperty("terminalIconResource", QStringLiteral(":/assets/file-panel-hide.svg"));
    m_fileWorkspaceButton->setIcon(terminalToolIcon(QStringLiteral(":/assets/file-panel-hide.svg")));
    m_fileWorkspaceButton->setIconSize(QSize(18, 18));
    m_fileWorkspaceButton->setFixedSize(29, 29);
    m_fileWorkspaceButton->setToolTip(QStringLiteral("隐藏文件管理"));
    inputLayout->addWidget(m_input, 1);
    const QStringList toolNames{QStringLiteral("进程管理"), QStringLiteral("网速测试"), QStringLiteral("流量观测")};
    const QStringList toolIcons{"process-management", "speed-test", "traffic-observation"};
    for (int index = 0; index < toolNames.size(); ++index) {
        auto *button = new QToolButton;
        button->setObjectName(QStringLiteral("serverToolButton%1").arg(index));
        button->setProperty("terminalFooterButton", true);
        button->setToolTip(toolNames[index]); button->setAccessibleName(toolNames[index]);
        button->setToolTipDuration(4000);
        const auto iconResource = QStringLiteral(":/assets/%1.svg").arg(toolIcons[index]);
        button->setProperty("terminalIconResource", iconResource);
        button->setIcon(terminalToolIcon(iconResource));
        button->setIconSize(QSize(18, 18)); button->setFixedSize(29, 29);
        inputLayout->addWidget(button);
        connect(button, &QToolButton::clicked, this, [this, index] {
            if (!m_tools) m_tools = new ServerToolsDialog(m_session, this);
            m_tools->showPage(index);
        });
    }
    auto *software = new QToolButton;
    software->setObjectName("softwareInstallButton");software->setProperty("terminalFooterButton",true);
    software->setProperty("terminalIconResource",QStringLiteral(":/assets/software-install.svg"));
    software->setIcon(terminalToolIcon(QStringLiteral(":/assets/software-install.svg")));
    software->setIconSize(QSize(18,18));software->setFixedSize(29,29);
    software->setToolTip(QStringLiteral("软件安装"));software->setAccessibleName(QStringLiteral("软件安装"));
    software->setToolTipDuration(4000);inputLayout->addWidget(software);
    connect(software,&QToolButton::clicked,this,[this]{
        if(m_software&&!m_software->belongsToCurrentProfile()){m_software->hide();m_software->deleteLater();m_software=nullptr;}
        if(!m_software)m_software=new SoftwareInstallDialog(m_session,this);
        m_software->showManager();
    });
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
    connect(m_output, &TerminalView::fullScreenModeChanged, m_session, &SshSession::setTerminalFullScreen);
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
        // Keep the input hint generic; the active tab already names the host.
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
    refreshCompletions();
    m_output->clear(true);
    m_prompt.clear();
    m_input->clear();
    m_input->setEnabled(false);
    m_output->setEnabled(false);
    hideConnectionLoading();
}

void TerminalPanel::changeEvent(QEvent *event)
{
    QFrame::changeEvent(event);
    if (event->type() != QEvent::PaletteChange && event->type() != QEvent::StyleChange
        && event->type() != QEvent::ApplicationPaletteChange) return;
    for (auto *button : findChildren<QToolButton *>()) {
        const auto resource = button->property("terminalIconResource").toString();
        if (!resource.isEmpty()) button->setIcon(terminalToolIcon(resource));
    }
}

void TerminalPanel::setFileWorkspaceVisible(bool visible)
{
    if (!m_fileWorkspaceButton) return;
    const auto resource = visible ? QStringLiteral(":/assets/file-panel-hide.svg")
                                  : QStringLiteral(":/assets/file-panel-show.svg");
    m_fileWorkspaceButton->setProperty("terminalIconResource", resource);
    m_fileWorkspaceButton->setIcon(terminalToolIcon(resource));
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

void TerminalPanel::showEvent(QShowEvent *event)
{
    // Tool dialogs are nonmodal: keep footer names available while they have
    // focus. Qt requires this attribute on the top-level window, not a button.
    window()->setAttribute(Qt::WA_AlwaysShowToolTips);
    QFrame::showEvent(event);
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
    refreshCompletions();
}

void TerminalPanel::refreshCompletions()
{
    if(!m_completer)return;
    QStringList commands;
    if(m_repository)for(const auto &entry:m_repository->loadCommandHistory(false,500))
        if(entry.serverId==m_profile.id && !entry.command.trimmed().isEmpty() && !commands.contains(entry.command))commands.append(entry.command);
    if(!AppSettings::value("terminal/prediction").toBool())commands.sort(Qt::CaseInsensitive);
    m_completionModel->setStringList(commands);
    m_completer->setWrapAround(AppSettings::value("terminal/completionWrap").toBool());
    m_input->setCompleter(AppSettings::value("terminal/autoComplete").toBool()?m_completer:nullptr);
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
