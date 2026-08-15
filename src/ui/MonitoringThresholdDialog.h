#pragma once

#include "../core/MonitoringData.h"

#include <QDialog>

class QDoubleSpinBox;

namespace noxshell::ui {

class MonitoringThresholdDialog final : public QDialog {
    Q_OBJECT

public:
    explicit MonitoringThresholdDialog(const MonitoringThresholds &thresholds, QWidget *parent = nullptr);
    [[nodiscard]] MonitoringThresholds thresholds() const;

private:
    QDoubleSpinBox *m_cpu{};
    QDoubleSpinBox *m_memory{};
    QDoubleSpinBox *m_load{};
    QDoubleSpinBox *m_disk{};
};

} // namespace noxshell::ui
