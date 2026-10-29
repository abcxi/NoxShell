#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QPointer>
class QLabel; class QLineEdit; class QTreeWidget; class QPlainTextEdit; class QPushButton; class QTimer;
namespace noxshell { class SshSession; }
namespace noxshell::ui {
class SoftwareInstallDialog final : public QDialog {
    Q_OBJECT
public:
    explicit SoftwareInstallDialog(SshSession *session, QWidget *parent=nullptr);
    ~SoftwareInstallDialog() override;
    void showManager();
    [[nodiscard]] bool belongsToCurrentProfile() const;
protected:
    void hideEvent(QHideEvent *event) override;
private:
    bool connectedToTarget() const;
    bool request(QJsonObject options);
    void search(); void poll(); void installSelected(); void forgetJob(); void controls();
    void finish(quint64 id, const QByteArray &output, const QString &error);
    void saveJob();
    QPointer<SshSession> m_session;
    QStringList m_target, m_selected;
    QString m_job, m_jobKey, m_manager, m_operation;
    quint64 m_request{};
    bool m_root{}, m_jobActive{}, m_jobUnknown{}, m_ignoreReply{};
    QLabel *m_environment{}, *m_status{};
    QLineEdit *m_query{};
    QTreeWidget *m_packages{};
    QPlainTextEdit *m_log{};
    QPushButton *m_search{}, *m_clear{}, *m_install{}, *m_refresh{}, *m_forget{};
    QTimer *m_timer{};
};
}
