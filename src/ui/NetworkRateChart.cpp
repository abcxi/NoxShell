#include "NetworkRateChart.h"
#include "AppTheme.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
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
    setToolTip(QStringLiteral("蓝色实线：上传；橙色虚线：下载。\n最近 60 秒的真实采样；曲线仅做视觉插值，不平均或改变采样值。"));
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
    layoutFooter();
    update();
}

void NetworkRateChart::setRateFooter(QWidget *footer)
{
    m_rateFooter=footer;
    if(footer){footer->setParent(this);footer->show();layoutFooter();}
}

void NetworkRateChart::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layoutFooter();
}

void NetworkRateChart::layoutFooter()
{
    if(!m_rateFooter)return;
    // Preserve the full plot. Rates share the existing time-axis row instead
    // of introducing another line above the graph.
    const int available=qMax(0,width()-78);
    const int fontSize=available<120?9:10;
    const auto style=QStringLiteral("QLabel{font-size:%1px;font-weight:500;}").arg(fontSize);
    if(m_rateFooter->styleSheet()!=style)m_rateFooter->setStyleSheet(style);
    m_rateFooter->setGeometry(43,height()-17,available,15);
}

void NetworkRateChart::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool dark = isApplicationDarkTheme();
    const auto theme=workspaceColors(dark);
    const QColor muted=theme.muted;
    // Keep the scale inside the chart instead of reserving a wide left gutter.
    const QRectF plot = QRectF(rect()).adjusted(5, 7, -6, -20);
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
        painter.setPen(QPen(theme.line, 1, Qt::DashLine));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
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
        QPen stroke(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        if (!upload) stroke.setDashPattern({3.0, 2.0});
        painter.setPen(stroke);
        painter.drawPath(path);
        painter.setPen(Qt::NoPen);
        auto halo = color;
        halo.setAlpha(30);
        painter.setBrush(halo);
        painter.drawEllipse(previous, 4, 4);
        if (upload) {
            painter.setBrush(color);
            painter.drawEllipse(previous, 2, 2);
        } else {
            painter.setPen(QPen(color, 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(previous - QPointF(2.5, 2.5), QSizeF(5, 5)));
        }
    };
    series(true, QColor(dark ? "#7BAAFF" : "#2F6FD6"));
    series(false, QColor(dark ? "#FFB15C" : "#B8600B"));
    // Transparent scale text never masks a line/fill underneath it.
    for (int row = 0; row <= 2; ++row) {
        const QString label = row == 2 ? QStringLiteral("0") : rateLabel(maximum * (2 - row) / 2);
        const qreal y = plot.top() + plot.height() * row / 2;
        const qreal top = row == 0 ? y + 1 : row == 2 ? y - 13 : y - 6;
        const QRectF badge(plot.left() + 1, top, painter.fontMetrics().horizontalAdvance(label) + 6, 12);
        painter.setPen(muted);
        painter.drawText(badge.adjusted(3, 0, -3, 0), Qt::AlignLeft | Qt::AlignVCenter, label);
    }
    painter.setPen(muted);
    painter.drawText(QRectF(plot.left(), height() - 16, plot.width(), 14), Qt::AlignLeft, QStringLiteral("60 秒前"));
    painter.drawText(QRectF(plot.left(), height() - 16, plot.width(), 14), Qt::AlignRight, QStringLiteral("现在"));
    if (m_rates.isEmpty()) painter.drawText(plot, Qt::AlignCenter, QStringLiteral("等待采样"));
}

} // namespace noxshell::ui
