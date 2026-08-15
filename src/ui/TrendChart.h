#pragma once

#include "../core/MetricHistory.h"

#include <QColor>
#include <QWidget>

namespace noxshell::ui {

enum class TrendMetric {
    Cpu,
    Memory,
    Load,
    Disk,
};

class TrendChart final : public QWidget {
    Q_OBJECT

public:
    TrendChart(QString title, TrendMetric metric, QColor accent, QWidget *parent = nullptr);

    void setSeries(const QVector<MetricHistoryPoint> &points, int windowSeconds);
    void clear();

    [[nodiscard]] qsizetype pointCount() const { return m_points.size(); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    [[nodiscard]] double valueFor(const MetricHistoryPoint &point) const;
    [[nodiscard]] bool valueIsValid(const MetricHistoryPoint &point) const;
    [[nodiscard]] QString rangeText() const;

    QString m_title;
    TrendMetric m_metric;
    QColor m_accent;
    QVector<MetricHistoryPoint> m_points;
    int m_windowSeconds{900};
};

} // namespace noxshell::ui
