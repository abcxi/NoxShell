#include "LocalAgentProcess.h"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>
#include <QProcess>
#include <QFile>
#include <vector>
#include <cstring>
#ifndef Q_OS_WIN
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#ifdef Q_OS_MACOS
#include <util.h>
#else
#include <pty.h>
#endif
#endif
namespace noxshell {
LocalAgentProcess::LocalAgentProcess(QObject *parent):QObject(parent),m_timer(new QTimer(this))
{
    m_timer->setInterval(16);connect(m_timer,&QTimer::timeout,this,&LocalAgentProcess::poll);
}
LocalAgentProcess::~LocalAgentProcess(){stop();}
QStringList LocalAgentProcess::searchPaths()
{
    auto paths=QProcessEnvironment::systemEnvironment().value("PATH").split(QDir::listSeparator(),Qt::SkipEmptyParts);
    const auto home=QDir::homePath();
    paths+=QStringList{home+"/.local/bin",home+"/.npm-global/bin",home+"/.cargo/bin","/opt/homebrew/bin","/usr/local/bin","/usr/bin","/bin"};
    // GUI launches do not necessarily inherit nvm's PATH. Discover installed
    // versions without running login-shell scripts or changing CLI auth files.
    QDir versions(home+"/.nvm/versions/node");for(const auto &entry:versions.entryList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Name|QDir::Reversed))paths.append(versions.filePath(entry+"/bin"));
    paths.removeDuplicates();return paths;
}
QString LocalAgentProcess::findCli(const QString &name)
{
    QStringList applicationDirectories;
#ifdef Q_OS_MACOS
    applicationDirectories = {QDir::homePath()+"/Applications", "/Applications"};
#endif
    return findCli(name, searchPaths(), applicationDirectories);
}
QString LocalAgentProcess::findCli(const QString &name, const QStringList &paths,
                                  const QStringList &applicationDirectories)
{
    const QStringList allowed{"codebuddy","codex","claude","opencode","kimi","agent"};
    if(!allowed.contains(name))return {};
    // Keep an explicitly installed CLI ahead of desktop-bundled fallbacks.
    const auto installed = QStandardPaths::findExecutable(name, paths);
    if (!installed.isEmpty()) return installed;
#ifdef Q_OS_MACOS
    if (name == "codex") {
        const QStringList applications{"ChatGPT.app", "Codex.app"};
        const QStringList entries{
            "Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex",
            "Contents/Resources/codex-cli/bin/codex"};
        for (const auto &root : applicationDirectories) {
            if (!QDir::isAbsolutePath(root)) continue;
            for (const auto &application : applications) {
                for (const auto &entry : entries) {
                    const QFileInfo candidate(QDir(root).filePath(application+"/"+entry));
                    if (candidate.isFile() && candidate.isExecutable())
                        return candidate.absoluteFilePath();
                }
            }
        }
    }
#else
    Q_UNUSED(applicationDirectories);
#endif
    return {};
}
bool LocalAgentProcess::start(const QString &executable,const QStringList &arguments,const QString &directory)
{
    if(isRunning()){m_error="本机 Agent 已在运行";return false;}
    m_error.clear();m_pending.clear();
    if(!QFileInfo(executable).isExecutable()||!QFileInfo(directory).isDir()){m_error="本机 CLI 路径或工作目录无效";return false;}
    auto env=QProcessEnvironment::systemEnvironment();
    env.insert("PATH",searchPaths().join(QDir::listSeparator()));env.insert("TERM","xterm-256color");env.insert("COLORTERM","truecolor");
#ifdef Q_OS_WIN
    // Qt pipes are a streaming fallback on Windows; do not claim PTY support.
    m_process=new QProcess(this);m_process->setProcessEnvironment(env);m_process->setWorkingDirectory(directory);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process,&QProcess::readyReadStandardOutput,this,[this]{emit outputReceived(m_process->readAllStandardOutput());});
    connect(m_process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int code,QProcess::ExitStatus){drain();m_pid=0;emit finished(code);});
    m_process->start(executable,arguments);if(!m_process->waitForStarted(2000)){m_error=m_process->errorString();m_process->deleteLater();m_process=nullptr;return false;}m_pid=m_process->processId();return true;
#else
    // Allocate all Qt/C++ data before fork. The child only uses async-signal-safe
    // system calls between forkpty and execve (the UI already has worker threads).
    const auto program=QFile::encodeName(executable),cwd=QFile::encodeName(directory);
    QList<QByteArray> argvStorage{program};for(const auto &arg:arguments)argvStorage.append(arg.toUtf8());
    QList<QByteArray> envStorage;for(const auto &entry:env.toStringList())envStorage.append(entry.toUtf8());
    std::vector<char*> argv,envp;for(auto &s:argvStorage)argv.push_back(s.data());argv.push_back(nullptr);
    for(auto &s:envStorage)envp.push_back(s.data());envp.push_back(nullptr);
    const auto *programBytes=program.constData(),*cwdBytes=cwd.constData();auto **argumentBytes=argv.data(),**environmentBytes=envp.data();
    const long descriptorLimit=sysconf(_SC_OPEN_MAX);
    winsize dimensions{};dimensions.ws_col=m_columns;dimensions.ws_row=m_rows;
    int master=-1;const pid_t pid=forkpty(&master,nullptr,nullptr,&dimensions);
    if(pid==0){
        for(int fd=3;fd<descriptorLimit;++fd)::close(fd);
        if(chdir(cwdBytes)!=0)_exit(126);execve(programBytes,argumentBytes,environmentBytes);_exit(127);
    }
    if(pid<0){m_error=QString::fromLocal8Bit(strerror(errno));return false;}
    m_pid=pid;m_descriptor=master;fcntl(master,F_SETFD,FD_CLOEXEC);fcntl(master,F_SETFL,fcntl(master,F_GETFL,0)|O_NONBLOCK);
    m_timer->start();return true;
#endif
}
void LocalAgentProcess::drain()
{
#ifndef Q_OS_WIN
    if(m_descriptor<0)return;char buffer[8192];int total=0;
    while(total<262144){const auto count=::read(m_descriptor,buffer,sizeof(buffer));if(count>0){total+=count;emit outputReceived(QByteArray(buffer,count));}else if(count<0&&errno==EINTR)continue;else break;}
#else
    if(m_process)emit outputReceived(m_process->readAllStandardOutput());
#endif
}
void LocalAgentProcess::poll()
{
#ifndef Q_OS_WIN
    drain();
    if(!m_pending.isEmpty()){const auto sent=::write(m_descriptor,m_pending.constData(),qMin<qsizetype>(8192,m_pending.size()));if(sent>0)m_pending.remove(0,sent);}
    int status=0;const auto result=waitpid(pid_t(m_pid),&status,WNOHANG);
    if(result>0||(result<0&&errno==ECHILD)){drain();::close(m_descriptor);m_descriptor=-1;m_pid=0;m_timer->stop();m_pending.clear();emit finished(WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status));}
#endif
}
void LocalAgentProcess::write(const QByteArray &bytes)
{
    if(!isRunning())return;
#ifdef Q_OS_WIN
    if(m_process)m_process->write(bytes);
#else
    if(m_pending.size()+bytes.size()<=1024*1024)m_pending+=bytes;
#endif
}
void LocalAgentProcess::resize(int columns,int rows)
{
    m_columns=qBound(10,columns,500);m_rows=qBound(2,rows,500);
#ifndef Q_OS_WIN
    if(m_descriptor>=0){winsize dimensions{};dimensions.ws_col=m_columns;dimensions.ws_row=m_rows;ioctl(m_descriptor,TIOCSWINSZ,&dimensions);}
#endif
}
void LocalAgentProcess::stop()
{
    if(!isRunning())return;
#ifdef Q_OS_WIN
    m_process->kill();m_process->waitForFinished(1000);m_pid=0;
#else
    const pid_t pid=pid_t(m_pid); // forkpty makes the child the session/process-group leader.
    kill(-pid,SIGKILL);kill(pid,SIGKILL);int status=0;while(waitpid(pid,&status,0)<0&&errno==EINTR){}
    drain();if(m_descriptor>=0)::close(m_descriptor);m_descriptor=-1;m_pid=0;m_timer->stop();m_pending.clear();emit finished(137);
#endif
}
}
