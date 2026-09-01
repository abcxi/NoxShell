#include "LinuxMetrics.h"

#include <QRegularExpression>

#include <algorithm>

namespace noxshell {

namespace {
enum class Section {
    None,
    Cpu,
    Memory,
    Load,
    Cores,
    Disk,
    Uptime,
    Network,
    Process,
};

QList<QByteArray> fields(const QByteArray &line)
{
    return line.simplified().split(' ');
}

quint64 kilobytesToBytes(quint64 value)
{
    constexpr quint64 bytesPerKilobyte = 1024;
    return value * bytesPerKilobyte;
}

void setError(QString *error, const QString &message)
{
    if (error) *error = message;
}

bool parseCpuTimes(const QList<QByteArray> &parts, LinuxCpuTimes &times)
{
    if (parts.size() < 5) return false;
    bool ok = true;
    auto value = [&parts, &ok](qsizetype index) {
        if (index >= parts.size()) return quint64{0};
        bool fieldOk = false;
        const auto parsed = parts.at(index).toULongLong(&fieldOk);
        ok = ok && fieldOk;
        return parsed;
    };
    times.user = value(1);
    times.nice = value(2);
    times.system = value(3);
    times.idle = value(4);
    times.ioWait = value(5);
    times.irq = value(6);
    times.softIrq = value(7);
    times.steal = value(8);
    return ok;
}

bool cpuUsage(const LinuxCpuTimes &current, const LinuxCpuTimes &previous, double &usage)
{
    const auto totalNow = current.total();
    const auto totalBefore = previous.total();
    const auto idleNow = current.idleTotal();
    const auto idleBefore = previous.idleTotal();
    if (totalNow <= totalBefore || idleNow < idleBefore) return false;
    const auto totalDelta = totalNow - totalBefore;
    const auto idleDelta = qMin(totalDelta, idleNow - idleBefore);
    usage = 100.0 * static_cast<double>(totalDelta - idleDelta) / static_cast<double>(totalDelta);
    return true;
}
} // namespace

quint64 LinuxCpuTimes::total() const
{
    return user + nice + system + idle + ioWait + irq + softIrq + steal;
}

quint64 LinuxCpuTimes::idleTotal() const
{
    return idle + ioWait;
}

bool LinuxMetricsParser::parse(const QByteArray &payload, LinuxMetricsSnapshot &snapshot, QString *error)
{
    snapshot = {};
    snapshot.capturedAt = QDateTime::currentDateTime();
    Section section = Section::None;
    bool hasCpu = false;
    bool hasMemoryTotal = false;
    bool hasMemoryAvailable = false;
    bool hasLoad = false;
    bool hasCores = false;
    quint64 memoryFree = 0;
    quint64 buffers = 0;
    quint64 cached = 0;
    quint64 reclaimable = 0;
    quint64 sharedMemory = 0;

    const auto lines = payload.split('\n');
    for (auto line : lines) {
        line = line.trimmed();
        if (line.endsWith('\r')) line.chop(1);
        if (line == "__CPU__") {
            section = Section::Cpu;
            continue;
        }
        if (line == "__MEM__") {
            section = Section::Memory;
            continue;
        }
        if (line == "__LOAD__") {
            section = Section::Load;
            continue;
        }
        if (line == "__CORES__") {
            section = Section::Cores;
            continue;
        }
        if (line == "__DISK__") {
            section = Section::Disk;
            continue;
        }
        if (line == "__UPTIME__") {
            section = Section::Uptime;
            continue;
        }
        if (line == "__NET__") {
            section = Section::Network;
            continue;
        }
        if (line == "__PROC__") {
            section = Section::Process;
            continue;
        }
        if (line.isEmpty()) continue;

        const auto parts = fields(line);
        switch (section) {
        case Section::Cpu: {
            if (parts.isEmpty()) break;
            if (parts.first() == "cpu") {
                hasCpu = parseCpuTimes(parts, snapshot.cpu);
            } else if (parts.first().startsWith("cpu")) {
                LinuxCpuTimes core;
                if (parseCpuTimes(parts, core)) snapshot.cpuCores.append(core);
            }
            break;
        }
        case Section::Memory: {
            if (parts.size() < 2) break;
            bool ok = false;
            const auto value = kilobytesToBytes(parts.at(1).toULongLong(&ok));
            if (!ok) break;
            const auto key = parts.first();
            if (key == "MemTotal:") {
                snapshot.memoryTotalBytes = value;
                hasMemoryTotal = true;
            } else if (key == "MemAvailable:") {
                snapshot.memoryAvailableBytes = value;
                hasMemoryAvailable = true;
            } else if (key == "MemFree:") {
                memoryFree = value;
            } else if (key == "Buffers:") {
                buffers = value;
            } else if (key == "Cached:") {
                cached = value;
            } else if (key == "SReclaimable:") {
                reclaimable = value;
            } else if (key == "Shmem:") {
                sharedMemory = value;
            }
            break;
        }
        case Section::Load: {
            if (parts.size() < 3) break;
            bool ok1 = false;
            bool ok5 = false;
            bool ok15 = false;
            snapshot.load1 = parts.at(0).toDouble(&ok1);
            snapshot.load5 = parts.at(1).toDouble(&ok5);
            snapshot.load15 = parts.at(2).toDouble(&ok15);
            hasLoad = ok1 && ok5 && ok15;
            break;
        }
        case Section::Cores: {
            bool ok = false;
            const int count = parts.first().toInt(&ok);
            if (ok && count > 0) {
                snapshot.cpuCoreCount = count;
                hasCores = true;
            }
            break;
        }
        case Section::Disk: {
            if (line.startsWith("Filesystem") || parts.size() < 6) break;
            bool totalOk = false;
            bool usedOk = false;
            bool availableOk = false;
            const auto total = kilobytesToBytes(parts.at(1).toULongLong(&totalOk));
            const auto used = kilobytesToBytes(parts.at(2).toULongLong(&usedOk));
            const auto available = kilobytesToBytes(parts.at(3).toULongLong(&availableOk));
            auto percentText = parts.at(4);
            if (percentText.endsWith('%')) percentText.chop(1);
            bool percentOk = false;
            const int percent = percentText.toInt(&percentOk);
            if (!totalOk || !usedOk || !availableOk || !percentOk) break;
            LinuxDiskUsage disk;
            disk.fileSystem = QString::fromUtf8(parts.first());
            disk.mountPoint = QString::fromUtf8(parts.mid(5).join(' '));
            disk.totalBytes = total;
            disk.usedBytes = used;
            disk.availableBytes = available;
            disk.usagePercent = qBound(0, percent, 100);
            snapshot.disks.append(std::move(disk));
            break;
        }
        case Section::Uptime: {
            bool ok = false;
            const double seconds = parts.first().toDouble(&ok);
            if (ok && seconds >= 0.0) snapshot.uptimeSeconds = static_cast<quint64>(seconds);
            break;
        }
        case Section::Network: {
            const auto separator = line.indexOf(':');
            if (separator <= 0) break;
            const auto interfaceName = line.left(separator).trimmed();
            if (interfaceName.isEmpty() || interfaceName == "Inter-|") break;
            const auto values = fields(line.mid(separator + 1));
            if (values.size() < 9) break;
            bool receivedOk = false;
            bool transmittedOk = false;
            const auto received = values.at(0).toULongLong(&receivedOk);
            const auto transmitted = values.at(8).toULongLong(&transmittedOk);
            if (!receivedOk || !transmittedOk) break;
            snapshot.networks.append({QString::fromUtf8(interfaceName), received, transmitted});
            break;
        }
        case Section::Process: {
            if (parts.size() < 6) break;
            bool pidOk = false;
            bool cpuOk = false;
            bool memoryOk = false;
            bool residentOk = false;
            LinuxProcessUsage process;
            process.pid = parts.at(0).toInt(&pidOk);
            process.user = QString::fromUtf8(parts.at(1));
            process.cpuPercent = parts.at(2).toDouble(&cpuOk);
            process.memoryPercent = parts.at(3).toDouble(&memoryOk);
            process.residentBytes = kilobytesToBytes(parts.at(4).toULongLong(&residentOk));
            process.command = QString::fromUtf8(parts.mid(5).join(' '));
            if (pidOk && cpuOk && memoryOk && residentOk && process.pid > 0 && !process.command.isEmpty()) {
                const auto existing = std::find_if(snapshot.processes.cbegin(), snapshot.processes.cend(),
                    [&process](const LinuxProcessUsage &candidate) { return candidate.pid == process.pid; });
                if (existing == snapshot.processes.cend()) snapshot.processes.append(std::move(process));
            }
            break;
        }
        case Section::None:
            break;
        }
    }

    if (!hasMemoryAvailable && hasMemoryTotal) {
        const auto fallback = memoryFree + buffers + cached + reclaimable;
        snapshot.memoryAvailableBytes = fallback > sharedMemory ? fallback - sharedMemory : fallback;
        hasMemoryAvailable = fallback > 0;
    }
    if (!hasCpu) {
        setError(error, QStringLiteral("无法解析 /proc/stat"));
        return false;
    }
    if (!hasMemoryTotal || !hasMemoryAvailable || snapshot.memoryAvailableBytes > snapshot.memoryTotalBytes) {
        setError(error, QStringLiteral("无法解析 /proc/meminfo"));
        return false;
    }
    if (!hasLoad) {
        setError(error, QStringLiteral("无法解析 /proc/loadavg"));
        return false;
    }
    if (!hasCores) {
        setError(error, QStringLiteral("无法获取 CPU 核心数"));
        return false;
    }
    return true;
}

MetricSample LinuxMetricsParser::calculate(const LinuxMetricsSnapshot &current, const LinuxMetricsSnapshot *previous)
{
    MetricSample sample;
    sample.capturedAt = QDateTime::currentDateTime();
    sample.memoryTotalBytes = current.memoryTotalBytes;
    sample.memoryUsedBytes = current.memoryTotalBytes - current.memoryAvailableBytes;
    if (sample.memoryTotalBytes > 0) {
        sample.memoryPercent = 100.0 * static_cast<double>(sample.memoryUsedBytes) / static_cast<double>(sample.memoryTotalBytes);
    }
    sample.load1 = current.load1;
    sample.load5 = current.load5;
    sample.load15 = current.load15;
    sample.uptimeSeconds = current.uptimeSeconds;
    sample.cpuCoreCount = qMax(1, current.cpuCoreCount);
    sample.disks = current.disks;
    sample.processes = current.processes;

    const auto rootDisk = std::find_if(current.disks.cbegin(), current.disks.cend(), [](const LinuxDiskUsage &disk) {
        return disk.mountPoint == QStringLiteral("/");
    });
    if (rootDisk != current.disks.cend()) {
        sample.primaryDisk = *rootDisk;
    } else if (!current.disks.isEmpty()) {
        sample.primaryDisk = current.disks.first();
    }

    if (previous) {
        const auto totalNow = current.cpu.total();
        const auto totalBefore = previous->cpu.total();
        const auto idleNow = current.cpu.idleTotal();
        const auto idleBefore = previous->cpu.idleTotal();
        if (totalNow > totalBefore && idleNow >= idleBefore) {
            const auto totalDelta = totalNow - totalBefore;
            const auto idleDelta = qMin(totalDelta, idleNow - idleBefore);
            const auto kernelNow = current.cpu.system + current.cpu.irq + current.cpu.softIrq;
            const auto kernelBefore = previous->cpu.system + previous->cpu.irq + previous->cpu.softIrq;
            sample.cpuPercent = 100.0 * static_cast<double>(totalDelta - idleDelta) / static_cast<double>(totalDelta);
            if (kernelNow >= kernelBefore) {
                sample.kernelPercent = 100.0 * static_cast<double>(qMin(totalDelta, kernelNow - kernelBefore)) / static_cast<double>(totalDelta);
            }
            sample.cpuReady = true;
        }

        const int coreCount = qMin(current.cpuCores.size(), previous->cpuCores.size());
        sample.cpuCorePercents.reserve(coreCount);
        for (int index = 0; index < coreCount; ++index) {
            double usage = 0.0;
            if (cpuUsage(current.cpuCores.at(index), previous->cpuCores.at(index), usage)) {
                sample.cpuCorePercents.append(qBound(0.0, usage, 100.0));
            }
        }

        const auto elapsedMilliseconds = previous->capturedAt.msecsTo(current.capturedAt);
        if (elapsedMilliseconds > 0) {
            const double elapsedSeconds = static_cast<double>(elapsedMilliseconds) / 1000.0;
            for (const auto &network : current.networks) {
                const auto before = std::find_if(previous->networks.cbegin(), previous->networks.cend(),
                    [&network](const LinuxNetworkUsage &candidate) {
                        return candidate.interfaceName == network.interfaceName;
                    });
                LinuxNetworkRate rate;
                rate.interfaceName = network.interfaceName;
                if (before != previous->networks.cend()) {
                    if (network.receivedBytes >= before->receivedBytes) {
                        rate.receivedBytesPerSecond = static_cast<double>(network.receivedBytes - before->receivedBytes)
                            / elapsedSeconds;
                    }
                    if (network.transmittedBytes >= before->transmittedBytes) {
                        rate.transmittedBytesPerSecond = static_cast<double>(network.transmittedBytes - before->transmittedBytes)
                            / elapsedSeconds;
                    }
                }
                sample.networkRates.append(std::move(rate));
            }
        }
    } else {
        for (const auto &network : current.networks) {
            sample.networkRates.append({network.interfaceName, 0.0, 0.0});
        }
    }
    return sample;
}

} // namespace noxshell
