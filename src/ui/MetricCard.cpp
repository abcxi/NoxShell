#include "MetricCard.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>

namespace noxshell::ui {

MetricCard::MetricCard(const QString &title, const QColor &accent, QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("metricRow"));
    setFixedHeight(38);
    setProperty("accent", accent.name());

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 6, 10, 6);
    layout->setSpacing(8);

    auto *titleLabel = new QLabel(title);
    titleLabel->setObjectName(QStringLiteral("metricTitle"));
    titleLabel->setFixedWidth(34);

    m_progress = new QProgressBar;
    m_progress->setObjectName(QStringLiteral("metricProgress"));
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(true);
    m_progress->setAlignment(Qt::AlignCenter);
    m_progress->setFixedHeight(24);
    m_progress->setOrientation(Qt::Horizontal);
    m_progress->setStyleSheet(QStringLiteral(
        "QProgressBar{border:0;background:#EDF1F5;border-radius:5px;color:#1C324B;"
        "font-size:10px;font-weight:600;text-align:center;}"
        "QProgressBar::chunk{background:%1;border-radius:5px;}")
                                  .arg(accent.name()));

    layout->addWidget(titleLabel);
    layout->addWidget(m_progress, 1);
}

void MetricCard::setValue(const QString &value, const QString &detail, int progress)
{
    m_progress->setFormat(detail.isEmpty() ? value : QStringLiteral("%1  %2").arg(value, detail));
    m_progress->setToolTip(detail);
    m_progress->setValue(qBound(0, progress, 100));
}

} // namespace noxshell::ui
