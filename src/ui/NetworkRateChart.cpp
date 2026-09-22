#include "NetworkRateChart.h"
#include "AppTheme.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace noxshell::ui {
namespace {
QString rateLabel(double value)
{
    const char *units[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int unit = 0;
    while (value >= 1024 && unit < 3) { value /= 1024; ++unit; }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', value < 10 && unit > 0 ? 1 : 0)
        .arg(QString::fromLatin1(units[unit]));
}
}

NetworkRateChart::NetworkRateChart(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("networkRateChart"));
    setFixedHeight(92);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setToolTip(QStringLiteral("最近 60 秒的真实采样；曲线仅做视觉插值，不平均或改变采样值。"));
}

void NetworkRateChart::setRates(QVector<NetworkRatePoint> rates, qint64 nowMs)
{
    m_nowMs = nowMs;
    m_rates.clear();
    for (const auto &point : rates) {
        if (point.timestampMs < nowMs - 60000 || point.timestampMs > nowMs
            || !std::isfinite(point.upload) || !std::isfinite(point.download)
            || point.upload < 0 || point.download < 0) continue;
        if (!m_rates.isEmpty() && point.timestampMs <= m_rates.last().timestampMs) continue;
        m_rates.append(point);
    }
    update();
}

void NetworkRateChart::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool dark = isApplicationDarkTheme();
    const QColor muted(dark ? "#8395A9" : "#8998AA");
    const QRectF plot = QRectF(rect()).adjusted(49, 8, -9, -21);
    if (plot.width() <= 0 || plot.height() <= 0) return;
    double maximum = 1;
    for (const auto &point : m_rates) maximum = std::max({maximum, point.upload, point.download});
    maximum *= 1.12; // Leave space above real peaks, never clip the endpoint.
    auto font = painter.font();
    font.setPixelSize(9);
    font.setWeight(QFont::Normal);
    painter.setFont(font);
    for (int row = 0; row <= 2; ++row) {
        const qreal y = plot.top() + plot.height() * row / 2;
        painter.setPen(QPen(QColor(dark ? "#2B3947" : "#EAF0F6"), 1, Qt::DashLine));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(muted);
        painter.drawText(QRectF(0, y - 6, 42, 12), Qt::AlignRight | Qt::AlignVCenter,
            row == 2 ? QStringLiteral("0") : rateLabel(maximum * (2 - row) / 2));
    }
    const auto series = [&](bool upload, QColor color) {
        if (m_rates.isEmpty()) return;
        QPainterPath path;
        QPointF first, previous;
        bool started = false;
        for (const auto &point : m_rates) {
            const qreal x = plot.right() - plot.width() * (m_nowMs - point.timestampMs) / 60000.0;
            const qreal y = plot.bottom() - plot.height() * (upload ? point.upload : point.download) / maximum;
            const QPointF current(x, y);
            if (!started) { path.moveTo(current); first = current; started = true; }
            else {
                // Both control ordinates are sample values: the curve passes
                // through every sample and cannot invent overshoot/negative rates.
                const qreal middle = (previous.x() + x) / 2;
                path.cubicTo(QPointF(middle, previous.y()), QPointF(middle, y), current);
            }
            previous = current;
        }
        auto area = path;
        area.lineTo(previous.x(), plot.bottom());
        area.lineTo(first.x(), plot.bottom());
        area.closeSubpath();
        QLinearGradient gradient(plot.topLeft(), plot.bottomLeft());
        auto fill = color;
        fill.setAlpha(dark ? 38 : 27);
        gradient.setColorAt(0, fill);
        fill.setAlpha(0);
        gradient.setColorAt(1, fill);
        painter.fillPath(area, gradient);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(path);
        painter.setPen(Qt::NoPen);
        auto halo = color;
        halo.setAlpha(30);
        painter.setBrush(halo);
        painter.drawEllipse(previous, 4, 4);
        painter.setBrush(color);
        painter.drawEllipse(previous, 2, 2);
    };
    series(true, QColor(dark ? "#7BAAFF" : "#427AE3"));
    series(false, QColor(dark ? "#54CDB5" : "#129D86"));
    painter.setPen(muted);
    painter.drawText(QRectF(plot.left(), height() - 16, plot.width(), 14), Qt::AlignLeft, QStringLiteral("60 秒前"));
    painter.drawText(QRectF(plot.left(), height() - 16, plot.width(), 14), Qt::AlignRight, QStringLiteral("现在"));
    if (m_rates.isEmpty()) painter.drawText(plot, Qt::AlignCenter, QStringLiteral("等待采样"));
}

} // namespace noxshell::ui
