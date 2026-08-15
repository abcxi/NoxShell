#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

namespace noxshell {

struct MonitoringThresholds {
    double cpuPercent{85.0};
    double memoryPercent{85.0};
    double loadPercent{90.0};
    double diskPercent{85.0};
};

struct MonitoringAlert {
    qint64 id{};
    QString serverId;
    QString metric;
    double value{};
    double threshold{};
    QDateTime createdAt;
};

} // namespace noxshell
