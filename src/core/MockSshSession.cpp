#include "MockSshSession.h"

#include <QDateTime>
#include <QTimer>

namespace noxshell {

MockSshSession::MockSshSession(QObject *parent)
    : QObject(parent)
{
}

void MockSshSession::connectTo(const ServerProfile &profile)
{
    m_profile = profile;
    m_connected = false;
    emit connectionChanged(false, QStringLiteral("正在连接 %1…").arg(profile.host));

    QTimer::singleShot(260, this, [this] {
        m_connected = true;
        emit outputReceived(QStringLiteral(
            "\x1b[90mLast login: %1 from 10.0.8.17\x1b[0m\n"
            "连接已建立；当前为本地演示会话，不会访问外部服务器。\n")
                                .arg(QDateTime::currentDateTime().toString(QStringLiteral("ddd MMM dd HH:mm:ss yyyy"))));
        emit connectionChanged(true, QStringLiteral("SSH 已连接 · 28 ms"));
        emit promptChanged(QStringLiteral("%1@%2:/var/www/app# ").arg(m_profile.user, m_profile.name));
    });
}

void MockSshSession::disconnectFromHost()
{
    if (!m_connected) {
        return;
    }
    m_connected = false;
    emit connectionChanged(false, QStringLiteral("SSH 已断开"));
}

void MockSshSession::execute(const QString &command)
{
    if (!m_connected) {
        emit outputReceived(QStringLiteral("会话未连接。\n"));
        return;
    }

    emit outputReceived(responseFor(command));
}

QString MockSshSession::responseFor(const QString &command) const
{
    const auto trimmed = command.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    if (trimmed == QStringLiteral("clear")) {
        return QStringLiteral("__CLEAR__");
    }
    if (trimmed == QStringLiteral("pwd")) {
        return QStringLiteral("/var/www/app\n");
    }
    if (trimmed == QStringLiteral("whoami")) {
        return m_profile.user + QLatin1Char('\n');
    }
    if (trimmed == QStringLiteral("uptime")) {
        return QStringLiteral("10:42:03 up 18 days,  2:41,  1 user,  load average: 1.26, 1.08, 0.94\n");
    }
    if (trimmed.startsWith(QStringLiteral("df"))) {
        return QStringLiteral(
            "Filesystem      Size  Used Avail Use% Mounted on\n"
            "/dev/vda1       200G  146G   54G  73% /\n");
    }
    if (trimmed.startsWith(QStringLiteral("systemctl status nginx"))) {
        return QStringLiteral(
            "● nginx.service - A high performance web server\n"
            "     Loaded: loaded (/lib/systemd/system/nginx.service; enabled)\n"
            "     Active: active (running) since Tue 07:59:41 CST\n"
            "   Main PID: 1842 (nginx)\n");
    }
    if (trimmed == QStringLiteral("help")) {
        return QStringLiteral("原型内置命令：pwd、whoami、uptime、df -h、systemctl status nginx、clear\n");
    }
    return QStringLiteral("bash: %1: 原型会话暂未接入远端执行，可输入 help 查看演示命令\n").arg(trimmed);
}

} // namespace noxshell
