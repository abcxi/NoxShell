#pragma once

#include "ServerProfile.h"

#include <QObject>
#include <QStringList>

namespace noxshell {

class MockSshSession final : public QObject {
    Q_OBJECT

public:
    explicit MockSshSession(QObject *parent = nullptr);

    void connectTo(const ServerProfile &profile);
    void disconnectFromHost();
    void execute(const QString &command);

    [[nodiscard]] bool isConnected() const { return m_connected; }
    [[nodiscard]] ServerProfile profile() const { return m_profile; }

signals:
    void connectionChanged(bool connected, const QString &message);
    void outputReceived(const QString &text);
    void promptChanged(const QString &prompt);

private:
    QString responseFor(const QString &command) const;

    ServerProfile m_profile;
    bool m_connected{false};
};

} // namespace noxshell

