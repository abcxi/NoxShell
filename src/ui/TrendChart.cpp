#include "TrendChart.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>

#include <algorithm>

namespace noxshell::ui {

TrendChart::TrendChart(QString title, TrendMetric metric, QColor accent, QWidget *parent)
    : QWidget(parent)
    , m_title(std::move(title))
    , m_metric(metric)
    , m_accent(std::move(accent))
{
    setObjectName(QStringLiteral("trendChart"));
    setMinimumHeight(180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void TrendChart::setSeries(const QVector<MetricHistoryPoint> &points, int windowSeconds)
{
    m_points = points;
    m_windowSeconds = qMax(60, windowSeconds);
    update();
}

void TrendChart::clear()
{
    m_points.clear();
    update();
}

double TrendChart::valueFor(const MetricHistoryPoint &point) const
{
    switch (m_metric) {
    case TrendMetric::Cpu: return point.cpuPercent;
    case TrendMetric::Memory: return point.memoryPercent;
    case TrendMetric::Load: return point.loadPercent;
    case TrendMetric::Disk: return point.diskPercent;
    }
    return 0.0;
}

bool TrendChart::valueIsValid(const MetricHistoryPoint &point) const
{
    return m_metric != TrendMetric::Cpu || point.cpuValid;
}

QString TrendChart::rangeText() const
{
    if (m_windowSeconds < 3600) return QStringLiteral("最近 %1 分钟").arg(m_windowSeconds / 60);
    return QStringLiteral("最近 %1 小时").arg(m_windowSeconds / 3600);
}

void TrendChart::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF outer = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(QColor(QStringLiteral("#E1E7EF")), 1));
    painter.setBrush(Qt::white);
    painter.drawRoundedRect(outer, 5, 5);

    painter.setPen(QColor(QStringLiteral("#52657B")));
    auto titleFont = painter.font();
    titleFont.setPixelSize(12);
    titleFont.setWeight(QFont::DemiBold);
    painter.setFont(titleFont);
    painter.drawText(QRectF(14, 10, width() - 28, 18), Qt::AlignLeft | Qt::AlignVCenter, m_title);

    QVector<double> values;
    values.reserve(m_points.size());
    for (const auto &point : m_points) {
        if (valueIsValid(point)) values.append(valueFor(point));
    }
    const double current = values.isEmpty() ? 0.0 : values.last();
    const double maximum = values.isEmpty() ? 0.0 : *std::max_element(values.cbegin(), values.cend());
    double sum = 0.0;
    for (double value : values) sum += value;
    const double average = values.isEmpty() ? 0.0 : sum / static_cast<double>(values.size());

    auto valueFont = painter.font();
    valueFont.setPixelSize(21);
    valueFont.setWeight(QFont::DemiBold);
    painter.setFont(valueFont);
    painter.setPen(QColor(QStringLiteral("#1C324B")));
    painter.drawText(QRectF(14, 28, 110, 28), Qt::AlignLeft | Qt::AlignVCenter,
        values.isEmpty() ? QStringLiteral("--") : QStringLiteral("%1%").arg(current, 0, 'f', 1));

    auto detailFont = painter.font();
    detailFont.setPixelSize(10);
    detailFont.setWeight(QFont::Normal);
    painter.setFont(detailFont);
    painter.setPen(QColor(QStringLiteral("#8794A5")));
    painter.drawText(QRectF(112, 32, width() - 126, 22), Qt::AlignRight | Qt::AlignVCenter,
        values.isEmpty()
            ? rangeText()
            : QStringLiteral("平均 %1%  ·  峰值 %2%").arg(average, 0, 'f', 1).arg(maximum, 0, 'f', 1));

    const QRectF plot(14, 62, width() - 42, height() - 86);
    painter.setClipRect(plot.adjusted(-1, -1, 1, 1));
    painter.setPen(QPen(QColor(QStringLiteral("#EDF1F5")), 1));
    for (int step = 0; step <= 4; ++step) {
        const qreal y = plot.top() + plot.height() * step / 4.0;
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    for (int step = 0; step <= 3; ++step) {
        const qreal x = plot.left() + plot.width() * step / 3.0;
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }

    if (!m_points.isEmpty()) {
        const auto newest = m_points.last().capturedAt;
        const auto oldestVisible = newest.addSecs(-m_windowSeconds);
        QPainterPath linePath;
        QPainterPath fillPath;
        bool pathStarted = false;
        QPointF firstPoint;
        QPointF lastPoint;
        for (const auto &point : m_points) {
            if (!valueIsValid(point)) continue;
            const auto elapsedMs = oldestVisible.msecsTo(point.capturedAt);
            const double ratio = qBound(0.0, static_cast<double>(elapsedMs) / (m_windowSeconds * 1000.0), 1.0);
            const double value = qBound(0.0, valueFor(point), 100.0);
            const QPointF chartPoint(plot.left() + plot.width() * ratio, plot.bottom() - plot.height() * value / 100.0);
            if (!pathStarted) {
                linePath.moveTo(chartPoint);
                firstPoint = chartPoint;
                pathStarted = true;
            } else {
                linePath.lineTo(chartPoint);
            }
            lastPoint = chartPoint;
        }
        if (pathStarted) {
            fillPath = linePath;
            fillPath.lineTo(lastPoint.x(), plot.bottom());
            fillPath.lineTo(firstPoint.x(), plot.bottom());
            fillPath.closeSubpath();
            QLinearGradient gradient(plot.topLeft(), plot.bottomLeft());
            auto fillColor = m_accent;
            fillColor.setAlpha(60);
            gradient.setColorAt(0.0, fillColor);
            fillColor.setAlpha(4);
            gradient.setColorAt(1.0, fillColor);
            painter.fillPath(fillPath, gradient);
            painter.setPen(QPen(m_accent, 2));
            painter.drawPath(linePath);
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_accent);
            painter.drawEllipse(lastPoint, 3.2, 3.2);
        }
    }
    painter.setClipping(false);

    painter.setFont(detailFont);
    painter.setPen(QColor(QStringLiteral("#9AA7B6")));
    painter.drawText(QRectF(plot.right() + 4, plot.top() - 6, 24, 12), Qt::AlignLeft, QStringLiteral("100"));
    painter.drawText(QRectF(plot.right() + 4, plot.center().y() - 6, 24, 12), Qt::AlignLeft, QStringLiteral("50"));
    painter.drawText(QRectF(plot.right() + 4, plot.bottom() - 6, 24, 12), Qt::AlignLeft, QStringLiteral("0"));
    painter.drawText(QRectF(plot.left(), plot.bottom() + 5, plot.width(), 14), Qt::AlignLeft, rangeText());
    painter.drawText(QRectF(plot.left(), plot.bottom() + 5, plot.width(), 14), Qt::AlignRight, QStringLiteral("现在"));

    if (values.isEmpty()) {
        painter.setPen(QColor(QStringLiteral("#A0ADBA")));
        painter.drawText(plot, Qt::AlignCenter, QStringLiteral("等待采样数据"));
    }
}

} // namespace noxshell::ui
