#pragma once

#include "TerminalView.h"

#include <QDialog>

class QDoubleSpinBox;
class QCheckBox;
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
    void appearancePreviewRequested(const QString &fontFamily, int pointSize, double lineSpacing, bool autoEnglishInput);

private:
    void refreshPreview();
    void restoreDefaults();

    QFontComboBox *m_fontFamily{};
    QSpinBox *m_fontSize{};
    QDoubleSpinBox *m_lineSpacing{};
    QCheckBox *m_autoEnglishInput{};
    QLabel *m_preview{};
};

} // namespace noxshell::ui
