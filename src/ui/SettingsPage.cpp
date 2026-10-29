#include "SettingsPage.h"
#include "AppTheme.h"
#include "TerminalView.h"
#include "../core/AppSettings.h"
#include "../core/LocalAgentProcess.h"
#include "../core/CredentialStore.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QFile>
#include <QUrl>
#include <QEvent>
#include <QColorDialog>
#include <QPainter>
#include <QDir>
#include <QProcessEnvironment>

namespace noxshell::ui {
namespace {
class SettingsSwitch final : public QCheckBox {
public:
    using QCheckBox::QCheckBox;
    QSize sizeHint() const override {return {36,22};}
protected:
    bool hitButton(const QPoint &point) const override {return rect().contains(point);}
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);painter.setPen(Qt::NoPen);
        painter.setBrush(isEnabled()?(isChecked()?QColor("#168DFF"):QColor("#BAC4D2")):QColor("#D8DDE5"));
        painter.drawRoundedRect(QRectF(1,3,32,18),9,9);painter.setBrush(Qt::white);
        painter.drawEllipse(QRectF(isChecked()?17:3,5,14,14));
        if(hasFocus()){painter.setBrush(Qt::NoBrush);painter.setPen(QPen(palette().color(QPalette::Highlight),1));painter.drawRoundedRect(QRectF(0,2,34,20),10,10);}
    }
};
QString id(const QString &key) { auto s = key; return "setting_" + s.replace('/', '_'); }
QPushButton *button(const QString &text, QVBoxLayout *layout) { auto *b = new QPushButton(text); b->setMaximumWidth(250); layout->addWidget(b); return b; }
}
SettingsPage::SettingsPage(CredentialStore *credentials, QWidget *parent) : QWidget(parent), m_credentials(credentials)
{
    setObjectName("settingsPage");
    auto *row = new QHBoxLayout(this); row->setContentsMargins(12,12,12,12); row->setSpacing(28);
    m_navigation = new QListWidget; m_navigation->setObjectName("settingsNavigation");
    m_navigation->setFixedWidth(170); m_navigation->setSpacing(4);
    m_navigation->addItems({"⚙  通用设置", "◐  主题外观", "⌘  快捷键设置", "▣  标签设置", "✦  AI 设置", "✧  智能体", "✎  文本高亮", "⇄  代理设置", "ⓘ  关于我们"});
    m_pages = new QStackedWidget; m_pages->setObjectName("settingsPages");
    row->addWidget(m_navigation); row->addWidget(m_pages,1);
    buildGeneral(); buildAppearance(); buildShortcuts(); buildTabs(); buildAi(); buildAgents(); buildHighlight(); buildProxy(); buildAbout();
    connect(m_navigation, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    m_navigation->setCurrentRow(0); refreshStyle();
}
QVBoxLayout *SettingsPage::page(const QString &title, const QString &description)
{
    auto *scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget; body->setObjectName("settingsBody");
    auto *layout = new QVBoxLayout(body); layout->setContentsMargins(12,24,30,32); layout->setSpacing(16);
    auto *heading = new QLabel(title); heading->setProperty("settingsHeading",true); layout->addWidget(heading);
    if (!description.isEmpty()) { auto *hint = new QLabel(description); hint->setWordWrap(true); hint->setProperty("settingsHint",true); layout->addWidget(hint); }
    layout->addStretch(); scroll->setWidget(body); m_pages->addWidget(scroll); return layout;
}
QGridLayout *SettingsPage::section(QVBoxLayout *layout, const QString &title)
{
    if (!title.isEmpty()) { auto *label = new QLabel(title); label->setProperty("settingsSection",true); layout->insertWidget(layout->count()-1,label); }
    auto *grid = new QGridLayout; grid->setSpacing(16); grid->setColumnStretch(0,1); grid->setColumnStretch(1,1);
    layout->insertLayout(layout->count()-1,grid); return grid;
}
QWidget *SettingsPage::card(QGridLayout *grid, const QString &title, const QString &description, QWidget *control)
{
    auto *frame = new QFrame; frame->setProperty("settingsCard",true); frame->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);
    auto *layout = new QVBoxLayout(frame); layout->setContentsMargins(16,14,16,14); layout->setSpacing(10);
    auto *row = new QHBoxLayout; auto *label = new QLabel(title); label->setWordWrap(true); row->addWidget(label,1);
    if (control) { control->setMaximumWidth(180); row->addWidget(control); }
    layout->addLayout(row);
    if (!description.isEmpty()) { auto *hint=new QLabel(description); hint->setWordWrap(true); hint->setTextFormat(Qt::PlainText); hint->setProperty("settingsHint",true); layout->addWidget(hint); }
    const int index = grid->count(); grid->addWidget(frame,index/2,index%2); return frame;
}
void SettingsPage::toggle(QGridLayout *grid,const QString &key,const QString &title,const QString &description)
{
    auto *control=new SettingsSwitch; control->setObjectName(id(key)); control->setAccessibleName(title); control->setChecked(AppSettings::value(key).toBool());
    card(grid,title,description,control); connect(control,&QCheckBox::toggled,this,[key](bool b){AppSettings::instance()->setValue(key,b);});
}
void SettingsPage::choice(QGridLayout *grid,const QString &key,const QString &title,const QStringList &labels,const QStringList &values,const QString &description)
{
    auto *control=new QComboBox; control->setObjectName(id(key));
    for(int i=0;i<labels.size();++i) control->addItem(labels[i],values[i]);
    control->setCurrentIndex(qMax(0,control->findData(AppSettings::value(key).toString()))); card(grid,title,description,control);
    connect(control,&QComboBox::currentIndexChanged,this,[this,key,control]{AppSettings::instance()->setValue(key,control->currentData()); if(key=="ui/themeMode") emit themeModeRequested(control->currentData().toString());});
}
void SettingsPage::number(QGridLayout *grid,const QString &key,const QString &title,int low,int high,const QString &description)
{
    auto *control=new QSpinBox; control->setObjectName(id(key)); control->setRange(low,high); control->setValue(AppSettings::value(key).toInt()); card(grid,title,description,control);
    connect(control,&QSpinBox::valueChanged,this,[key](int n){AppSettings::instance()->setValue(key,n);});
}
void SettingsPage::buildGeneral()
{
    auto *p=page("通用设置","设置自动保存。终端外观和操作设置即时生效；连接参数在下次连接时应用。"); auto *g=section(p,"基础设置");
    toggle(g,"general/openTerminal","启动时打开终端","恢复上次保存的服务器标签；关闭后启动进入服务器管理。");
    toggle(g,"general/showLatency","显示网络延时状态","在各分屏显示独立 TCP 探测 RTT，每 10 秒刷新。");
    toggle(g,"general/serverStatus","显示服务器状态","启用当前服务器 CPU、内存、磁盘及网络采样。");
    toggle(g,"general/confirmClose","关闭时提醒","退出应用时提醒断开连接和文件传输。");
    choice(g,"general/closeBehavior","关闭时行为",{"退出","隐藏窗口"},{"quit","hide"},"隐藏窗口会保持连接，可通过 Dock 或任务栏恢复。");
    toggle(g,"general/rdpImmersive","远程桌面自动全屏","新建 RDP 连接使用全屏模式；已启动的客户端不受影响。");
    auto *cloud=new SettingsSwitch; cloud->setEnabled(false); card(g,"同步本地配置到云端","当前版本未接入云账号服务。可在“关于我们”中导出和导入本地设置。",cloud);
    g=section(p,"终端设置"); auto *renderer=new QComboBox; renderer->addItem("原生 QPainter"); renderer->setEnabled(false); card(g,"渲染模式","当前终端采用原生绘制，支持 ANSI、宽字符与输入法。",renderer);
    toggle(g,"terminal/copyOnSelect","选中时复制","将终端中选中的文本自动复制到剪贴板。");
    choice(g,"terminal/rightClick","鼠标右键行为",{"弹出右键菜单","直接粘贴"},{"menu","paste"});
    toggle(g,"terminal/bracketedPaste","启用括号粘贴","仅在远程 Shell 已声明支持时使用括号粘贴。");
    toggle(g,"terminal/warnMultilinePaste","多行粘贴时显示警告","发送多行文本前要求确认，避免意外执行命令。");
    toggle(g,"terminal/autoComplete","启用自动补全","底部命令输入框按本服务器历史命令提供建议。");
    toggle(g,"terminal/prediction","命令预测","使用最近输入的本服务器命令优先排序。");
    choice(g,"terminal/completionAccept","选中补全项",{"Tab","Enter"},{"Tab","Enter"},"仅作用于底部命令输入框，不拦截远程 Shell 的快捷键。");
    choice(g,"terminal/completionCycle","切换补全项",{"↓ / ↑","Ctrl+N / Ctrl+P"},{"arrows","ctrl"});
    toggle(g,"terminal/completionWrap","启用循环切换补全项","到达最后一个建议时回到第一个。");
    number(g,"terminal/idleMinutes","空闲连接保持时间（分钟）",15,720,"新连接在没有键盘输入的指定时间后断开；运行 Agent 的独立会话不自动关闭。");
    number(g,"terminal/scrollback","历史行数（ScrollBack）",1,10000,"降低上限会移除超出上限的历史行。");
    toggle(g,"terminal/sessionLog","记录详细会话日志","记录收到的终端输出，可能含敏感内容。默认关闭；日志保存在应用数据目录/session-logs。");
    choice(g,"terminal/shell","新建 Shell 连接类型",{"服务器默认","zsh","bash","sh","fish"},{"default","zsh","bash","sh","fish"},"远端必须已安装所选 Shell；作用于新连接。");
    choice(g,"terminal/wheelBehavior","交互模式下鼠标滚轮行为",{"上下键","滚动页面"},{"arrows","scroll"},"远端启用鼠标报告时，优先遵循远端程序的鼠标协议。");
    toggle(g,"terminal/autoEnglishInput","终端自动切换英文输入","macOS 聚焦终端时切换英文，离开后恢复输入法。");
    g=section(p,"文件管理设置");
    choice(g,"files/overwrite","上传文件已存在时的操作",{"每次提醒","覆盖","跳过"},{"ask","overwrite","skip"},"上传前检查远端目录，同名文件按此规则处理。");
    toggle(g,"files/autoSave","自动保存","文件修改后延迟 3 秒保存；每个分屏和文档分别计时。");
}
void SettingsPage::buildAppearance()
{
    auto *p=page("主题外观","应用界面与终端均跟随主题；终端色板可独立选择。"); auto *g=section(p,"主题预览");
    choice(g,"ui/themeMode","界面主题",{"跟随系统","浅色","深色"},{"system","light","dark"});
    choice(g,"terminal/palette","终端色板",{"跟随界面","Light","Dark","Solarized Dark","Dracula"},{"follow","light","dark","solarized","dracula"});
    auto *preview=new TerminalView; preview->setObjectName("settingsTerminalPreview"); preview->setFixedHeight(155); preview->feedText("\x1b[32muser@noxshell\x1b[0m:~$ ls\r\n\x1b[34mDocuments  Downloads\x1b[0m\r\nINFO  Terminal theme preview\r\n"); preview->setEnabled(false); p->insertWidget(p->count()-1,preview);
    g=section(p,"字体与光标"); auto *font=new QFontComboBox; font->setObjectName("setting_terminal_fontFamily"); font->setFontFilters(QFontComboBox::MonospacedFonts); font->setCurrentFont(QFont(AppSettings::value("terminal/fontFamily").toString().isEmpty()?TerminalView::defaultAppearance().fontFamily:AppSettings::value("terminal/fontFamily").toString())); card(g,"字体","等宽字体可保证终端网格对齐。",font); connect(font,&QFontComboBox::currentFontChanged,this,[](const QFont &f){AppSettings::instance()->setValue("terminal/fontFamily",f.family());});
    choice(g,"terminal/fontWeight","字重",{"Regular","Medium","Bold"},{"400","500","700"}); number(g,"terminal/fontSize","字号",8,32);
    auto *spacing=new QDoubleSpinBox; spacing->setObjectName("setting_terminal_lineSpacing"); spacing->setRange(1,2); spacing->setSingleStep(.05); spacing->setDecimals(2); spacing->setValue(AppSettings::value("terminal/lineSpacing").toDouble()); card(g,"行间距",{},spacing); connect(spacing,&QDoubleSpinBox::valueChanged,this,[](double d){AppSettings::instance()->setValue("terminal/lineSpacing",d);});
    choice(g,"terminal/cursorShape","光标形状",{"遵循 Shell","方块","竖线","下划线"},{"shell","block","beam","underline"}); toggle(g,"terminal/cursorBlink","光标闪烁","聚焦终端时闪烁光标。");
    g=section(p,"背景"); auto *upload=new QPushButton("上传图片…"); card(g,"终端背景图片","图片从本地读取。不会上传到服务器。",upload); connect(upload,&QPushButton::clicked,this,[this]{const auto path=QFileDialog::getOpenFileName(this,"选择背景图片",{},"图片 (*.png *.jpg *.jpeg *.webp *.bmp)"); if(!path.isEmpty()) AppSettings::instance()->setValue("terminal/backgroundImage",path);});
    auto *clear=new QPushButton("无背景"); card(g,"移除背景","恢复纯色终端背景。",clear); connect(clear,&QPushButton::clicked,this,[]{AppSettings::instance()->setValue("terminal/backgroundImage",QString{});});
    auto *opacity=new QSlider(Qt::Horizontal); opacity->setObjectName("setting_terminal_backgroundOpacity"); opacity->setRange(0,100); opacity->setValue(AppSettings::value("terminal/backgroundOpacity").toInt()); card(g,"图片不透明度","0% 为纯色背景，100% 完整展示图片。",opacity); connect(opacity,&QSlider::valueChanged,this,[](int n){AppSettings::instance()->setValue("terminal/backgroundOpacity",n);});
    choice(g,"terminal/backgroundFit","适配方式",{"填充","适应","拉伸","平铺"},{"cover","contain","stretch","tile"});
}
void SettingsPage::buildShortcuts()
{
    auto *p=page("快捷键设置","自定义工作区快捷键。终端内的 Ctrl+C 等按键继续发送到服务器。"); auto *g=section(p,"工作区操作");
#ifdef Q_OS_MACOS
    const QString split="Ctrl+D", horizontal="Ctrl+Shift+D";
#else
    const QString split="Ctrl+Shift+D", horizontal="Ctrl+Shift+E";
#endif
    const QList<QStringList> actions{{"splitVertical","左右分屏",split},{"splitHorizontal","上下分屏",horizontal},{"files","显示 / 隐藏文件管理","Ctrl+Shift+F"},{"agent","Shell / Agent","Ctrl+Shift+A"},{"settings","打开设置","Ctrl+,"}};
    for(const auto &a:actions) {
        auto *edit=new QKeySequenceEdit;const auto key="shortcuts/"+a[0];
        edit->setObjectName(id(key));edit->setKeySequence(QKeySequence(QSettings().value(key,a[2]).toString()));
        auto *frame=card(g,a[1],{},edit);auto *note=new QLabel;note->setWordWrap(true);note->setProperty("settingsHint",true);frame->layout()->addWidget(note);
        connect(edit,&QKeySequenceEdit::editingFinished,this,[edit,key,note,actions]{
            const auto seq=edit->keySequence();
            if(seq.isEmpty()||seq.count()>1){note->setText("请使用单组快捷键。");return;}
            for(const auto &action:actions) {
                const auto other="shortcuts/"+action[0];
                if(other!=key&&QKeySequence(QSettings().value(other,action[2]).toString())==seq){note->setText("该快捷键已被“"+action[1]+"”使用。");return;}
            }
            note->clear();AppSettings::instance()->setValue(key,seq.toString(QKeySequence::PortableText));
        });
    }
    connect(button("恢复默认快捷键",p),&QPushButton::clicked,this,[this,actions]{
        for(const auto &a:actions){QSettings().remove("shortcuts/"+a[0]);if(auto *edit=findChild<QKeySequenceEdit*>(id("shortcuts/"+a[0])))edit->setKeySequence(QKeySequence(a[2]));}
        emit AppSettings::instance()->changed("shortcuts/");
    });
}
void SettingsPage::buildTabs()
{
    auto *p=page("标签设置","标签下方的数据和操作始终属于当前服务器；分屏的文件和 Agent 会话各自独立。"); auto *g=section(p,"标签显示");
    toggle(g,"tabs/showUser","显示用户名","在标签标题中追加 SSH 用户名。"); toggle(g,"tabs/showHost","显示服务器地址","在标签标题中追加主机地址。"); number(g,"tabs/maxTitleLength","标题最大长度",8,60); toggle(g,"tabs/confirmClose","关闭标签时提醒","关闭 SSH 标签或分屏前进行确认。");
}
void SettingsPage::buildAi()
{
    auto *p=page("AI 设置","配置兼容 Chat Completions 的服务。在各分屏的 Agent 中选择“AI 助手”发送问题。仅发送手动输入的文本，不自动上传终端内容或执行命令。"); auto *g=section(p,"模型服务"); toggle(g,"ai/enabled","启用 AI 助手","请求仅在点击“发送”后发出，服务费用由所配置的提供方收取。");
    auto *endpoint=new QLineEdit(AppSettings::value("ai/baseUrl").toString()); endpoint->setObjectName("aiEndpoint"); endpoint->setPlaceholderText("https://your-service.example/v1"); card(g,"API 基础地址","将请求 POST 到此地址下的 /chat/completions。",endpoint);
    auto *model=new QLineEdit(AppSettings::value("ai/model").toString()); model->setObjectName("aiModel"); card(g,"模型名称","填写服务方提供的模型 ID。",model);
    auto *secret=new QLineEdit; secret->setObjectName("aiApiKey"); secret->setEchoMode(QLineEdit::Password); secret->setPlaceholderText("留空保留已有密钥"); card(g,"API Key","加密保存在本机凭据库，不写入设置文件或导出文件。",secret);
    auto *result=new QLabel; result->setWordWrap(true); p->insertWidget(p->count()-1,result);
    auto *save=button("保存 AI 配置",p); save->setObjectName("saveAiSettings"); connect(save,&QPushButton::clicked,this,[this,endpoint,model,secret,result]{const QUrl url(endpoint->text().trimmed()); if(!url.isValid()||url.host().isEmpty()||!url.userInfo().isEmpty()||(url.scheme()!="https"&&!(url.scheme()=="http"&&(url.host()=="localhost"||url.host()=="127.0.0.1"||url.host()=="::1")))){result->setText("请输入有效 HTTPS 地址；本地服务允许使用 HTTP。");return;} if(model->text().trimmed().isEmpty()){result->setText("请填写模型名称。");return;} if(!secret->text().isEmpty()){if(!m_credentials||!m_credentials->save("noxshell.ai.api",{secret->text(),{}})){result->setText("密钥保存失败："+(m_credentials?m_credentials->lastError():"凭据库不可用"));return;} AppSettings::instance()->setValue("ai/credentialRef","noxshell.ai.api"); secret->clear();} AppSettings::instance()->setValue("ai/baseUrl",url.toString(QUrl::RemoveQuery|QUrl::RemoveFragment)); AppSettings::instance()->setValue("ai/model",model->text().trimmed()); result->setText("已保存。可在 Agent → AI 助手中发送测试问题。");});
    connect(button("删除已保存的 API Key",p),&QPushButton::clicked,this,[this,result]{if(m_credentials&&m_credentials->remove(AppSettings::value("ai/credentialRef").toString())){AppSettings::instance()->setValue("ai/credentialRef",QString{});result->setText("API Key 已删除。");}});
}
void SettingsPage::buildAgents()
{
    auto *p=page("智能体设置","每个分屏独立调用本机 CLI 和本机登录状态。Codex、Claude Code、OpenCode 接入本分屏 SSH 工具；每条远程命令均需在玄壳确认。CLI 文件操作留在本机工作区，不会自动上传服务器。"); auto *g=section(p,{}); toggle(g,"agent/notifications","灵动岛通知","在应用顶部显示本机 Agent 启动、运行和结束状态。");
    g=section(p,"本机 CLI 检测");
    const QList<QStringList> agents{{"CodeBuddy Code","codebuddy"},{"Codex","codex"},{"Claude Code","claude"},{"OpenCode","opencode"},{"Kimi Code","kimi"},{"Cursor CLI","agent"}};
    for(const auto &a:agents){auto *detect=new QPushButton("重新检测"); auto *frame=card(g,a[0],"本机检测，不运行 CLI，不读取 CLI 登录密钥。",detect); g->removeWidget(frame);g->addWidget(frame,g->rowCount(),0,1,2); auto *state=new QLabel; frame->layout()->addWidget(state); const auto run=[state,a]{const auto path=LocalAgentProcess::findCli(a[1]);state->setText(path.isEmpty()?"本机未检测到":"本机已检测到："+path);state->setToolTip(path);}; connect(detect,&QPushButton::clicked,this,run);run();}
    g=section(p,"默认智能体"); choice(g,"agent/default","新 Agent 会话",{"CodeBuddy Code","Codex","Claude Code","OpenCode","Kimi Code","Cursor CLI"},{"codebuddy","codex","claude","opencode","kimi","agent"});
    card(g,"服务器工具","Codex、Claude Code 和 OpenCode 会接入本分屏的 SSH 工具。每条命令在服务器运行前都需用户确认。其他 CLI 仍可在本机使用，但本版本不会配置它们的远程工具。",nullptr);
}
void SettingsPage::buildHighlight()
{
    auto *p=page("文本高亮","只改变终端显示颜色，不改写远程输出；全屏交互程序保留原配色。"); auto *g=section(p,"预设规则");
    const auto rules=AppSettings::highlightRules();
    auto add=[this](QGridLayout *grid,const HighlightRule &rule){
        auto *check=new SettingsSwitch;check->setObjectName("highlight_"+rule.id);check->setAccessibleName(rule.name);check->setChecked(rule.enabled);
        auto *frame=card(grid,rule.name,rule.pattern,check);
        auto *title=frame->findChild<QLabel*>();
        auto *pattern=frame->findChildren<QLabel*>().last();
        auto *swatch=new QPushButton("颜色");swatch->setMaximumWidth(90);frame->layout()->addWidget(swatch);
        const auto paint=[swatch](const QColor &color){swatch->setStyleSheet("QPushButton{border:2px solid "+color.name()+";border-radius:5px;}");};paint(rule.color);
        connect(swatch,&QPushButton::clicked,this,[this,rule,paint]{auto all=AppSettings::highlightRules();for(auto &r:all)if(r.id==rule.id){const auto color=QColorDialog::getColor(r.color,this,"高亮颜色");if(color.isValid()){r.color=color;AppSettings::saveHighlightRules(all);paint(color);}break;}});
        connect(check,&QCheckBox::toggled,this,[rule](bool b){auto all=AppSettings::highlightRules();for(auto &r:all)if(r.id==rule.id)r.enabled=b;AppSettings::saveHighlightRules(all);});
        if(rule.custom){
            auto *edit=new QPushButton("编辑规则");frame->layout()->addWidget(edit);
            connect(edit,&QPushButton::clicked,this,[this,rule,title,pattern]{auto all=AppSettings::highlightRules();for(auto &r:all)if(r.id==rule.id){bool ok=false;const auto name=QInputDialog::getText(this,"编辑规则","规则名称",QLineEdit::Normal,r.name,&ok);if(!ok||name.trimmed().isEmpty()||name.size()>80)return;const auto text=QInputDialog::getText(this,"编辑规则","正则表达式（PCRE2）",QLineEdit::Normal,r.pattern,&ok);if(!ok)return;if(text.isEmpty()||text.size()>512||!QRegularExpression(text).isValid()){QMessageBox::warning(this,"无效规则","请输入有效且不超过 512 字符的表达式。");return;}r.name=name.trimmed();r.pattern=text;AppSettings::saveHighlightRules(all);title->setText(r.name);pattern->setText(r.pattern);break;}});
            auto *remove=new QPushButton("删除规则");frame->layout()->addWidget(remove);connect(remove,&QPushButton::clicked,this,[frame,rule]{auto all=AppSettings::highlightRules();all.removeIf([rule](const auto &r){return r.id==rule.id;});AppSettings::saveHighlightRules(all);frame->deleteLater();});
        }
    };
    for(const auto &r:rules)if(!r.custom)add(g,r);
    connect(button("预设规则一键全关",p),&QPushButton::clicked,this,[this]{auto rules=AppSettings::highlightRules();for(auto &r:rules)if(!r.custom)r.enabled=false;AppSettings::saveHighlightRules(rules);for(auto *c:findChildren<QCheckBox*>())if(c->objectName().startsWith("highlight_"))for(const auto &r:rules)if(c->objectName()=="highlight_"+r.id)c->setChecked(r.enabled);});
    auto *custom=section(p,"自定义规则");for(const auto &r:rules)if(r.custom)add(custom,r);
    connect(button("添加规则",p),&QPushButton::clicked,this,[this,custom,add]{bool ok=false;const auto name=QInputDialog::getText(this,"添加高亮规则","规则名称",QLineEdit::Normal,{},&ok);if(!ok||name.trimmed().isEmpty()||name.size()>80)return;const auto pattern=QInputDialog::getText(this,"正则表达式","匹配表达式（PCRE2）",QLineEdit::Normal,{},&ok);if(!ok)return;QRegularExpression regex(pattern);if(pattern.isEmpty()||pattern.size()>512||!regex.isValid()){QMessageBox::warning(this,"无效规则","表达式无效或过长："+regex.errorString());return;}const auto color=QColorDialog::getColor(QColor("#4EA5F6"),this,"高亮颜色");if(!color.isValid())return;HighlightRule r{QUuid::createUuid().toString(QUuid::WithoutBraces),name,pattern,color,true,true};auto all=AppSettings::highlightRules();all.append(r);AppSettings::saveHighlightRules(all);add(custom,r);});
    connect(button("自定义规则一键全开",p),&QPushButton::clicked,this,[this]{auto rules=AppSettings::highlightRules();for(auto &r:rules)if(r.custom)r.enabled=true;AppSettings::saveHighlightRules(rules);for(auto *c:findChildren<QCheckBox*>())for(const auto &r:rules)if(r.custom&&c->objectName()=="highlight_"+r.id)c->setChecked(true);});
}
void SettingsPage::buildProxy()
{
    auto *p=page("代理设置","代理用于新建 SSH / SFTP 连接；已建立的连接需要重连。AI 请求使用相同代理模式；RDP 由外部客户端管理。");auto *g=section(p,{});
    auto *mode=new QComboBox;mode->setObjectName("proxyMode");mode->addItem("直连","direct");mode->addItem("跟随系统","system");mode->addItem("手动","manual");mode->setCurrentIndex(qMax(0,mode->findData(AppSettings::value("proxy/mode"))));card(g,"代理模式",{},mode);
    auto *type=new QComboBox;type->setObjectName("proxyType");type->addItem("SOCKS5","socks5");type->addItem("HTTP CONNECT","http");type->setCurrentIndex(qMax(0,type->findData(AppSettings::value("proxy/type"))));card(g,"代理类型",{},type);
    auto *host=new QLineEdit(AppSettings::value("proxy/host").toString());host->setObjectName("proxyHost");card(g,"代理地址",{},host);auto *port=new QSpinBox;port->setObjectName("proxyPort");port->setRange(1,65535);port->setValue(AppSettings::value("proxy/port").toInt());card(g,"端口",{},port);
    auto *user=new QLineEdit(AppSettings::value("proxy/user").toString());user->setObjectName("proxyUser");card(g,"用户名（可选）",{},user);auto *password=new QLineEdit;password->setObjectName("proxyPassword");password->setEchoMode(QLineEdit::Password);password->setPlaceholderText("留空保留已有密码");card(g,"密码（可选）","密码保存在本机加密凭据库。",password);
    const auto enable=[mode,type,host,port,user,password]{for(QWidget *w:QList<QWidget*>{type,host,port,user,password})w->setEnabled(mode->currentData()=="manual");};connect(mode,&QComboBox::currentIndexChanged,this,enable);enable();
    auto *state=new QLabel;p->insertWidget(p->count()-1,state);auto *save=button("保存",p);save->setObjectName("saveProxySettings");connect(save,&QPushButton::clicked,this,[this,mode,type,host,port,user,password,state]{if(mode->currentData()=="manual"&&(host->text().trimmed().isEmpty()||host->text().contains(QRegularExpression("[\\s/\\r\\n]")))){state->setText("请输入有效代理地址（不含协议或路径）。");return;}if(!password->text().isEmpty()){if(!m_credentials||!m_credentials->save("noxshell.proxy",{password->text(),{}})){state->setText("代理密码保存失败。");return;}AppSettings::instance()->setValue("proxy/credentialRef","noxshell.proxy");password->clear();}for(const auto &pair:QList<QPair<QString,QVariant>>{{"proxy/mode",mode->currentData()},{"proxy/type",type->currentData()},{"proxy/host",host->text().trimmed()},{"proxy/port",port->value()},{"proxy/user",user->text()}})AppSettings::instance()->setValue(pair.first,pair.second);state->setText("已保存。新连接生效；已建立的连接请重连。");});
    auto *forget=button("删除已保存的代理密码",p);forget->setObjectName("deleteProxyPassword");connect(forget,&QPushButton::clicked,this,[this,state]{const auto ref=AppSettings::value("proxy/credentialRef").toString();if(ref.isEmpty()){state->setText("未保存代理密码。");return;}if(m_credentials&&m_credentials->remove(ref)){AppSettings::instance()->setValue("proxy/credentialRef",QString{});state->setText("代理密码已删除。");}else state->setText("代理密码删除失败。");});
}
void SettingsPage::buildAbout()
{
    auto *p=page("关于玄壳 · NoxShell","原生 SSH / SFTP 连接管理、分屏终端、文件编辑与 Coding Agent 工作区。");auto *g=section(p,"应用信息");
#ifdef NOXSHELL_APP_VERSION
    card(g,"版本",QStringLiteral(NOXSHELL_APP_VERSION),nullptr);
#else
    card(g,"版本","开发构建",nullptr);
#endif
    card(g,"运行环境",QStringLiteral("Qt %1 · %2").arg(qVersion(),QSysInfo::prettyProductName()),nullptr);
    connect(button("导出本地设置…",p),&QPushButton::clicked,this,[this]{const auto path=QFileDialog::getSaveFileName(this,"导出设置","noxshell-settings.json","JSON (*.json)");if(path.isEmpty())return;QJsonObject values;QSettings settings;for(const auto &key:settings.allKeys())if((AppSettings::defaults().contains(key)||key=="terminal/fontFamily"||key.startsWith("shortcuts/")||key=="highlight/rules")&&!key.endsWith("credentialRef"))values[key]=QJsonValue::fromVariant(settings.value(key));QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(QJsonDocument(QJsonObject{{"format","noxshell-settings-v1"},{"settings",values}}).toJson())<0)QMessageBox::warning(this,"导出失败",f.errorString());});
    connect(button("导入本地设置…",p),&QPushButton::clicked,this,[this]{const auto path=QFileDialog::getOpenFileName(this,"导入设置",{},"JSON (*.json)");if(path.isEmpty())return;QFile f(path);if(!f.open(QIODevice::ReadOnly)||f.size()>1024*1024)return;const auto root=QJsonDocument::fromJson(f.readAll()).object();if(root["format"]!="noxshell-settings-v1"){QMessageBox::warning(this,"导入失败","文件格式不正确。");return;}const auto settings=root["settings"].toObject();for(auto it=settings.begin();it!=settings.end();++it)if((AppSettings::defaults().contains(it.key())||it.key()=="terminal/fontFamily"||it.key().startsWith("shortcuts/")||it.key()=="highlight/rules")&&!it.key().endsWith("credentialRef"))AppSettings::instance()->setValue(it.key(),it.value().toVariant());emit themeModeRequested(AppSettings::value("ui/themeMode").toString());QMessageBox::information(this,"导入完成","设置已应用，重新打开设置页以刷新控件。服务器与密钥不包含在此文件中。");});
}
void SettingsPage::changeEvent(QEvent *event){QWidget::changeEvent(event);if(event->type()==QEvent::PaletteChange)refreshStyle();}
void SettingsPage::refreshStyle()
{
    const bool dark=isApplicationDarkTheme();
    if(m_styleInitialized && m_darkStyle==dark)return;
    m_styleInitialized=true;m_darkStyle=dark;
    const auto theme=workspaceColors(dark);
    setStyleSheet(QStringLiteral("QWidget#settingsPage,QWidget#settingsBody{background:%1;color:%2;} QListWidget#settingsNavigation{background:%1;border:0;outline:0;} QListWidget#settingsNavigation::item{height:36px;padding-left:12px;border-radius:6px;} QListWidget#settingsNavigation::item:selected{background:%3;color:#168DFF;} QFrame[settingsCard=true]{background:%4;border:1px solid %5;border-radius:8px;} QLabel[settingsHint=true]{color:%6;font-size:12px;} QLabel[settingsHeading=true]{font-size:22px;font-weight:600;margin-bottom:8px;} QLabel[settingsSection=true]{font-size:16px;font-weight:600;margin-top:10px;} QCheckBox::indicator{width:30px;height:16px;border-radius:8px;background:%7;border:0;} QCheckBox::indicator:checked{background:#168DFF;} QCheckBox::indicator:disabled{background:%5;} QComboBox,QLineEdit,QSpinBox,QDoubleSpinBox,QKeySequenceEdit{min-height:26px;} QPushButton{min-height:28px;padding:2px 12px;} QScrollArea{border:0;background:%1;}")
        .arg(theme.canvas.name(),theme.text.name(),dark?"#293C53":"#EAF1FC",theme.surface.name(),
            theme.line.name(),theme.muted.name(),dark?"#53617A":"#BAC4D2"));
}
}
