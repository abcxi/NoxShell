#pragma once

#include "../core/ServerProfile.h"

#include <QFrame>
#include <QPointer>

class QCompleter;
class QStringListModel;
class QLabel;
class QLineEdit;
class QToolButton;
class QPushButton;
class QCheckBox;
class QShowEvent;

namespace noxshell {
class SshSession;
class ServerRepository;
}

namespace noxshell::ui {

class TerminalView;
class CommandHistoryPanel;
class ServerToolsDialog;
class SoftwareInstallDialog;

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
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void submitCommand();
    void appendOutput(const QString &text);
    void showConnectionLoading(const QString &message);
    void hideConnectionLoading();
    void rememberCommand(const QString &command);
    void refreshCompletions();
    ServerRepository *m_repository{};
    QCompleter *m_completer{};
    QStringListModel *m_completionModel{};

    SshSession *m_session{};
    QWidget *m_loadingOverlay{};
    QLabel *m_loadingDetail{};
    QWidget *m_passwordForm{};
    QLineEdit *m_connectionPassword{};
    QCheckBox *m_rememberPassword{};
    QPushButton *m_passwordConnectButton{};
    CommandHistoryPanel *m_commandHistory{};
    TerminalView *m_output{};
    QLineEdit *m_input{};
    QToolButton *m_historyButton{};
    QToolButton *m_fileWorkspaceButton{};
    QString m_prompt;
    QPointer<ServerToolsDialog> m_tools;
    QPointer<SoftwareInstallDialog> m_software;
    ServerProfile m_profile;
};

} // namespace noxshell::ui
