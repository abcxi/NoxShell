#pragma once

#include <QFrame>
#include <QVector>

class QEnterEvent;
class QGridLayout;
class QLabel;
class QProgressBar;

namespace noxshell::ui {

class MetricCard final : public QFrame {
    Q_OBJECT

public:
    explicit MetricCard(const QString &title, const QColor &accent, QWidget *parent = nullptr);

    void setValue(const QString &value, const QString &detail, int progress);
    void setCoreValues(const QVector<double> &values);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void applyProgressStyle();
    void rebuildCoreRows();
    void setCorePanelVisible(bool visible);
    void updateDetailText();
    QLabel *m_value{};
    QLabel *m_detail{};
    QString m_detailText;
    QProgressBar *m_progress{};
    QFrame *m_corePanel{};
    QGridLayout *m_coreLayout{};
    QVector<QProgressBar *> m_coreProgressBars;
    QVector<QLabel *> m_coreValueLabels;
    QVector<double> m_coreValues;
    QColor m_accent;
};

} // namespace noxshell::ui
