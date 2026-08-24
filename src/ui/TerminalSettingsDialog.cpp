#include "TerminalSettingsDialog.h"

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace noxshell::ui {

TerminalSettingsDialog::TerminalSettingsDialog(const TerminalAppearance &appearance, QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("terminalSettingsDialog"));
    setWindowTitle(QStringLiteral("终端显示设置"));
    setModal(true);
    setMinimumWidth(420);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(14);

    auto *heading = new QLabel(QStringLiteral("SSH 终端外观"));
    heading->setStyleSheet(QStringLiteral("font-size:16px;font-weight:650;"));
    auto *hint = new QLabel(QStringLiteral("设置会同步应用到所有已打开和之后新建的终端标签。"));
    hint->setObjectName(QStringLiteral("mutedLabel"));
    hint->setWordWrap(true);
    layout->addWidget(heading);
    layout->addWidget(hint);

    auto *form = new QFormLayout;
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(11);
    m_fontFamily = new QFontComboBox;
    m_fontFamily->setObjectName(QStringLiteral("terminalFontFamilyCombo"));
    m_fontFamily->setEditable(false);
    m_fontFamily->setFontFilters(QFontComboBox::ScalableFonts);
    m_fontFamily->setMinimumHeight(32);
    m_fontFamily->setMinimumContentsLength(18);
    m_fontFamily->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_fontFamily->setToolTip(QStringLiteral("选择本机系统中已安装的字体"));
    m_fontFamily->setStyleSheet(QStringLiteral(
        "QComboBox{padding-right:32px;}"
        "QComboBox::drop-down{width:30px;border-left:1px solid #E1E7EF;}"
        "QComboBox::down-arrow{image:url(:/assets/chevron-down.svg);width:12px;height:12px;}"));
    m_fontFamily->setCurrentFont(QFont(appearance.fontFamily));
    m_fontSize = new QSpinBox;
    m_fontSize->setObjectName(QStringLiteral("terminalFontSizeSpin"));
    m_fontSize->setRange(8, 32);
    m_fontSize->setSuffix(QStringLiteral(" pt"));
    m_fontSize->setValue(appearance.pointSize);
    m_lineSpacing = new QDoubleSpinBox;
    m_lineSpacing->setObjectName(QStringLiteral("terminalLineSpacingSpin"));
    m_lineSpacing->setRange(1.0, 2.0);
    m_lineSpacing->setDecimals(2);
    m_lineSpacing->setSingleStep(0.05);
    m_lineSpacing->setSuffix(QStringLiteral(" 倍"));
    m_lineSpacing->setValue(appearance.lineSpacing);
    form->addRow(QStringLiteral("系统字体"), m_fontFamily);
    form->addRow(QStringLiteral("字号"), m_fontSize);
    form->addRow(QStringLiteral("行间距"), m_lineSpacing);
    layout->addLayout(form);

    m_preview = new QLabel(QStringLiteral("root@noxshell:~$ ls -la\nSSH 终端字体预览  Aa  0123456789"));
    m_preview->setObjectName(QStringLiteral("terminalAppearancePreview"));
    m_preview->setMinimumHeight(86);
    m_preview->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_preview->setStyleSheet(QStringLiteral(
        "background:#0C1825;color:#D0DBE5;border:1px solid #203349;border-radius:5px;padding:10px 12px;"));
    layout->addWidget(m_preview);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->setObjectName(QStringLiteral("terminalSettingsButtons"));
    auto *reset = buttons->addButton(QStringLiteral("恢复默认"), QDialogButtonBox::ResetRole);
    reset->setObjectName(QStringLiteral("terminalSettingsResetButton"));
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);

    connect(m_fontFamily, &QFontComboBox::currentFontChanged, this, [this] { refreshPreview(); });
    connect(m_fontSize, &QSpinBox::valueChanged, this, [this] { refreshPreview(); });
    connect(m_lineSpacing, &QDoubleSpinBox::valueChanged, this, [this] { refreshPreview(); });
    connect(reset, &QPushButton::clicked, this, &TerminalSettingsDialog::restoreDefaults);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    refreshPreview();
}

TerminalAppearance TerminalSettingsDialog::appearance() const
{
    return {m_fontFamily->currentFont().family(), m_fontSize->value(), m_lineSpacing->value()};
}

void TerminalSettingsDialog::refreshPreview()
{
    const auto value = appearance();
    auto font = m_preview->font();
    font.setFamily(value.fontFamily);
    font.setPointSize(value.pointSize);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    m_preview->setFont(font);
    m_preview->setMinimumHeight(qMax(86, qRound(QFontMetricsF(font).height() * value.lineSpacing * 3.2)));
    emit appearancePreviewRequested(value.fontFamily, value.pointSize, value.lineSpacing);
}

void TerminalSettingsDialog::restoreDefaults()
{
    const auto fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_fontFamily->setCurrentFont(fixedFont);
    m_fontSize->setValue(12);
    m_lineSpacing->setValue(1.05);
    refreshPreview();
}

} // namespace noxshell::ui
