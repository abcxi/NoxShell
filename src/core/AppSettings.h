#pragma once

#include <QObject>
#include <QVariant>
#include <QColor>
#include <QRegularExpression>
#include <QNetworkProxy>

namespace noxshell {

struct HighlightRule {
    QString id, name, pattern;
    QColor color;
    bool enabled{true};
    bool custom{false};
};

// Each read uses its own QSettings object, including worker-thread snapshots.
class AppSettings final : public QObject {
    Q_OBJECT
public:
    static AppSettings *instance();
    static QVariant value(const QString &key);
    static QVariant defaultValue(const QString &key);
    static QVariantMap defaults();
    static QList<HighlightRule> highlightRules();
    static void saveHighlightRules(const QList<HighlightRule> &rules);
    static QNetworkProxy proxyFor(const QString &host, quint16 port);
    void setValue(const QString &key, const QVariant &value);
    void reset();
signals:
    void changed(const QString &key);
};

} // namespace noxshell
