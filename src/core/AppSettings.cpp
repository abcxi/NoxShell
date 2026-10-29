#include "AppSettings.h"

#include <QSettings>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QSet>
#include <cmath>

namespace noxshell {
AppSettings *AppSettings::instance() { static AppSettings settings; return &settings; }

QVariantMap AppSettings::defaults()
{
    return {
        {"general/openTerminal", true}, {"general/showLatency", false},
        {"general/serverStatus", true}, {"general/confirmClose", true},
#ifdef Q_OS_MACOS
        {"general/closeBehavior", "hide"},
#else
        {"general/closeBehavior", "quit"},
#endif
        {"general/rdpImmersive", true}, {"terminal/copyOnSelect", false},
        {"terminal/rightClick", "menu"}, {"terminal/bracketedPaste", true},
        {"terminal/warnMultilinePaste", false}, {"terminal/autoComplete", true},
        {"terminal/prediction", true}, {"terminal/completionAccept", "Tab"},
        {"terminal/completionCycle", "arrows"}, {"terminal/completionWrap", false},
        {"terminal/idleMinutes", 120}, {"terminal/scrollback", 2000},
        {"terminal/sessionLog", false}, {"terminal/shell", "default"},
        {"terminal/wheelBehavior", "arrows"}, {"terminal/fontSize", 12},
        {"terminal/lineSpacing", 1.05}, {"terminal/autoEnglishInput", true},
        {"terminal/fontWeight", 400}, {"terminal/cursorShape", "shell"},
        {"terminal/cursorBlink", true}, {"terminal/palette", "follow"},
        {"terminal/backgroundImage", ""}, {"terminal/backgroundOpacity", 20},
        {"terminal/backgroundFit", "cover"}, {"files/overwrite", "ask"},
        {"files/autoSave", false}, {"agent/notifications", false},
        {"agent/default", "codex"}, {"proxy/mode", "direct"},
        {"proxy/type", "socks5"}, {"proxy/host", "127.0.0.1"},
        {"proxy/port", 1080}, {"proxy/user", ""}, {"proxy/credentialRef", ""},
        {"ai/enabled", false}, {"ai/baseUrl", ""}, {"ai/model", ""},
        {"ai/credentialRef", ""}, {"tabs/showUser", false}, {"tabs/showHost", false},
        {"tabs/maxTitleLength", 20}, {"tabs/confirmClose", true},
        {"ui/themeMode", "system"},
    };
}
namespace {
QVariant checkedSetting(const QString &key,const QVariant &input)
{
    static const QMap<QString,QStringList> enums{
        {"general/closeBehavior",{"quit","hide"}}, {"terminal/rightClick",{"menu","paste"}},
        {"terminal/completionAccept",{"Tab","Enter"}}, {"terminal/completionCycle",{"arrows","ctrl"}},
        {"terminal/shell",{"default","bash","zsh","sh","fish"}}, {"terminal/wheelBehavior",{"arrows","scroll"}},
        {"terminal/cursorShape",{"shell","block","beam","underline"}},
        {"terminal/palette",{"follow","light","dark","solarized","dracula"}},
        {"terminal/backgroundFit",{"cover","contain","stretch","tile"}},
        {"files/overwrite",{"ask","overwrite","skip"}}, {"proxy/mode",{"direct","system","manual"}},
        {"proxy/type",{"http","socks5"}}, {"ui/themeMode",{"system","light","dark"}},
        {"agent/default",{"codebuddy","codex","claude","opencode","kimi","agent"}}
    };
    static const QMap<QString,QPair<double,double>> ranges{
        {"terminal/idleMinutes",{15,720}}, {"terminal/scrollback",{1,10000}},
        {"terminal/fontSize",{8,32}}, {"terminal/fontWeight",{100,900}},
        {"terminal/lineSpacing",{1,2}}, {"terminal/backgroundOpacity",{0,100}},
        {"proxy/port",{1,65535}}, {"tabs/maxTitleLength",{8,60}}
    };
    if(enums.contains(key))return enums[key].contains(input.toString())?QVariant(input.toString()):QVariant{};
    if(ranges.contains(key)) {
        bool ok=false;const double n=input.toDouble(&ok);const auto range=ranges[key];
        if(!ok || !std::isfinite(n) || n<range.first || n>range.second)return {};
        if(key=="terminal/lineSpacing")return n;
        if(std::floor(n)!=n)return {};
        return int(n);
    }
    if(key.startsWith("shortcuts/")) {
        static const QStringList allowed{"shortcuts/settings","shortcuts/files","shortcuts/agent","shortcuts/splitVertical","shortcuts/splitHorizontal"};
        const QKeySequence seq(input.toString(),QKeySequence::PortableText);
        if(!allowed.contains(key)||seq.isEmpty()||seq.count()>1)return {};
        return seq.toString(QKeySequence::PortableText);
    }
    if(key=="highlight/rules") {
        const auto doc=QJsonDocument::fromJson(input.toByteArray());if(!doc.isArray()||doc.array().size()>64)return {};
        QSet<QString> ids;
        for(const auto &v:doc.array()) {
            const auto rule=v.toObject();const auto id=rule["id"].toString(),pattern=rule["pattern"].toString();
            if(id.isEmpty()||id.size()>80||ids.contains(id)||rule["name"].toString().size()>80||pattern.isEmpty()
                ||pattern.size()>512||!QRegularExpression(pattern).isValid()||!QColor(rule["color"].toString()).isValid())return {};
            ids.insert(id);
        }
        return doc.toJson(QJsonDocument::Compact);
    }
    const auto defaults=AppSettings::defaults();
    if(!defaults.contains(key) && key!="terminal/fontFamily")return {};
    if(defaults.value(key).typeId()==QMetaType::Bool)return input.typeId()==QMetaType::Bool?input:QVariant{};
    const auto text=input.toString();if(text.size()>4096)return {};
    return text;
}
}
QVariant AppSettings::defaultValue(const QString &key) { return defaults().value(key); }
QVariant AppSettings::value(const QString &key) { const auto v=checkedSetting(key,QSettings().value(key,defaultValue(key)));return v.isValid()?v:defaultValue(key); }
void AppSettings::setValue(const QString &key, const QVariant &v)
{
    const auto checked=checkedSetting(key,v);
    if (!checked.isValid() || value(key) == checked) return;
    QSettings s; s.setValue(key, checked); s.sync(); emit changed(key);
}
void AppSettings::reset()
{
    QSettings s;
    const auto all = defaults();
    for (auto it = all.cbegin(); it != all.cend(); ++it) s.remove(it.key());
    s.remove("terminal/fontFamily"); s.remove("highlight/rules"); s.sync(); emit changed({});
}
QList<HighlightRule> AppSettings::highlightRules()
{
    const auto stored = value("highlight/rules").toByteArray();
    if (!stored.isEmpty()) {
        QList<HighlightRule> rules;
        for (const auto &item : QJsonDocument::fromJson(stored).array()) {
            const auto o = item.toObject();
            rules.append({o["id"].toString(), o["name"].toString(), o["pattern"].toString(),
                QColor(o["color"].toString()), o["enabled"].toBool(), o["custom"].toBool()});
        }
        return rules;
    }
    return {
        {"ipv4", "IPv4 地址", R"(\b(?:(?:25[0-5]|2[0-4]\d|[01]?\d\d?)\.){3}(?:25[0-5]|2[0-4]\d|[01]?\d\d?)\b)", QColor("#00ACC1")},
        {"error", "ERROR 日志", R"((?i)\b(?:ERROR|FATAL|CRITICAL|FAIL(?:ED|URE)?)\b)", QColor("#F4515A")},
        {"warn", "WARN 日志", R"((?i)\bWARN(?:ING)?\b)", QColor("#E49A30")},
        {"info", "INFO 日志", R"((?i)\bINFO\b)", QColor("#27AE60")},
        {"debug", "DEBUG 日志", R"((?i)\bDEBUG\b)", QColor("#7588C7")},
        {"time", "时间戳", R"(\b\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:?\d{2})?\b|\b\d{2}:\d{2}:\d{2}\b)", QColor("#AD7FEF")},
        {"url", "URL", R"(https?://[^\s<>]+)", QColor("#3D9EED")},
        {"path", "文件路径", R"((?:~|/)(?:[\w.-]+/)+[\w.-]*)", QColor("#38B5A0")},
    };
}
void AppSettings::saveHighlightRules(const QList<HighlightRule> &rules)
{
    QJsonArray array;
    for (const auto &r : rules) array.append(QJsonObject{{"id", r.id}, {"name", r.name},
        {"pattern", r.pattern}, {"color", r.color.name()}, {"enabled", r.enabled}, {"custom", r.custom}});
    instance()->setValue("highlight/rules", QJsonDocument(array).toJson(QJsonDocument::Compact));
}
QNetworkProxy AppSettings::proxyFor(const QString &host, quint16 port)
{
    const auto mode = value("proxy/mode").toString();
    if (mode == "system") {
        const auto proxies = QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(host, port, "ssh"));
        for (const auto &p : proxies)
            if (p.type() == QNetworkProxy::Socks5Proxy || p.type() == QNetworkProxy::HttpProxy
                || p.type() == QNetworkProxy::NoProxy) return p;
        return QNetworkProxy(QNetworkProxy::NoProxy);
    }
    if (mode != "manual") return QNetworkProxy(QNetworkProxy::NoProxy);
    return QNetworkProxy(value("proxy/type").toString() == "http" ? QNetworkProxy::HttpProxy : QNetworkProxy::Socks5Proxy,
        value("proxy/host").toString(), value("proxy/port").value<quint16>(), value("proxy/user").toString());
}
} // namespace noxshell
