#pragma once
#include <QWidget>
#include <QJsonArray>
#include <QPointer>
class QComboBox; class QPushButton; class QLabel; class QLineEdit; class QTextBrowser; class QLocalServer; class QLocalSocket;
class QNetworkAccessManager; class QNetworkReply; class QStackedWidget; class QBoxLayout;
namespace noxshell { class SshSession; class ServerRepository; class CredentialStore; class LocalAgentProcess; }
namespace noxshell::ui {
class TerminalView;
class AgentPanel final : public QWidget {
    Q_OBJECT
public:
    explicit AgentPanel(SshSession *shell, ServerRepository *repository, CredentialStore *credentials, QWidget *parent=nullptr);
    ~AgentPanel() override;
    [[nodiscard]] LocalAgentProcess *localProcess() const {return m_process;}
    [[nodiscard]] bool taskRunning() const {return m_running;}
    [[nodiscard]] QString workspaceDirectory() const {return m_workspace;}
signals:
    void statusChanged(const QString &status);
protected:
    void resizeEvent(QResizeEvent *event) override;
private:
    void launch(bool resume); void sendQuestion(); void setStatus(const QString &text);
    bool prepareBridge(); void handleBridgeRequest(QLocalSocket *socket);
    QPointer<SshSession> m_shell;
    CredentialStore *m_credentials{};
    TerminalView *m_terminal{};
    QComboBox *m_selector{};
    QLabel *m_status{};
    QPushButton *m_start{}, *m_resume{}, *m_stop{}, *m_send{};
    QLineEdit *m_question{};
    QTextBrowser *m_chat{};
    QStackedWidget *m_body{};
    QBoxLayout *m_toolbarLayout{};
    LocalAgentProcess *m_process{};
    QLocalServer *m_bridge{};
    QNetworkAccessManager *m_network{};
    QPointer<QNetworkReply> m_reply;
    QJsonArray m_messages;
    QStringList m_cliArguments;
    QString m_nonce, m_workspace, m_bridgeFile, m_bridgeSocket, m_bridgeToken, m_serverId;
    quint64 m_pendingBridgeRequest{};
    QPointer<QLocalSocket> m_bridgeClient;
    bool m_confirmingBridge{};
    bool m_running{},m_hasRun{};
};
}
