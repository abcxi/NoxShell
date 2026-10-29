#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QPointer>
class QTabWidget;
class QLabel;
class QTreeWidget;
class QComboBox;
class QSpinBox;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QTimer;
namespace noxshell { class SshSession; }
namespace noxshell::ui {
class ServerToolsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit ServerToolsDialog(SshSession *session, QWidget *parent = nullptr);
    ~ServerToolsDialog() override;
    void showPage(int page);
protected:
    void closeEvent(QCloseEvent *event) override;
    void hideEvent(QHideEvent *event) override;
private:
    void refresh();
    void request(QJsonObject options);
    void finish(quint64 id, const QByteArray &output, const QString &error);
    void processAction(const QString &action);
    void speedTest();
    void cancelTask();
    void controls();
    QPointer<SshSession> m_session;
    QTabWidget *m_tabs{};
    QLabel *m_status{};
    QTreeWidget *m_processes{}, *m_clients{};
    QComboBox *m_processSort{}, *m_trafficSort{}, *m_source{}, *m_direction{};
    QSpinBox *m_processLimit{}, *m_trafficLimit{}, *m_port{}, *m_mbps{};
    QLineEdit *m_target{};
    QPushButton *m_refresh{}, *m_cancel{}, *m_speed{};
    QList<QPushButton *> m_processActions;
    QCheckBox *m_auto{};
    QTimer *m_timer{};
    quint64 m_request{};
    QString m_operation;
    int m_requestPage{};
    bool m_safeSignals{}, m_refreshAfterCancel{}, m_cancelRequested{};
};
}
