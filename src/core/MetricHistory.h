#pragma once

#include "LinuxMetrics.h"

#include <QDateTime>
#include <QVector>

namespace noxshell {

struct MetricHistoryPoint {
    QDateTime capturedAt;
    bool cpuValid{false};
    double cpuPercent{};
    double memoryPercent{};
    double loadPercent{};
    double diskPercent{};
};

class MetricHistory final {
public:
    explicit MetricHistory(qsizetype capacity = 3600);

    void append(const MetricSample &sample);
    void appendPoint(const MetricHistoryPoint &point);
    void clear();

    [[nodiscard]] qsizetype size() const { return m_size; }
    [[nodiscard]] qsizetype capacity() const { return m_buffer.size(); }
    [[nodiscard]] bool isEmpty() const { return m_size == 0; }
    [[nodiscard]] QVector<MetricHistoryPoint> points(int windowSeconds = 3600) const;

private:
    QVector<MetricHistoryPoint> m_buffer;
    qsizetype m_start{};
    qsizetype m_size{};
};

} // namespace noxshell
