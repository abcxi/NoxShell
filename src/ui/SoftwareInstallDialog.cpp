#include "SoftwareInstallDialog.h"
#include "../core/SshSession.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QHideEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTextCursor>
#include <QTimer>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

namespace noxshell::ui {
namespace {
QStringList target(SshSession *session) {
    if (!session) return {};
    const auto p=session->profile();
    return {p.id,p.host,QString::number(p.port),p.user};
}
QLabel *note(const QString &text, const char *name) {
    auto *label=new QLabel(text);label->setObjectName(name);label->setTextFormat(Qt::PlainText);label->setWordWrap(true);return label;
}
bool packageName(const QString &text) {
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9](?:[A-Za-z0-9+._-]{0,98}[A-Za-z0-9])?\\z"));
    return expression.match(text).hasMatch();
}
}

SoftwareInstallDialog::SoftwareInstallDialog(SshSession *session, QWidget *parent)
    : QDialog(parent),m_session(session),m_target(target(session))
{
    setObjectName("softwareInstallDialog");resize(900,710);setMinimumSize(760,640);
    const auto profile=session->profile();
    setWindowTitle(QStringLiteral("软件安装 · %1@%2:%3").arg(profile.user,profile.host).arg(profile.port));
    m_jobKey="packageJobs/"+QString::fromLatin1(QCryptographicHash::hash(m_target.join('\n').toUtf8(),QCryptographicHash::Sha256).toHex())+"/job";
    const auto saved=QSettings().value(m_jobKey).toString();
    if(QRegularExpression("^[0-9a-f]{32}$").match(saved).hasMatch()){m_job=saved;m_jobActive=true;}
    auto *layout=new QVBoxLayout(this);layout->setSpacing(8);
    m_environment=note("按当前服务器的软件源查询；尚未读取包管理器。","softwareEnvironment");layout->addWidget(m_environment);
    auto *searchRow=new QHBoxLayout;
    m_query=new QLineEdit;m_query->setObjectName("softwareSearch");m_query->setMaxLength(64);m_query->setPlaceholderText("搜索软件包英文名称，例如 nginx、redis、htop；留空查看常用软件");
    m_search=new QPushButton("搜索");m_search->setObjectName("searchSoftware");
    m_clear=new QPushButton("清除 / 常用软件");m_clear->setObjectName("clearSoftwareSearch");
    searchRow->addWidget(m_query,1);searchRow->addWidget(m_search);searchRow->addWidget(m_clear);layout->addLayout(searchRow);
    m_packages=new QTreeWidget;m_packages->setObjectName("availableSoftwareList");
    m_packages->setHeaderLabels({"选择","软件包","状态","说明"});m_packages->setRootIsDecorated(false);m_packages->setUniformRowHeights(true);m_packages->setAlternatingRowColors(true);
    m_packages->header()->setSectionResizeMode(QHeaderView::Interactive);m_packages->header()->setStretchLastSection(true);
    m_packages->setColumnWidth(0,48);m_packages->setColumnWidth(1,190);m_packages->setColumnWidth(2,90);layout->addWidget(m_packages,2);
    layout->addWidget(note("每次最多显示 50 项、安装最多 10 项；只查询已有仓库缓存，不自动更新软件源。安装需要 root，不自动 sudo、添加源、升级系统或执行下载脚本。软件包自身可能安装依赖、启动/重启服务，请先确认业务影响。","softwareSafetyNote"));
    auto *logTitle=note("安装日志（末尾最多 32 KiB）","softwareLogTitle");layout->addWidget(logTitle);
    m_log=new QPlainTextEdit;m_log->setObjectName("softwareInstallLog");m_log->setReadOnly(true);m_log->setMaximumBlockCount(600);layout->addWidget(m_log,1);
    auto *bottom=new QHBoxLayout;
    m_install=new QPushButton("安装选中软件");m_install->setObjectName("installSelectedSoftware");
    m_refresh=new QPushButton("刷新任务状态");m_refresh->setObjectName("refreshSoftwareJob");
    m_forget=new QPushButton("清除失联任务引用");m_forget->setObjectName("forgetSoftwareJob");
    auto *close=new QPushButton("关闭");close->setObjectName("closeSoftwareInstall");
    bottom->addWidget(m_install);bottom->addWidget(m_refresh);bottom->addWidget(m_forget);bottom->addStretch();bottom->addWidget(close);layout->addLayout(bottom);
    m_status=note("安装前会展示完整命令并再次确认。关闭窗口或断开 SSH 不会强制停止已启动的后台安装。","softwareInstallStatus");
    m_status->setMinimumHeight(72);m_status->setAlignment(Qt::AlignLeft|Qt::AlignTop);
    m_status->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum);m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(m_status);
    m_timer=new QTimer(this);m_timer->setObjectName("softwareJobPollTimer");m_timer->setInterval(3000);
    connect(m_timer,&QTimer::timeout,this,[this]{if(isVisible()&&!isMinimized()&&!QApplication::activeModalWidget()&&!m_request)poll();});
    connect(m_search,&QPushButton::clicked,this,&SoftwareInstallDialog::search);
    connect(m_query,&QLineEdit::returnPressed,this,&SoftwareInstallDialog::search);
    connect(m_clear,&QPushButton::clicked,this,[this]{m_query->clear();search();});
    connect(m_refresh,&QPushButton::clicked,this,&SoftwareInstallDialog::poll);
    connect(m_install,&QPushButton::clicked,this,&SoftwareInstallDialog::installSelected);
    connect(m_forget,&QPushButton::clicked,this,&SoftwareInstallDialog::forgetJob);
    connect(close,&QPushButton::clicked,this,&QDialog::reject);
    connect(m_packages,&QTreeWidget::itemChanged,this,[this]{controls();});
    connect(session,&SshSession::serverToolFinished,this,&SoftwareInstallDialog::finish);
    connect(session,&SshSession::connectionChanged,this,[this](bool connected,const QString &){
        if(!connected){m_timer->stop();m_request=0;m_status->setText("SSH 已断开。已提交的后台安装可能仍在运行，重连原服务器后可刷新状态，勿重复安装。");}
        else if(isVisible()&&connectedToTarget()&&m_jobActive)poll();
        controls();
    });
    controls();
}

SoftwareInstallDialog::~SoftwareInstallDialog()
{
    // Cancelling a read-only RPC never kills the detached package process.
    if(m_request&&m_session&&m_operation!="packages.install")m_session->cancelServerTool(m_request);
}
bool SoftwareInstallDialog::connectedToTarget() const {
    return m_session&&m_session->isConnected()&&m_session->profile().connectionMode==ConnectionMode::Ssh&&belongsToCurrentProfile();
}
bool SoftwareInstallDialog::belongsToCurrentProfile() const {return m_session&&target(m_session)==m_target;}
void SoftwareInstallDialog::showManager() {
    show();raise();activateWindow();
    if(m_request)return;
    if(!m_job.isEmpty())poll();else search();
}
void SoftwareInstallDialog::hideEvent(QHideEvent *event) {
    m_timer->stop();
    if(m_request&&m_session&&m_operation!="packages.install"){
        m_ignoreReply=true;m_session->cancelServerTool(m_request);
    }
    QDialog::hideEvent(event);
}
void SoftwareInstallDialog::saveJob() {
    QSettings settings;
    if(m_job.isEmpty())settings.remove(m_jobKey);else settings.setValue(m_jobKey,m_job);
}
void SoftwareInstallDialog::controls() {
    const bool ready=connectedToTarget()&&!m_request;
    int selected=0;for(int i=0;i<m_packages->topLevelItemCount();++i){const auto *item=m_packages->topLevelItem(i);selected+=item->checkState(0)==Qt::Checked&&!item->data(0,Qt::UserRole).toBool();}
    m_search->setEnabled(ready&&!m_jobActive);m_clear->setEnabled(m_search->isEnabled());m_query->setEnabled(m_search->isEnabled());
    m_packages->setEnabled(ready&&!m_jobActive);
    m_install->setEnabled(ready&&m_root&&!m_jobActive&&selected>0&&selected<=10);
    m_install->setText(selected?QString("安装选中软件（%1）").arg(selected):QString("安装选中软件"));
    m_refresh->setEnabled(ready&&!m_job.isEmpty());m_forget->setEnabled(!m_request&&!m_job.isEmpty()&&m_jobUnknown);
}
bool SoftwareInstallDialog::request(QJsonObject options) {
    if(m_request||!connectedToTarget()){
        m_status->setText("需要连接原来的真实 Linux SSH 服务器；演示会话不能查询或安装。");controls();return false;
    }
    m_request=m_session->runServerTool(options);
    if(!m_request){m_status->setText("管理通道繁忙（采样/文件传输/其他查询），请稍后重试；未提交新操作。");controls();return false;}
    m_ignoreReply=false;m_operation=options.value("operation").toString();controls();return true;
}
void SoftwareInstallDialog::search() {
    if(m_jobActive)return;
    if(request({{"operation","packages.search"},{"query",m_query->text().trimmed()}}))m_status->setText("正在查询已有软件源缓存，最多返回 50 项…");
}
void SoftwareInstallDialog::poll() {
    if(m_job.isEmpty())return;
    if(request({{"operation","packages.status"},{"job",m_job}}))m_status->setText("正在读取后台安装状态；不会中止包管理器…");
}
void SoftwareInstallDialog::installSelected() {
    if(!m_install->isEnabled())return;
    m_selected.clear();
    for(int i=0;i<m_packages->topLevelItemCount();++i){auto *item=m_packages->topLevelItem(i);if(item->checkState(0)==Qt::Checked&&!item->data(0,Qt::UserRole).toBool())m_selected.append(item->text(1));}
    std::sort(m_selected.begin(),m_selected.end());
    if(m_selected.isEmpty()||m_selected.size()>10)return;
    for(const auto &name:m_selected)if(!packageName(name))return;
    QJsonArray packages;for(const auto &name:m_selected)packages.append(name);
    if(request({{"operation","packages.plan"},{"manager",m_manager},{"packages",packages}}))m_status->setText("正在校验软件包并生成安装计划；尚未安装…");
}
void SoftwareInstallDialog::forgetJob() {
    if(!m_jobUnknown||m_request)return;
    QMessageBox box(QMessageBox::Warning,"清除失联任务引用","这不会停止远程安装，也不会删除远端日志。请先在终端确认包管理器已经结束，再清除引用；否则可能重复提交安装。",QMessageBox::Ok|QMessageBox::Cancel,this);
    box.setDefaultButton(QMessageBox::Cancel);if(box.exec()!=QMessageBox::Ok)return;
    m_job.clear();m_jobActive=m_jobUnknown=false;saveJob();m_timer->stop();controls();
    m_status->setText("已清除本机任务引用；未中止远端进程或删除日志。请重新搜索后再操作。");
}
void SoftwareInstallDialog::finish(quint64 id,const QByteArray &output,const QString &error) {
    if(id!=m_request)return;
    m_request=0;
    if(m_ignoreReply){m_ignoreReply=false;m_status->setText("上一次只读查询已取消，请重新搜索或刷新任务状态；未提交新安装。");controls();return;}
    const auto operation=m_operation;
    const auto result=QJsonDocument::fromJson(output).object();
    const auto failure=error.isEmpty()?result.value("error").toString():error;
    if(!connectedToTarget()){controls();return;}
    if(!failure.isEmpty()||result.isEmpty()){
        m_status->setText((failure.isEmpty()?QString("服务器返回格式无效，需 Python 3.8+ 和包管理器"):failure.left(700))
            +(m_jobActive?QString("\n已有任务的状态未确认，勿重复安装；可刷新或到终端检查。 "):QString{}));
        if(!m_job.isEmpty()){m_jobUnknown=true;m_timer->stop();}
        controls();return;
    }
    if(operation=="packages.search"){
        const auto manager=result.value("manager").toString();
        if(manager!="apt"&&manager!="dnf"&&manager!="yum"){m_status->setText("不支持的包管理器");controls();return;}
        m_manager=manager;m_root=result.value("canInstall").toBool();m_packages->clear();
        const auto rows=result.value("rows").toArray();
        for(int i=0;i<qMin(50,int(rows.size()));++i){
            const auto row=rows[i].toObject();const auto name=row.value("name").toString();if(!packageName(name))continue;
            const bool installed=row.value("installed").toBool();
            auto *item=new QTreeWidgetItem(m_packages,{"",name,installed?"已安装":"可安装",row.value("description").toString().left(240)});
            item->setToolTip(3,item->text(3));item->setData(0,Qt::UserRole,installed);
            if(!installed){item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(0,Qt::Unchecked);}
            else item->setFlags(item->flags()&~Qt::ItemIsUserCheckable);
        }
        m_environment->setText(QString("当前目标：%1@%2:%3 · 包管理器 %4 · %5").arg(m_target[3],m_target[1],m_target[2],m_manager.toUpper(),m_root?"root 可安装":"非 root，仅查询，不自动提权"));
        m_status->setText(QString("已显示 %1 项%2。列表来自本机仓库缓存；缺少软件时请先在终端检查/更新软件源，不会自动添加第三方源。")
            .arg(m_packages->topLevelItemCount()).arg(result.value("partial").toBool()?"（结果已截断，请缩小关键词）":""));
    }else if(operation=="packages.plan"){
        QStringList packages;for(const auto &p:result.value("packages").toArray())packages.append(p.toString());
        const auto digest=result.value("plan").toString(),command=result.value("command").toString();
        if(!isVisible()||packages!=m_selected||result.value("manager").toString()!=m_manager
            ||!QRegularExpression("^[0-9a-f]{64}$").match(digest).hasMatch()||command.isEmpty()||command.size()>8192){
            m_status->setText("安装计划无效或窗口已隐藏，未安装。");controls();return;
        }
        QDialog approval(this);approval.setObjectName("softwareInstallConfirmation");approval.setWindowTitle("确认服务器软件安装");approval.resize(760,420);
        auto *layout=new QVBoxLayout(&approval);
        layout->addWidget(note(QString("将在 %1@%2:%3 安装：%4\n可能安装依赖或由软件包脚本启动/重启服务。关闭窗口、断线或退出玄壳后，后台安装仍可能继续；不提供强制取消。请检查完整命令。")
            .arg(m_target[3],m_target[1],m_target[2],m_selected.join(", ")),"softwareApprovalTarget"));
        auto *preview=new QPlainTextEdit(command);preview->setObjectName("softwareInstallCommand");preview->setReadOnly(true);layout->addWidget(preview,1);
        auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);buttons->button(QDialogButtonBox::Ok)->setText("确认安装");buttons->button(QDialogButtonBox::Cancel)->setDefault(true);layout->addWidget(buttons);
        connect(buttons,&QDialogButtonBox::accepted,&approval,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&approval,&QDialog::reject);
        if(approval.exec()!=QDialog::Accepted||!connectedToTarget()||!isVisible()||m_request||m_jobActive){m_status->setText("未提交安装。");controls();return;}
        QJsonArray array;for(const auto &p:m_selected)array.append(p);
        const auto job=QUuid::createUuid().toString(QUuid::Id128);
        if(request({{"operation","packages.install"},{"manager",m_manager},{"packages",array},{"plan",digest},{"job",job}})){
            m_job=job;m_jobActive=true;m_jobUnknown=false;saveJob();m_log->clear();m_status->setText("已提交安装启动请求，正在等待服务器确认；请勿重复提交。");controls();
        }
    }else if(operation=="packages.install"){
        if(result.value("job").toString()!=m_job){m_jobUnknown=true;m_status->setText("服务器返回任务编号不一致，请手动检查，勿重复安装。");}
        else {m_status->setText("后台安装已启动，关闭窗口不会中止。日志："+result.value("logPath").toString());m_jobUnknown=false;if(isVisible())m_timer->start();}
    }else if(operation=="packages.status"){
        if(result.value("job").toString()!=m_job){m_jobUnknown=true;controls();return;}
        const auto phase=result.value("phase").toString();
        m_log->setPlainText(result.value("log").toString().right(65536));m_log->moveCursor(QTextCursor::End);
        m_jobUnknown=phase=="unknown"||(phase!="starting"&&phase!="running"&&phase!="succeeded"&&phase!="failed");
        m_jobActive=phase!="succeeded"&&phase!="failed";
        if(phase=="succeeded"){
            const auto packages=result.value("packages").toArray();
            for(int i=0;i<m_packages->topLevelItemCount();++i){
                auto *item=m_packages->topLevelItem(i);
                if(packages.contains(item->text(1))){item->setCheckState(0,Qt::Unchecked);item->setData(0,Qt::UserRole,true);item->setFlags(item->flags()&~Qt::ItemIsUserCheckable);item->setText(2,"已安装");}
            }
        }
        const auto description=phase=="succeeded"?QString("安装完成"):phase=="failed"?QString("安装失败，请查看日志，不自动重试")
            :m_jobUnknown?QString("任务状态不确定，请到终端检查，勿重复安装"):QString("后台安装中，关闭窗口不会中止");
        m_status->setText(description+"\n任务："+m_job+" · 日志："+result.value("logPath").toString()+"\n"+result.value("error").toString().left(500)+result.value("logWarning").toString().left(200));
        if(m_jobActive&&!m_jobUnknown&&isVisible())m_timer->start();else m_timer->stop();
    }
    controls();
}
}
