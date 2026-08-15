#include "MonitoringThresholdDialog.h"

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace noxshell::ui {

MonitoringThresholdDialog::MonitoringThresholdDialog(const MonitoringThresholds &thresholds, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("监控告警阈值"));
    setModal(true);
    setMinimumWidth(390);
    auto *layout = new QVBoxLayout(this);
    auto *hint = new QLabel(QStringLiteral("指标达到阈值时记录一次告警；回落 3% 后允许再次触发。"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *form = new QFormLayout;
    auto createSpin = [](double value) {
        auto *spin = new QDoubleSpinBox;
        spin->setRange(1.0, 100.0);
        spin->setDecimals(1);
        spin->setSuffix(QStringLiteral(" %"));
        spin->setValue(value);
        return spin;
    };
    m_cpu = createSpin(thresholds.cpuPercent);
    m_memory = createSpin(thresholds.memoryPercent);
    m_load = createSpin(thresholds.loadPercent);
    m_disk = createSpin(thresholds.diskPercent);
    form->addRow(QStringLiteral("CPU 使用率"), m_cpu);
    form->addRow(QStringLiteral("内存使用率"), m_memory);
    form->addRow(QStringLiteral("系统负载 / CPU 容量"), m_load);
    form->addRow(QStringLiteral("根磁盘使用率"), m_disk);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

MonitoringThresholds MonitoringThresholdDialog::thresholds() const
{
    return {m_cpu->value(), m_memory->value(), m_load->value(), m_disk->value()};
}

} // namespace noxshell::ui
