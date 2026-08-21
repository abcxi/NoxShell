#pragma once

#include "../core/ServerProfile.h"

#include <QFrame>

class QLabel;
class QLineEdit;
class QToolButton;

namespace noxshell {
class SshSession;
class ServerRepository;
}

namespace noxshell::ui {

class TerminalView;
class CommandHistoryPanel;

class TerminalPanel final : public QFrame {
    Q_OBJECT

public:
    explicit TerminalPanel(SshSession *session, ServerRepository *repository, QWidget *parent = nullptr);

    void setServer(const ServerProfile &profile);
    void connectToServer(const ServerProfile &profile);
    void updateServer(const ServerProfile &profile);
    void clearTerminal();
    void setFileWorkspaceVisible(bool visible);
    [[nodiscard]] bool isConnected() const;

signals:
    void commandSubmitted(const QString &command);
    void fileWorkspaceToggleRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void submitCommand();
    void appendOutput(const QString &text);
    void showConnectionLoading(const QString &message);
    void hideConnectionLoading();
    void rememberCommand(const QString &command);

    SshSession *m_session{};
    QWidget *m_loadingOverlay{};
    QLabel *m_loadingDetail{};
    CommandHistoryPanel *m_commandHistory{};
    TerminalView *m_output{};
    QLineEdit *m_input{};
    QToolButton *m_historyButton{};
    QToolButton *m_fileWorkspaceButton{};
    QString m_prompt;
    ServerProfile m_profile;
};

} // namespace noxshell::ui
