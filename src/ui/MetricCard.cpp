#include "MetricCard.h"

#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>

namespace noxshell::ui {

MetricCard::MetricCard(const QString &title, const QColor &accent, QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("metricCard"));
    setMinimumHeight(98);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(13, 11, 13, 10);
    layout->setSpacing(3);

    auto *titleLabel = new QLabel(title);
    titleLabel->setObjectName(QStringLiteral("metricTitle"));
    m_valueLabel = new QLabel(QStringLiteral("--"));
    m_valueLabel->setObjectName(QStringLiteral("metricValue"));
    m_detailLabel = new QLabel;
    m_detailLabel->setObjectName(QStringLiteral("metricDetail"));

    m_progress = new QProgressBar;
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(4);
    m_progress->setStyleSheet(QStringLiteral(
        "QProgressBar{border:0;background:#EDF1F5;border-radius:2px;}"
        "QProgressBar::chunk{background:%1;border-radius:2px;}")
                                  .arg(accent.name()));

    layout->addWidget(titleLabel);
    layout->addWidget(m_valueLabel);
    layout->addWidget(m_detailLabel);
    layout->addSpacing(3);
    layout->addWidget(m_progress);
}

void MetricCard::setValue(const QString &value, const QString &detail, int progress)
{
    m_valueLabel->setText(value);
    m_detailLabel->setText(detail);
    m_progress->setValue(qBound(0, progress, 100));
}

} // namespace noxshell::ui

