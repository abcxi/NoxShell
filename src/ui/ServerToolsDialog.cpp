#include "ServerToolsDialog.h"
#include "../core/SshSession.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace noxshell::ui {
namespace {
QLabel *note(const QString &text) {
    auto *label = new QLabel(text); label->setWordWrap(true); label->setTextFormat(Qt::PlainText); return label;
}
QTreeWidget *table(const QStringList &headers, const QString &name) {
    auto *tree = new QTreeWidget; tree->setObjectName(name); tree->setHeaderLabels(headers);
    tree->setRootIsDecorated(false); tree->setUniformRowHeights(true); tree->setAlternatingRowColors(true);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree->header()->setStretchLastSection(true); return tree;
}
QString rate(const QJsonValue &value, bool complete) {
    if (!value.isDouble()) return QStringLiteral("不可用");
    double number = value.toDouble(); int unit = 0;
    const QStringList units{"B/s", "KiB/s", "MiB/s", "GiB/s"};
    while (number >= 1024 && unit < 3) { number /= 1024; ++unit; }
    return (complete ? QString() : QStringLiteral("≥ ")) + QStringLiteral("%1 %2").arg(number, 0, 'f', 1).arg(units[unit]);
}
bool confirm(QWidget *parent, const QString &title, const QString &text) {
    QMessageBox box(QMessageBox::Warning, title, text, QMessageBox::Ok | QMessageBox::Cancel, parent);
    box.setTextFormat(Qt::PlainText); box.setDefaultButton(QMessageBox::Cancel);
    return box.exec() == QMessageBox::Ok;
}
}

ServerToolsDialog::ServerToolsDialog(SshSession *session, QWidget *parent)
    : QDialog(parent), m_session(session)
{
    setObjectName(QStringLiteral("serverToolsDialog")); setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("服务器工具 · %1 (%2)").arg(session->profile().name, session->profile().host));
    resize(860, 570);
    auto *layout = new QVBoxLayout(this);
    m_tabs = new QTabWidget; m_tabs->setObjectName(QStringLiteral("serverToolsTabs")); layout->addWidget(m_tabs, 1);
    auto *process = new QWidget; auto *pl = new QVBoxLayout(process);
    auto *pc = new QHBoxLayout; m_processSort = new QComboBox; m_processSort->addItems({"CPU", "内存"});
    m_processSort->setObjectName("processManagementSort");
    m_processLimit = new QSpinBox; m_processLimit->setRange(10, 500); m_processLimit->setValue(100);
    pc->addWidget(new QLabel(QStringLiteral("排序"))); pc->addWidget(m_processSort);
    pc->addWidget(new QLabel(QStringLiteral("最多显示"))); pc->addWidget(m_processLimit); pc->addStretch(); pl->addLayout(pc);
    m_processes = table({"PID", "进程名", "CPU", "内存 MiB", "状态"}, "managedProcessList"); pl->addWidget(m_processes, 1);
    auto *actions = new QHBoxLayout;
    const QStringList names{"结束 TERM", "强制结束 KILL", "暂停", "恢复"};
    const QStringList signalNames{"TERM", "KILL", "STOP", "CONT"};
    for (int i = 0; i < names.size(); ++i) {
        auto *button = new QPushButton(names[i]); button->setObjectName("processAction" + signalNames[i]);
        actions->addWidget(button); m_processActions.append(button);
        connect(button, &QPushButton::clicked, this, [this, action = signalNames[i]] { processAction(action); });
    }
    actions->addStretch(); pl->addLayout(actions);
    pl->addWidget(note(QStringLiteral("最多扫描 10000 个进程，不读取命令行参数。所有信号操作须确认并验证 PID 身份；关键进程和当前 SSH 链路受保护。旧内核/Python 不支持 pidfd 时仅可查看。")));
    m_tabs->addTab(process, QStringLiteral("进程管理"));

    auto *speed = new QWidget; auto *sl = new QVBoxLayout(speed); auto *form = new QFormLayout;
    m_source = new QComboBox; m_source->setObjectName("speedSourceAddress"); m_source->addItem(QStringLiteral("系统路由（不绑定）"), "");
    m_target = new QLineEdit; m_target->setObjectName("speedTestTarget"); m_target->setPlaceholderText(QStringLiteral("你控制的 iperf3 服务端 IP / 主机名"));
    m_target->setMaxLength(253);
    m_port = new QSpinBox; m_port->setRange(1, 65535); m_port->setValue(5201);
    m_mbps = new QSpinBox; m_mbps->setRange(1, 50); m_mbps->setValue(10); m_mbps->setSuffix(QStringLiteral(" Mbps"));
    m_mbps->setObjectName("speedTestMbps");
    m_direction = new QComboBox; m_direction->addItems({"上传到目标", "从目标下载"});
    form->addRow(QStringLiteral("绑定网卡 / IP"), m_source); form->addRow(QStringLiteral("测试目标"), m_target);
    form->addRow(QStringLiteral("端口"), m_port); form->addRow(QStringLiteral("速率上限"), m_mbps);
    form->addRow(QStringLiteral("方向"), m_direction); sl->addLayout(form);
    m_speed = new QPushButton(QStringLiteral("开始 3 秒单路 TCP 测试")); m_speed->setObjectName("startSpeedTest"); sl->addWidget(m_speed);
    sl->addWidget(note(QStringLiteral("主动测速必然占用服务器带宽和少量 CPU。默认 10 Mbps / 3 秒、单路，最高 50 Mbps；需人工确认。远端需 Python 3.8+、timeout、ip、iperf3，目标须已运行 iperf3 -s；不自动安装、开放端口或使用第三方测速站。点击下方刷新读取网卡，可选内网 IP。")));
    sl->addStretch(); m_tabs->addTab(speed, QStringLiteral("网速测试"));
    connect(m_speed, &QPushButton::clicked, this, &ServerToolsDialog::speedTest);

    auto *traffic = new QWidget; auto *tl = new QVBoxLayout(traffic); auto *tc = new QHBoxLayout;
    m_trafficSort = new QComboBox; m_trafficSort->addItems({"连接数最多", "带宽占用最多"});
    m_trafficSort->setObjectName("trafficClientSort");
    m_trafficLimit = new QSpinBox; m_trafficLimit->setObjectName("trafficClientLimit");
    m_trafficLimit->setRange(10, 100); m_trafficLimit->setValue(10);
    tc->addWidget(new QLabel(QStringLiteral("排序"))); tc->addWidget(m_trafficSort);
    tc->addWidget(new QLabel(QStringLiteral("前 N 个客户端"))); tc->addWidget(m_trafficLimit); tc->addStretch(); tl->addLayout(tc);
    m_clients = table({"客户端 IP", "连接数", "发往客户端", "来自客户端"}, "trafficClientList");
    m_clients->setColumnWidth(0, 240); tl->addWidget(m_clients, 1);
    tl->addWidget(note(QStringLiteral("按本机监听端口识别入站 TCP 已建立连接，不含回环、UDP 或转发流量；反代后看到的是 TCP 对端 IP。两次 TCP_INFO 字节计数之差估算速率，短连接/缺少计数时不可用；≥ 表示仅部分连接可测。每次最多 10000 条连接 / 4 MiB 快照，超限提示结果不完整，非完整计费带宽。远端需 Python 3.8+、timeout 和 ss。")));
    m_tabs->addTab(traffic, QStringLiteral("流量观测"));
    auto *bottom = new QHBoxLayout; m_auto = new QCheckBox(QStringLiteral("每 5 秒刷新（测速不自动运行）"));
    m_auto->setObjectName("serverToolsAutoRefresh"); m_refresh = new QPushButton(QStringLiteral("刷新"));
    m_refresh->setObjectName("refreshServerTools"); m_cancel = new QPushButton(QStringLiteral("取消任务"));
    m_cancel->setObjectName(QStringLiteral("cancelServerTools"));
    bottom->addWidget(m_auto); bottom->addStretch(); bottom->addWidget(m_refresh); bottom->addWidget(m_cancel); layout->addLayout(bottom);
    m_status = note(QStringLiteral("按需查询；同一会话只运行一个任务。")); m_status->setObjectName("serverToolsStatus"); layout->addWidget(m_status);
    m_timer = new QTimer(this); m_timer->setObjectName(QStringLiteral("serverToolsRefreshTimer")); m_timer->setInterval(5000);
    connect(m_timer, &QTimer::timeout, this, [this] {
        if (isVisible() && !isMinimized() && !QApplication::activeModalWidget()
            && m_tabs->currentIndex() != 1 && !m_request) refresh();
    });
    connect(m_auto, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled) m_timer->start(); else m_timer->stop();
        controls();
    });
    connect(m_refresh, &QPushButton::clicked, this, &ServerToolsDialog::refresh);
    connect(m_cancel, &QPushButton::clicked, this, &ServerToolsDialog::cancelTask);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] {
        if (m_request && m_session) {
            if (!m_cancelRequested) {
                m_refreshAfterCancel = true;
                m_session->cancelServerTool();
                m_status->setText(QStringLiteral("正在停止上一页查询，完成后查询当前页…"));
            }
        }
        else refresh();
    });
    connect(m_processes, &QTreeWidget::itemSelectionChanged, this, &ServerToolsDialog::controls);
    connect(m_source, &QComboBox::currentIndexChanged, this, &ServerToolsDialog::controls);
    connect(session, &SshSession::serverToolFinished, this, &ServerToolsDialog::finish);
    connect(session, &SshSession::connectionChanged, this, [this](bool connected, const QString &) {
        if (!connected) { m_auto->setChecked(false); m_status->setText(QStringLiteral("SSH 已断开")); } controls();
    });
    controls();
}

ServerToolsDialog::~ServerToolsDialog()
{
    if (m_request && m_session) m_session->cancelServerTool();
}

void ServerToolsDialog::showPage(int page)
{
    const bool same = m_tabs->currentIndex() == page;
    m_tabs->setCurrentIndex(page); show(); raise(); activateWindow();
    if (same && !m_request) refresh();
}
void ServerToolsDialog::controls()
{
    const bool connected = m_session && m_session->isConnected() && m_session->profile().connectionMode == ConnectionMode::Ssh;
    const bool ready = connected && !m_request;
    m_auto->setEnabled(connected && !m_cancelRequested);
    m_refresh->setEnabled(ready); m_speed->setEnabled(ready && !m_source->currentData(Qt::UserRole + 1).toBool());
    m_cancel->setEnabled(!m_cancelRequested && (m_request || m_auto->isChecked()));
    m_cancel->setText(m_cancelRequested ? QStringLiteral("正在取消…")
        : m_request ? QStringLiteral("取消任务") : m_auto->isChecked() ? QStringLiteral("停止自动刷新") : QStringLiteral("无运行任务"));
    m_cancel->setToolTip(m_cancelRequested ? QStringLiteral("正在等待远端停止并释放通道；不会启动新任务")
        : m_request ? QStringLiteral("取消当前任务，同时停止自动刷新和待执行查询")
        : m_auto->isChecked() ? QStringLiteral("停止后续自动查询，保留已有结果") : QStringLiteral("查询已结束，当前没有需要取消的任务"));
    for (auto *button : m_processActions) button->setEnabled(ready && m_safeSignals && m_processes->currentItem()
        && !m_processes->currentItem()->data(0, Qt::UserRole).toJsonObject().value("protected").toBool());
}
void ServerToolsDialog::cancelTask()
{
    m_refreshAfterCancel = false;
    m_auto->setChecked(false);
    m_timer->stop();
    m_cancelRequested = m_request != 0;
    if (m_request && m_session) {
        m_status->setText(QStringLiteral("正在取消任务并释放通道…自动刷新已停止。"));
        m_session->cancelServerTool();
    } else {
        m_status->setText(QStringLiteral("自动刷新已停止，当前无运行任务。"));
    }
    controls();
}
void ServerToolsDialog::refresh()
{
    const int page = m_tabs->currentIndex();
    if (page == 0) request({{"operation", "processes"}, {"limit", m_processLimit->value()}, {"sort", m_processSort->currentIndex() == 1 ? "memory" : "cpu"}});
    else if (page == 1) request({{"operation", "interfaces"}});
    else request({{"operation", "traffic"}, {"limit", m_trafficLimit->value()}, {"sort", m_trafficSort->currentIndex() == 1 ? "bandwidth" : "connections"}});
}
void ServerToolsDialog::request(QJsonObject options)
{
    if (m_request || !m_session) return;
    m_request = m_session->runServerTool(options);
    if (!m_request) {
        m_status->setText(QStringLiteral("无法启动：需真实 Linux SSH，且没有目录扫描、文件传输或其他查询正在进行。请稍后刷新。"));
        controls(); return;
    }
    m_cancelRequested = false;
    m_operation = options.value("operation").toString(); m_requestPage = m_tabs->currentIndex();
    m_status->setText(QStringLiteral("正在查询…可取消；不会把输出写入终端。")); controls();
}
void ServerToolsDialog::finish(quint64 id, const QByteArray &output, const QString &error)
{
    if (id != m_request) return;
    m_request = 0;
    if (m_cancelRequested) {
        m_cancelRequested = false;
        m_refreshAfterCancel = false;
        m_status->setText(m_session && m_session->isConnected()
            ? QStringLiteral("任务已取消，自动刷新已停止；保留上次结果。")
            : QStringLiteral("任务已停止，SSH 已断开。"));
        controls(); return;
    }
    if (m_refreshAfterCancel) { m_refreshAfterCancel = false; refresh(); return; }
    const auto result = QJsonDocument::fromJson(output).object();
    const auto failure = error.isEmpty() ? result.value("error").toString() : error;
    if (!failure.isEmpty() || result.isEmpty()) {
        m_status->setText(failure.isEmpty() ? QStringLiteral("返回格式无效；远端需要 Python 3.8+ 与相应工具") : failure.left(600)); controls(); return;
    }
    if (m_requestPage != m_tabs->currentIndex()) { controls(); return; }
    const auto rows = result.value("rows").toArray();
    if (m_operation == "processes") {
        m_processes->clear(); m_safeSignals = result.value("safeSignals").toBool();
        for (int i = 0; i < qMin(m_processLimit->value(), int(rows.size())); ++i) {
            const auto row = rows[i].toObject();
            auto *item = new QTreeWidgetItem(m_processes, {QString::number(row.value("pid").toInt()), row.value("name").toString().left(128),
                QString::number(row.value("cpu").toDouble(), 'f', 1) + '%', QString::number(row.value("rss").toDouble() / 1048576, 'f', 1), row.value("state").toString().left(8)});
            item->setData(0, Qt::UserRole, row);
        }
    } else if (m_operation == "traffic") {
        m_clients->clear();
        for (int i = 0; i < qMin(m_trafficLimit->value(), int(rows.size())); ++i) {
            const auto row = rows[i].toObject(); const bool complete = row.value("complete").toBool();
            new QTreeWidgetItem(m_clients, {row.value("ip").toString().left(128), QString::number(row.value("connections").toInt()),
                rate(row.value("tx"), complete), rate(row.value("rx"), complete)});
        }
    } else if (m_operation == "interfaces") {
        const auto current = m_source->currentData().toString(); m_source->clear(); m_source->addItem(QStringLiteral("系统路由（不绑定）"), "");
        for (int i = 0; i < qMin(512, int(rows.size())); ++i) {
            const auto row = rows[i].toObject();
            m_source->addItem(QStringLiteral("%1 · %2%3").arg(row.value("name").toString().left(64), row.value("ip").toString().left(64),
                row.value("private").toBool() ? QStringLiteral("（内网）") : QString()), row.value("ip").toString());
        }
        int selected = m_source->findData(current);
        if (!current.isEmpty() && selected < 0) {
            m_source->addItem(QStringLiteral("已失效，请重新选择 · %1").arg(current), current);
            selected = m_source->count() - 1;
            m_source->setItemData(selected, true, Qt::UserRole + 1);
        }
        m_source->setCurrentIndex(qMax(0, selected));
    }
    QString text = result.value("message").toString();
    if (m_operation == "speed") text += QStringLiteral(" · 发送 %1 Mbps / 接收 %2 Mbps").arg(result.value("sent").toDouble() / 1e6, 0, 'f', 2).arg(result.value("received").toDouble() / 1e6, 0, 'f', 2);
    if (text.isEmpty()) text = QStringLiteral("查询已完成，已更新 %1 行").arg(rows.size());
    if (result.value("partial").toBool()) text += QStringLiteral(" · 受扫描上限或权限限制，结果不完整，不代表全量排名");
    m_status->setText(text.left(700)); controls();
}
void ServerToolsDialog::processAction(const QString &action)
{
    auto *item = m_processes->currentItem(); if (!item || m_request || !m_session) return;
    const auto row = item->data(0, Qt::UserRole).toJsonObject();
    const QPointer<ServerToolsDialog> lifetime(this);
    if (!confirm(this, QStringLiteral("确认进程操作"), QStringLiteral("服务器 %1\nPID %2 · %3\n发送 %4 可能中断服务或丢失未保存数据。继续？")
        .arg(m_session->profile().host, item->text(0), item->text(1), action))) return;
    if (!lifetime) return;
    request({{"operation", "signal"}, {"pid", row.value("pid")}, {"start", row.value("start")}, {"action", action}});
}
void ServerToolsDialog::speedTest()
{
    if (m_request || !m_session || m_target->text().trimmed().isEmpty()) { m_status->setText(QStringLiteral("请先填写你控制的 iperf3 目标")); return; }
    const QPointer<ServerToolsDialog> lifetime(this);
    if (!confirm(this, QStringLiteral("确认主动测速"), QStringLiteral("从服务器 %1 到目标 %2:%3\n绑定：%4\n方向：%5\n3 秒、单路、上限 %6 Mbps。会占用真实网络带宽，确认开始？")
        .arg(m_session->profile().host, m_target->text().trimmed()).arg(m_port->value())
        .arg(m_source->currentText(), m_direction->currentText()).arg(m_mbps->value()))) return;
    if (!lifetime) return;
    request({{"operation", "speed"}, {"target", m_target->text().trimmed()}, {"port", m_port->value()},
        {"source", m_source->currentData().toString()}, {"mbps", m_mbps->value()}, {"reverse", m_direction->currentIndex() == 1}});
}
void ServerToolsDialog::closeEvent(QCloseEvent *event)
{
    m_refreshAfterCancel = false;
    m_timer->stop(); if (m_request && m_session) m_session->cancelServerTool();
    QDialog::closeEvent(event);
}
void ServerToolsDialog::hideEvent(QHideEvent *event)
{
    m_refreshAfterCancel = false;
    if (m_auto) m_auto->setChecked(false);
    if (m_request && m_session) m_session->cancelServerTool();
    QDialog::hideEvent(event);
}
}
