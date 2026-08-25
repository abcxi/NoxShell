#pragma once

#include "../core/LinuxMetrics.h"

#include <QFrame>
#include <QHash>
#include <QPointF>

class QComboBox;
class QLabel;
class QTabBar;
class QTreeWidget;

namespace noxshell::ui {

class NetworkRateChart;

class SystemDetailPanel final : public QFrame {
    Q_OBJECT

public:
    explicit SystemDetailPanel(QWidget *parent = nullptr);

    void setSample(const MetricSample &sample);
    void reset(const QString &detail);

private:
    void refreshNetwork();
    void refreshProcesses();
    void refreshFileSystems();

    QComboBox *m_networkInterface{};
    QLabel *m_uploadRate{};
    QLabel *m_downloadRate{};
    NetworkRateChart *m_networkChart{};
    QTabBar *m_processTabs{};
    QTreeWidget *m_processList{};
    QTreeWidget *m_fileSystemList{};
    QLabel *m_emptyHint{};
    MetricSample m_sample;
    QHash<QString, QVector<QPointF>> m_networkHistory;
};

} // namespace noxshell::ui
