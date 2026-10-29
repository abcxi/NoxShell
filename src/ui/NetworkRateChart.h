#pragma once

#include <QWidget>
#include <QVector>

namespace noxshell::ui {

struct NetworkRatePoint {
    qint64 timestampMs{};
    double upload{};
    double download{};
};

class NetworkRateChart final : public QWidget {
public:
    explicit NetworkRateChart(QWidget *parent = nullptr);
    void setRates(QVector<NetworkRatePoint> rates, qint64 nowMs);
    void setRateFooter(QWidget *footer);
    [[nodiscard]] const QVector<NetworkRatePoint> &rates() const { return m_rates; }
protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *event) override;
private:
    void layoutFooter();
    QWidget *m_rateFooter{};
    QVector<NetworkRatePoint> m_rates;
    qint64 m_nowMs{};
};

} // namespace noxshell::ui
