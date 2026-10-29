#include "TcpLatencyProbe.h"
#include <QTcpSocket>

namespace noxshell {

TcpLatencyProbe::TcpLatencyProbe(QObject *parent) : QObject(parent)
{
    setObjectName(QStringLiteral("tabLatencyProbe"));
    m_refresh.setInterval(10000);
    m_timeout.setInterval(4000);
    m_timeout.setSingleShot(true);
    connect(&m_refresh, &QTimer::timeout, this, &TcpLatencyProbe::startProbe);
    connect(&m_timeout, &QTimer::timeout, this, [this] { finishProbe(false); });
}

TcpLatencyProbe::~TcpLatencyProbe()
{
    m_refresh.stop();
    cancelProbe();
}

void TcpLatencyProbe::setTarget(const QString &host, quint16 port, const QNetworkProxy &proxy)
{
    if (m_host == host && m_port == port && m_proxy == proxy) return;
    cancelProbe();
    m_host = host; m_port = port; m_proxy = proxy;
    setText(QStringLiteral("—"));
    if (m_active) startProbe();
}

void TcpLatencyProbe::setActive(bool active)
{
    if (m_active == active) return;
    m_active = active;
    if (active) {
        m_refresh.start();
        startProbe();
    } else {
        m_refresh.stop();
        cancelProbe();
    }
}

void TcpLatencyProbe::setText(const QString &text)
{
    if (m_text == text) return;
    m_text = text;
    emit updated(text);
}

void TcpLatencyProbe::cancelProbe()
{
    ++m_generation;
    m_timeout.stop();
    if (auto *socket = m_socket) {
        m_socket = nullptr;
        socket->abort();
        socket->deleteLater();
    }
}

void TcpLatencyProbe::startProbe()
{
    if (!m_active || m_socket || m_host.isEmpty() || !m_port) return;
    auto *socket = new QTcpSocket(this);
    m_socket = socket;
    socket->setObjectName(QStringLiteral("latencyProbeSocket"));
    socket->setProxy(m_proxy);
    const auto generation = ++m_generation;
    connect(socket, &QTcpSocket::connected, this, [this, generation] {
        if (generation == m_generation) finishProbe(true);
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [this, generation] {
        if (generation == m_generation) finishProbe(false);
    });
    setText(QStringLiteral("…"));
    if (!m_active || generation != m_generation) return;
    m_elapsed.start();
    m_timeout.start();
    emit probeStarted();
    if (!m_active || generation != m_generation) return;
    socket->connectToHost(m_host, m_port);
}

void TcpLatencyProbe::finishProbe(bool success)
{
    if (!m_socket) return;
    const auto reading = success ? QStringLiteral("%1ms").arg(m_elapsed.elapsed()) : QStringLiteral("—");
    cancelProbe();
    setText(reading);
}

} // namespace noxshell
