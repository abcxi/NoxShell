#include "MetricCard.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>

namespace noxshell::ui {

MetricCard::MetricCard(const QString &title, const QColor &accent, QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("metricRow"));
    setFixedHeight(46);
    setProperty("accent", accent.name());

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(11, 5, 10, 5);
    layout->setSpacing(4);
    auto *summaryRow = new QHBoxLayout;
    summaryRow->setContentsMargins(0, 0, 0, 0);
    summaryRow->setSpacing(7);

    auto *titleLabel = new QLabel(title);
    titleLabel->setObjectName(QStringLiteral("metricTitle"));
    m_valueLabel = new QLabel(QStringLiteral("--"));
    m_valueLabel->setObjectName(QStringLiteral("metricValue"));
    m_detailLabel = new QLabel;
    m_detailLabel->setObjectName(QStringLiteral("metricDetail"));

    m_progress = new QProgressBar;
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(3);
    m_progress->setOrientation(Qt::Horizontal);
    m_progress->setStyleSheet(QStringLiteral(
        "QProgressBar{border:0;background:#EDF1F5;border-radius:1px;}"
        "QProgressBar::chunk{background:%1;border-radius:1px;}")
                                  .arg(accent.name()));

    summaryRow->addWidget(titleLabel);
    summaryRow->addStretch();
    summaryRow->addWidget(m_valueLabel);
    summaryRow->addWidget(m_detailLabel);
    layout->addLayout(summaryRow);
    layout->addWidget(m_progress);
    m_detailLabel->setMaximumWidth(82);
}

void MetricCard::setValue(const QString &value, const QString &detail, int progress)
{
    m_valueLabel->setText(value);
    m_detailLabel->setToolTip(detail);
    m_detailLabel->setText(m_detailLabel->fontMetrics().elidedText(detail, Qt::ElideRight, 82));
    m_progress->setValue(qBound(0, progress, 100));
}

} // namespace noxshell::ui
