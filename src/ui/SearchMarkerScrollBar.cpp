#include "SearchMarkerScrollBar.h"

#include <QMouseEvent>
#include <QPainter>

#include <limits>

namespace noxshell::ui {

SearchMarkerScrollBar::SearchMarkerScrollBar(Qt::Orientation orientation, QWidget *parent)
    : QScrollBar(orientation, parent)
{
    setToolTip(QStringLiteral("点击黄色标记跳转到匹配位置"));
}

void SearchMarkerScrollBar::setSearchMarkers(const QVector<qreal> &positions, int currentIndex)
{
    m_positions.clear();
    m_positions.reserve(positions.size());
    for (const qreal position : positions) m_positions.append(qBound<qreal>(0.0, position, 1.0));
    m_currentIndex = currentIndex >= 0 && currentIndex < m_positions.size() ? currentIndex : -1;
    update();
}

void SearchMarkerScrollBar::clearSearchMarkers()
{
    if (m_positions.isEmpty() && m_currentIndex < 0) return;
    m_positions.clear();
    m_currentIndex = -1;
    update();
}

QRect SearchMarkerScrollBar::searchMarkerRect(int index) const
{
    if (index < 0 || index >= m_positions.size()) return {};
    const QRect track = rect().adjusted(1, 2, -1, -2);
    if (track.isEmpty()) return {};
    const int thickness = index == m_currentIndex ? 4 : 3;
    if (orientation() == Qt::Vertical) {
        const int center = track.top() + qRound(m_positions.at(index) * qMax(0, track.height() - 1));
        return QRect(track.left(), qBound(track.top(), center - thickness / 2,
                                           qMax(track.top(), track.bottom() - thickness + 1)),
            track.width(), thickness);
    }
    const int center = track.left() + qRound(m_positions.at(index) * qMax(0, track.width() - 1));
    return QRect(qBound(track.left(), center - thickness / 2,
                     qMax(track.left(), track.right() - thickness + 1)),
        track.top(), thickness, track.height());
}

void SearchMarkerScrollBar::paintEvent(QPaintEvent *event)
{
    QScrollBar::paintEvent(event);
    if (m_positions.isEmpty()) return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    for (int index = 0; index < m_positions.size(); ++index) {
        if (index == m_currentIndex) continue;
        painter.fillRect(searchMarkerRect(index), QColor(QStringLiteral("#FFD84D")));
    }
    if (m_currentIndex >= 0 && m_currentIndex < m_positions.size()) {
        painter.fillRect(searchMarkerRect(m_currentIndex), QColor(QStringLiteral("#FF8A00")));
    }
}

void SearchMarkerScrollBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && !m_positions.isEmpty()) {
        int nearestIndex = -1;
        int nearestDistance = std::numeric_limits<int>::max();
        for (int index = 0; index < m_positions.size(); ++index) {
            const QRect hitRect = searchMarkerRect(index).adjusted(-3, -4, 3, 4);
            if (!hitRect.contains(event->position().toPoint())) continue;
            const int markerCenter = orientation() == Qt::Vertical
                ? searchMarkerRect(index).center().y() : searchMarkerRect(index).center().x();
            const int clickPosition = orientation() == Qt::Vertical
                ? qRound(event->position().y()) : qRound(event->position().x());
            const int distance = qAbs(markerCenter - clickPosition);
            if (distance < nearestDistance) {
                nearestIndex = index;
                nearestDistance = distance;
            }
        }
        if (nearestIndex >= 0) {
            m_currentIndex = nearestIndex;
            update();
            emit searchMarkerActivated(nearestIndex);
            event->accept();
            return;
        }
    }
    QScrollBar::mousePressEvent(event);
}

} // namespace noxshell::ui
