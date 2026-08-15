#pragma once

#include <QFrame>

class QLabel;
class QProgressBar;

namespace noxshell::ui {

class MetricCard final : public QFrame {
    Q_OBJECT

public:
    explicit MetricCard(const QString &title, const QColor &accent, QWidget *parent = nullptr);

    void setValue(const QString &value, const QString &detail, int progress);

private:
    QLabel *m_valueLabel{};
    QLabel *m_detailLabel{};
    QProgressBar *m_progress{};
};

} // namespace noxshell::ui

