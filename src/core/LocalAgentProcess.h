#pragma once
#include <QObject>
#include <QByteArray>
#include <QProcessEnvironment>
class QTimer;
#ifdef Q_OS_WIN
class QProcess;
#endif
namespace noxshell {
// A local PTY; never writes CLI commands to an SSH terminal.
class LocalAgentProcess final : public QObject {
    Q_OBJECT
public:
    explicit LocalAgentProcess(QObject *parent=nullptr);
    ~LocalAgentProcess() override;
    static QStringList searchPaths();
    static QString findCli(const QString &name);
    // Explicit roots keep discovery bounded and independently testable without
    // touching installed applications, shell profiles or CLI credentials.
    static QString findCli(const QString &name, const QStringList &paths,
                           const QStringList &applicationDirectories);
    bool start(const QString &executable,const QStringList &arguments,const QString &directory);
    void stop();
    void write(const QByteArray &bytes);
    void resize(int columns,int rows);
    bool isRunning() const {return m_pid>0;}
    QString lastError() const {return m_error;}
    qint64 processId() const {return m_pid;}
signals:
    void outputReceived(const QByteArray &bytes);
    void finished(int exitCode);
private:
    void poll();void drain();
    qint64 m_pid{};
    int m_descriptor{-1},m_columns{100},m_rows{30};
    QByteArray m_pending;
    QTimer *m_timer{};
    QString m_error;
#ifdef Q_OS_WIN
    QProcess *m_process{};
#endif
};
}
