#include "MetricCard.h"
#include "AppTheme.h"

#include <QEvent>
#include <QEnterEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace noxshell::ui {
namespace { constexpr int kSummaryHeight = 44; }

MetricCard::MetricCard(const QString &title, const QColor &accent, QWidget *parent)
    : QFrame(parent)
    , m_accent(accent)
{
    setObjectName(QStringLiteral("metricRow"));
    setFixedHeight(kSummaryHeight);
    setProperty("accent", accent.name());

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *summaryRow = new QWidget;
    summaryRow->setObjectName(QStringLiteral("metricSummaryRow"));
    summaryRow->setFixedHeight(kSummaryHeight);
    auto *summaryLayout = new QVBoxLayout(summaryRow);
    summaryLayout->setContentsMargins(12, 7, 12, 9);
    summaryLayout->setSpacing(3);
    auto *valueRow = new QHBoxLayout;
    valueRow->setSpacing(6);

    auto *titleLabel = new QLabel(title);
    titleLabel->setObjectName(QStringLiteral("metricTitle"));
    titleLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_value = new QLabel(QStringLiteral("--"));
    m_value->setObjectName(QStringLiteral("metricValue"));
    m_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_value->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_detail = new QLabel;
    m_detail->setObjectName(QStringLiteral("metricDetail"));
    m_detail->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_detail->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_detail->setMinimumWidth(0);
    m_detail->installEventFilter(this);
    valueRow->addWidget(titleLabel);
    valueRow->addWidget(m_detail, 1);
    valueRow->addWidget(m_value);
    summaryLayout->addLayout(valueRow);

    m_progress = new QProgressBar;
    m_progress->setObjectName(QStringLiteral("metricProgress"));
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(4);
    m_progress->setOrientation(Qt::Horizontal);
    m_progress->setAccessibleName(title);
    applyProgressStyle();

    summaryLayout->addWidget(m_progress);
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
        "QProgressBar{border:0;border-radius:2px;background:%1;}"
        "QProgressBar::chunk{border:0;border-radius:2px;background:%2;}")
        .arg(dark ? QStringLiteral("#2B3948") : QStringLiteral("#EAF0F6"), m_accent.name()));

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
    m_value->setText(value);
    m_detailText = detail;
    updateDetailText();
    m_progress->setFormat(detail.isEmpty() ? value : QStringLiteral("%1  %2").arg(value, detail));
    setToolTip(m_progress->format());
    m_progress->setAccessibleDescription(m_progress->format());
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
    setFixedHeight(kSummaryHeight + panelHeight);
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

void MetricCard::updateDetailText()
{
    // The value keeps its natural width; only the inline secondary text elides.
    m_detail->setText(m_detail->fontMetrics().elidedText(m_detailText, Qt::ElideRight,
        qMax(0, m_detail->contentsRect().width())));
    m_detail->setToolTip(m_detailText);
    m_detail->setAccessibleName(m_detailText);
}

bool MetricCard::eventFilter(QObject *watched, QEvent *event)
{
    // Values may gain digits without the card itself resizing. Re-elide after
    // layout assigns the detail label its new width, also after font changes.
    if (watched == m_detail && (event->type() == QEvent::Resize || event->type() == QEvent::FontChange))
        updateDetailText();
    return QFrame::eventFilter(watched, event);
}

void MetricCard::resizeEvent(QResizeEvent *event)
{
    QFrame::resizeEvent(event);
    updateDetailText();
}

} // namespace noxshell::ui
