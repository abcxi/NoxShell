#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QVector>

namespace noxshell {

struct LinuxCpuTimes {
    quint64 user{};
    quint64 nice{};
    quint64 system{};
    quint64 idle{};
    quint64 ioWait{};
    quint64 irq{};
    quint64 softIrq{};
    quint64 steal{};

    [[nodiscard]] quint64 total() const;
    [[nodiscard]] quint64 idleTotal() const;
};

struct LinuxDiskUsage {
    QString fileSystem;
    QString mountPoint;
    quint64 totalBytes{};
    quint64 usedBytes{};
    quint64 availableBytes{};
    int usagePercent{};
};

struct LinuxNetworkUsage {
    QString interfaceName;
    quint64 receivedBytes{};
    quint64 transmittedBytes{};
};

struct LinuxNetworkRate {
    QString interfaceName;
    double receivedBytesPerSecond{};
    double transmittedBytesPerSecond{};
};

struct LinuxProcessUsage {
    int pid{};
    QString user;
    double cpuPercent{};
    double memoryPercent{};
    quint64 residentBytes{};
    QString command;
};

struct LinuxMetricsSnapshot {
    QDateTime capturedAt;
    LinuxCpuTimes cpu;
    quint64 memoryTotalBytes{};
    quint64 memoryAvailableBytes{};
    double load1{};
    double load5{};
    double load15{};
    quint64 uptimeSeconds{};
    int cpuCoreCount{1};
    QVector<LinuxDiskUsage> disks;
    QVector<LinuxNetworkUsage> networks;
    QVector<LinuxProcessUsage> processes;
};

struct MetricSample {
    QDateTime capturedAt;
    bool cpuReady{false};
    double cpuPercent{};
    double kernelPercent{};
    quint64 memoryTotalBytes{};
    quint64 memoryUsedBytes{};
    double memoryPercent{};
    double load1{};
    double load5{};
    double load15{};
    quint64 uptimeSeconds{};
    int cpuCoreCount{1};
    LinuxDiskUsage primaryDisk;
    QVector<LinuxDiskUsage> disks;
    QVector<LinuxNetworkRate> networkRates;
    QVector<LinuxProcessUsage> processes;
};

class LinuxMetricsParser final {
public:
    static bool parse(const QByteArray &payload, LinuxMetricsSnapshot &snapshot, QString *error = nullptr);
    static MetricSample calculate(const LinuxMetricsSnapshot &current, const LinuxMetricsSnapshot *previous = nullptr);
};

} // namespace noxshell

Q_DECLARE_METATYPE(noxshell::MetricSample)
