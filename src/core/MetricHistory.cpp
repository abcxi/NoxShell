#include "MetricHistory.h"

namespace noxshell {

MetricHistory::MetricHistory(qsizetype capacity)
    : m_buffer(qMax<qsizetype>(1, capacity))
{
}

void MetricHistory::append(const MetricSample &sample)
{
    MetricHistoryPoint point;
    point.capturedAt = sample.capturedAt.isValid() ? sample.capturedAt : QDateTime::currentDateTime();
    point.cpuValid = sample.cpuReady;
    point.cpuPercent = qBound(0.0, sample.cpuPercent, 100.0);
    point.memoryPercent = qBound(0.0, sample.memoryPercent, 100.0);
    point.loadPercent = qBound(0.0, sample.load1 * 100.0 / qMax(1, sample.cpuCoreCount), 100.0);
    point.diskPercent = qBound(0.0, static_cast<double>(sample.primaryDisk.usagePercent), 100.0);

    appendPoint(point);
}

void MetricHistory::appendPoint(const MetricHistoryPoint &point)
{
    if (m_size < m_buffer.size()) {
        m_buffer[(m_start + m_size) % m_buffer.size()] = point;
        ++m_size;
        return;
    }
    m_buffer[m_start] = point;
    m_start = (m_start + 1) % m_buffer.size();
}

void MetricHistory::clear()
{
    m_start = 0;
    m_size = 0;
}

QVector<MetricHistoryPoint> MetricHistory::points(int windowSeconds) const
{
    QVector<MetricHistoryPoint> result;
    if (m_size == 0) return result;

    const auto newest = m_buffer[(m_start + m_size - 1) % m_buffer.size()].capturedAt;
    const auto cutoff = newest.addSecs(-qMax(1, windowSeconds));
    result.reserve(m_size);
    for (qsizetype offset = 0; offset < m_size; ++offset) {
        const auto &point = m_buffer[(m_start + offset) % m_buffer.size()];
        if (point.capturedAt >= cutoff) result.append(point);
    }
    return result;
}

} // namespace noxshell
