#include "MetricCard.h"
#include "AppTheme.h"

#include <QEvent>
#include <QEnterEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>

namespace noxshell::ui {

MetricCard::MetricCard(const QString &title, const QColor &accent, QWidget *parent)
    : QFrame(parent)
    , m_accent(accent)
{
    setObjectName(QStringLiteral("metricRow"));
    setFixedHeight(38);
    setProperty("accent", accent.name());

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *summaryRow = new QWidget;
    summaryRow->setObjectName(QStringLiteral("metricSummaryRow"));
    summaryRow->setFixedHeight(38);
    auto *summaryLayout = new QHBoxLayout(summaryRow);
    summaryLayout->setContentsMargins(10, 6, 10, 6);
    summaryLayout->setSpacing(8);

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
    applyProgressStyle();

    summaryLayout->addWidget(titleLabel);
    summaryLayout->addWidget(m_progress, 1);
    layout->addWidget(summaryRow);

    m_corePanel = new QFrame;
    m_corePanel->setObjectName(QStringLiteral("metricCorePanel"));
    m_corePanel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_coreLayout = new QGridLayout(m_corePanel);
    m_coreLayout->setContentsMargins(10, 3, 10, 7);
    m_coreLayout->setHorizontalSpacing(0);
    m_coreLayout->setVerticalSpacing(3);
    m_corePanel->hide();
    layout->addWidget(m_corePanel);
}

void MetricCard::changeEvent(QEvent *event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) applyProgressStyle();
}

void MetricCard::applyProgressStyle()
{
    if (!m_progress) return;
    const bool dark = isApplicationDarkTheme();
    m_progress->setStyleSheet(QStringLiteral(
        "QProgressBar{border:0;background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        "stop:0 transparent,stop:0.38 transparent,stop:0.39 %1,stop:0.61 %1,stop:0.62 transparent,stop:1 transparent);"
        "color:%2;font-size:10px;font-weight:600;text-align:center;selection-color:%2;}"
        "QProgressBar::chunk{background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        "stop:0 transparent,stop:0.38 transparent,stop:0.39 %3,stop:0.61 %3,stop:0.62 transparent,stop:1 transparent);}")
        .arg(dark ? QStringLiteral("#283440") : QStringLiteral("#EDF1F5"),
            dark ? QStringLiteral("#E2EBF4") : QStringLiteral("#1C324B"), m_accent.name()));

    if (!m_corePanel) return;
    for (auto *progress : m_corePanel->findChildren<QProgressBar *>(QStringLiteral("metricCoreProgress"))) {
        progress->setStyleSheet(QStringLiteral(
            "QProgressBar{border:0;border-radius:2px;background:%1;}"
            "QProgressBar::chunk{border-radius:2px;background:%2;}")
                .arg(dark ? QStringLiteral("#2C3946") : QStringLiteral("#E5EBF1"), m_accent.name()));
    }
}

void MetricCard::setValue(const QString &value, const QString &detail, int progress)
{
    m_progress->setFormat(detail.isEmpty() ? value : QStringLiteral("%1  %2").arg(value, detail));
    m_progress->setToolTip(detail);
    m_progress->setValue(qBound(0, progress, 100));
}

void MetricCard::setCoreValues(const QVector<double> &values)
{
    constexpr int maximumVisibleCores = 16;
    const int visibleCount = qMin(maximumVisibleCores, values.size());
    const bool structureChanged = visibleCount != m_coreProgressBars.size()
        || (values.size() > maximumVisibleCores) != (m_coreValues.size() > maximumVisibleCores);
    m_coreValues = values;
    if (structureChanged) {
        rebuildCoreRows();
    } else {
        for (int index = 0; index < visibleCount; ++index) {
            const double corePercent = m_coreValues.at(index);
            m_coreProgressBars.at(index)->setValue(qBound(0, qRound(corePercent), 100));
            m_coreValueLabels.at(index)->setText(QStringLiteral("%1%").arg(corePercent, 0, 'f', 0));
        }
    }
    if (m_coreValues.isEmpty()) setCorePanelVisible(false);
}

void MetricCard::rebuildCoreRows()
{
    if (!m_coreLayout) return;
    const bool wasVisible = m_corePanel->isVisible();
    while (auto *item = m_coreLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_coreProgressBars.clear();
    m_coreValueLabels.clear();
    constexpr int maximumVisibleCores = 16;
    const int count = qMin(maximumVisibleCores, m_coreValues.size());
    for (int index = 0; index < count; ++index) {
        auto *core = new QWidget;
        core->setObjectName(QStringLiteral("metricCoreRow"));
        auto *layout = new QHBoxLayout(core);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        core->setFixedHeight(14);
        auto *name = new QLabel(QStringLiteral("核心 %1").arg(index + 1));
        name->setObjectName(QStringLiteral("metricCoreName"));
        name->setFixedWidth(38);
        auto *progress = new QProgressBar;
        progress->setObjectName(QStringLiteral("metricCoreProgress"));
        progress->setRange(0, 100);
        progress->setValue(qBound(0, qRound(m_coreValues.at(index)), 100));
        progress->setTextVisible(false);
        progress->setFixedHeight(4);
        auto *value = new QLabel(QStringLiteral("%1%").arg(m_coreValues.at(index), 0, 'f', 0));
        value->setObjectName(QStringLiteral("metricCoreValue"));
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value->setFixedWidth(28);
        layout->addWidget(name);
        layout->addWidget(progress, 1);
        layout->addWidget(value);
        m_coreLayout->addWidget(core, index, 0);
        m_coreProgressBars.append(progress);
        m_coreValueLabels.append(value);
    }
    if (m_coreValues.size() > maximumVisibleCores) {
        auto *more = new QLabel(QStringLiteral("另有 %1 个核心").arg(m_coreValues.size() - maximumVisibleCores));
        more->setObjectName(QStringLiteral("metricCoreMore"));
        more->setAlignment(Qt::AlignCenter);
        m_coreLayout->addWidget(more, count, 0);
    }
    applyProgressStyle();
    m_coreLayout->invalidate();
    m_coreLayout->activate();
    if (wasVisible) setCorePanelVisible(true);
}

void MetricCard::setCorePanelVisible(bool visible)
{
    visible = visible && !m_coreValues.isEmpty();
    m_corePanel->setVisible(visible);
    const int panelHeight = visible ? m_corePanel->sizeHint().height() : 0;
    setFixedHeight(38 + panelHeight);
    updateGeometry();
    if (parentWidget() && parentWidget()->layout()) {
        parentWidget()->layout()->invalidate();
        parentWidget()->layout()->activate();
    }
}

void MetricCard::enterEvent(QEnterEvent *event)
{
    QFrame::enterEvent(event);
    setCorePanelVisible(true);
}

void MetricCard::leaveEvent(QEvent *event)
{
    setCorePanelVisible(false);
    QFrame::leaveEvent(event);
}

} // namespace noxshell::ui
