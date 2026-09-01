#pragma once

#include <QRect>
#include <QScrollBar>
#include <QVector>

namespace noxshell::ui {

class SearchMarkerScrollBar final : public QScrollBar {
    Q_OBJECT

public:
    explicit SearchMarkerScrollBar(Qt::Orientation orientation, QWidget *parent = nullptr);

    void setSearchMarkers(const QVector<qreal> &positions, int currentIndex = -1);
    void clearSearchMarkers();
    [[nodiscard]] int searchMarkerCount() const { return m_positions.size(); }
    [[nodiscard]] int currentSearchMarker() const { return m_currentIndex; }
    [[nodiscard]] QRect searchMarkerRect(int index) const;

signals:
    void searchMarkerActivated(int index);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QVector<qreal> m_positions;
    int m_currentIndex{-1};
};

} // namespace noxshell::ui
