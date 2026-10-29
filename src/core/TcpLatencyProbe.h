#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QNetworkProxy>
#include <QTimer>

class QTcpSocket;

namespace noxshell {

// One controller per top-level tab, with at most one non-blocking attempt.
class TcpLatencyProbe final : public QObject {
    Q_OBJECT
public:
    explicit TcpLatencyProbe(QObject *parent = nullptr);
    ~TcpLatencyProbe() override;
    void setTarget(const QString &host, quint16 port, const QNetworkProxy &proxy);
    void setActive(bool active);
    bool isActive() const { return m_active; }
    bool isRunning() const { return m_socket != nullptr; }
    QString text() const { return m_text; }

signals:
    void updated(const QString &text);
    void probeStarted();

private:
    void startProbe();
    void cancelProbe();
    void finishProbe(bool success);
    void setText(const QString &text);
    QTimer m_refresh, m_timeout;
    QElapsedTimer m_elapsed;
    QTcpSocket *m_socket{};
    quint64 m_generation{};
    QString m_host, m_text{QStringLiteral("—")};
    quint16 m_port{};
    QNetworkProxy m_proxy{QNetworkProxy::NoProxy};
    bool m_active{};
};

} // namespace noxshell
