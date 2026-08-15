#pragma once

#include "../core/ServerProfile.h"

#include <QFrame>

class QLabel;
class QLineEdit;

namespace noxshell {
class SshSession;
}

namespace noxshell::ui {

class TerminalView;

class TerminalPanel final : public QFrame {
    Q_OBJECT

public:
    explicit TerminalPanel(SshSession *session, QWidget *parent = nullptr);

    void setServer(const ServerProfile &profile);
    void connectToServer(const ServerProfile &profile);
    void updateServer(const ServerProfile &profile);
    void clearTerminal();
    [[nodiscard]] bool isConnected() const;

signals:
    void commandSubmitted(const QString &command);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void submitCommand();
    void appendOutput(const QString &text);
    void showConnectionLoading(const QString &message);
    void hideConnectionLoading();

    SshSession *m_session{};
    QLabel *m_connectionLabel{};
    QWidget *m_loadingOverlay{};
    QLabel *m_loadingDetail{};
    TerminalView *m_output{};
    QLineEdit *m_input{};
    QLabel *m_sizeLabel{};
    QString m_prompt;
    ServerProfile m_profile;
};

} // namespace noxshell::ui
