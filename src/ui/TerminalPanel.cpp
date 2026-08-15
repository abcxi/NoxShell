#include "TerminalPanel.h"

#include "../core/SshSession.h"
#include "TerminalView.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QStackedLayout>
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

TerminalPanel::TerminalPanel(SshSession *session, QWidget *parent)
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
    auto *loadingProgress = new QProgressBar;
    loadingProgress->setObjectName(QStringLiteral("terminalLoadingProgress"));
    loadingProgress->setRange(0, 0);
    loadingProgress->setTextVisible(false);
    loadingProgress->setFixedHeight(4);
    loadingLayout->addWidget(loadingTitle, 0, Qt::AlignCenter);
    loadingLayout->addWidget(m_loadingDetail);
    loadingLayout->addWidget(loadingProgress);
    centerRow->addWidget(loadingCard);
    centerRow->addStretch();
    overlayLayout->addLayout(centerRow);
    overlayLayout->addStretch();
    outputStack->addWidget(m_loadingOverlay);
    m_loadingOverlay->hide();

    m_input = new QLineEdit;
    m_input->setObjectName(QStringLiteral("terminalInput"));
    m_input->setPlaceholderText(QStringLiteral("输入命令并按 Enter（输入 help 查看原型命令）"));
    m_input->setEnabled(false);
    m_output->setEnabled(false);
    m_input->installEventFilter(this);

    auto *status = new QWidget;
    status->setObjectName(QStringLiteral("terminalStatus"));
    auto *statusLayout = new QHBoxLayout(status);
    statusLayout->setContentsMargins(11, 0, 11, 0);
    m_connectionLabel = new QLabel(QStringLiteral("○ 待连接 · 双击左侧主机或右键连接"));
    m_sizeLabel = new QLabel(QStringLiteral("UTF-8   |   120 × 36"));
    statusLayout->addWidget(m_connectionLabel);
    statusLayout->addStretch();
    statusLayout->addWidget(m_sizeLabel);

    m_input->setFixedHeight(35);
    status->setFixedHeight(27);
    layout->addWidget(outputContainer, 1);
    layout->addWidget(m_input);
    layout->addWidget(status);

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
    connect(m_output, &TerminalView::commandSubmitted, this, &TerminalPanel::commandSubmitted);
    connect(m_output, &TerminalView::terminalSizeChanged, this,
        [this](int columns, int rows, int pixelWidth, int pixelHeight) {
            m_sizeLabel->setText(QStringLiteral("UTF-8   |   %1 × %2").arg(columns).arg(rows));
            m_session->resizeTerminal(columns, rows, pixelWidth, pixelHeight);
        });
    connect(m_session, &SshSession::promptChanged, this, [this](const QString &prompt) {
        m_prompt = prompt;
        m_input->setPlaceholderText(prompt);
        m_input->setFocus();
    });
    connect(m_session, &SshSession::connectionChanged, this, [this](bool connected, const QString &message) {
        m_connectionLabel->setText((connected ? QStringLiteral("● ") : QStringLiteral("○ ")) + message);
        m_connectionLabel->setStyleSheet(connected ? QStringLiteral("color:#53C99C;") : QStringLiteral("color:#C5D3DF;"));
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
    m_profile = profile;
    m_output->clear();
    m_prompt.clear();
    m_input->clear();
    m_input->setEnabled(false);
    m_output->setEnabled(false);
    m_connectionLabel->setText(QStringLiteral("○ 待连接 · 双击左侧主机或右键连接"));
    m_connectionLabel->setStyleSheet(QStringLiteral("color:#C5D3DF;"));
    hideConnectionLoading();
}

void TerminalPanel::connectToServer(const ServerProfile &profile)
{
    if (m_session->isConnected() && m_session->profile().id == profile.id) {
        m_output->setFocus();
        return;
    }
    setServer(profile);
    m_connectionLabel->setText(QStringLiteral("○ 正在建立 SSH 会话…"));
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
    emit commandSubmitted(command);
    if (m_session->profile().connectionMode == ConnectionMode::Demo) {
        m_output->feedText(m_prompt + command + QLatin1Char('\n'));
    }
    m_session->execute(command);
    // Interactive commands such as top/vim keep running in the PTY. Give the
    // raw terminal focus so Ctrl/Cmd+C and subsequent keys reach that process.
    m_output->setFocus();
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
