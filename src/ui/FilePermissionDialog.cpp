#include "FilePermissionDialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QRadioButton>
#include <QPushButton>
#include <QVBoxLayout>

namespace noxshell::ui {

FilePermissionDialog::FilePermissionDialog(const RemoteFileEntry &entry, QWidget *parent)
    : QDialog(parent)
    , m_specialBits(entry.permissions & 07000U)
{
    setObjectName(QStringLiteral("filePermissionDialog"));
    setWindowTitle(QStringLiteral("修改文件权限"));
    setModal(true);
    setMinimumWidth(330);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(12);

    auto *name = new QLabel(entry.name.isEmpty() ? QFileInfo(entry.path).fileName() : entry.name);
    name->setObjectName(QStringLiteral("permissionFileName"));
    name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(name);

    auto *permissionsBox = new QGroupBox(QStringLiteral("访问权限"));
    auto *grid = new QGridLayout(permissionsBox);
    grid->setContentsMargins(12, 12, 12, 10);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(8);
    grid->addWidget(new QLabel(QStringLiteral("范围")), 0, 0);
    grid->addWidget(new QLabel(QStringLiteral("读取")), 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("写入")), 0, 2);
    grid->addWidget(new QLabel(QStringLiteral("执行")), 0, 3);

    const QStringList rowNames{QStringLiteral("所有者"), QStringLiteral("组"), QStringLiteral("其他")};
    const quint32 masks[] = {0400U, 0200U, 0100U, 0040U, 0020U, 0010U, 0004U, 0002U, 0001U};
    for (int row = 0; row < 3; ++row) {
        grid->addWidget(new QLabel(rowNames.at(row)), row + 1, 0);
        for (int column = 0; column < 3; ++column) {
            const int index = row * 3 + column;
            auto *check = new QCheckBox;
            check->setObjectName(QStringLiteral("permissionCheck_%1_%2").arg(row).arg(column));
            check->setChecked((entry.permissions & masks[index]) != 0U);
            check->setAccessibleName(QStringLiteral("%1%2").arg(rowNames.at(row),
                column == 0 ? QStringLiteral("读取") : column == 1 ? QStringLiteral("写入") : QStringLiteral("执行")));
            m_permissionChecks[index] = check;
            grid->addWidget(check, row + 1, column + 1, Qt::AlignCenter);
        }
    }
    layout->addWidget(permissionsBox);

    if (entry.directory) {
        auto *recursiveBox = new QGroupBox(QStringLiteral("目录应用范围"));
        auto *recursiveLayout = new QVBoxLayout(recursiveBox);
        recursiveLayout->setContentsMargins(12, 10, 12, 10);
        recursiveLayout->setSpacing(7);
        m_recursiveCheck = new QCheckBox(QStringLiteral("递归设置子目录"));
        m_recursiveCheck->setObjectName(QStringLiteral("recursivePermissionCheck"));
        m_allRadio = new QRadioButton(QStringLiteral("应用到文件和目录"));
        m_filesRadio = new QRadioButton(QStringLiteral("只应用到文件"));
        m_directoriesRadio = new QRadioButton(QStringLiteral("只应用到目录"));
        m_allRadio->setObjectName(QStringLiteral("permissionScopeAll"));
        m_filesRadio->setObjectName(QStringLiteral("permissionScopeFiles"));
        m_directoriesRadio->setObjectName(QStringLiteral("permissionScopeDirectories"));
        m_allRadio->setChecked(true);
        for (auto *radio : {m_allRadio, m_filesRadio, m_directoriesRadio}) {
            radio->setEnabled(false);
            recursiveLayout->addWidget(radio);
        }
        recursiveLayout->insertWidget(0, m_recursiveCheck);
        connect(m_recursiveCheck, &QCheckBox::toggled, recursiveBox, [this](bool enabled) {
            m_allRadio->setEnabled(enabled);
            m_filesRadio->setEnabled(enabled);
            m_directoriesRadio->setEnabled(enabled);
        });
        layout->addWidget(recursiveBox);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->setObjectName(QStringLiteral("permissionDialogButtons"));
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

quint32 FilePermissionDialog::permissions() const
{
    const quint32 masks[] = {0400U, 0200U, 0100U, 0040U, 0020U, 0010U, 0004U, 0002U, 0001U};
    quint32 value = m_specialBits;
    for (int index = 0; index < static_cast<int>(m_permissionChecks.size()); ++index) {
        if (m_permissionChecks[index]->isChecked()) value |= masks[index];
    }
    return value;
}

bool FilePermissionDialog::recursive() const
{
    return m_recursiveCheck && m_recursiveCheck->isChecked();
}

PermissionScope FilePermissionDialog::scope() const
{
    if (m_filesRadio && m_filesRadio->isChecked()) return PermissionScope::FilesOnly;
    if (m_directoriesRadio && m_directoriesRadio->isChecked()) return PermissionScope::DirectoriesOnly;
    return PermissionScope::FilesAndDirectories;
}

} // namespace noxshell::ui
