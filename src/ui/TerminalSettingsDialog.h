#pragma once

#include "TerminalView.h"

#include <QDialog>

class QDoubleSpinBox;
class QFontComboBox;
class QLabel;
class QSpinBox;

namespace noxshell::ui {

class TerminalSettingsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit TerminalSettingsDialog(const TerminalAppearance &appearance, QWidget *parent = nullptr);

    [[nodiscard]] TerminalAppearance appearance() const;

signals:
    void appearancePreviewRequested(const QString &fontFamily, int pointSize, double lineSpacing);

private:
    void refreshPreview();
    void restoreDefaults();

    QFontComboBox *m_fontFamily{};
    QSpinBox *m_fontSize{};
    QDoubleSpinBox *m_lineSpacing{};
    QLabel *m_preview{};
};

} // namespace noxshell::ui
