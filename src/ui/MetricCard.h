#pragma once

#include <QFrame>

class QProgressBar;

namespace noxshell::ui {

class MetricCard final : public QFrame {
    Q_OBJECT

public:
    explicit MetricCard(const QString &title, const QColor &accent, QWidget *parent = nullptr);

    void setValue(const QString &value, const QString &detail, int progress);

private:
    QProgressBar *m_progress{};
};

} // namespace noxshell::ui
