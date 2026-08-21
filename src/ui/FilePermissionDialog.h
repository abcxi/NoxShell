#pragma once

#include "../core/RemoteFileEntry.h"

#include <QDialog>

#include <array>

class QCheckBox;
class QRadioButton;

namespace noxshell::ui {

class FilePermissionDialog final : public QDialog {
    Q_OBJECT

public:
    explicit FilePermissionDialog(const RemoteFileEntry &entry, QWidget *parent = nullptr);

    [[nodiscard]] quint32 permissions() const;
    [[nodiscard]] bool recursive() const;
    [[nodiscard]] PermissionScope scope() const;

private:
    std::array<QCheckBox *, 9> m_permissionChecks{};
    QCheckBox *m_recursiveCheck{};
    QRadioButton *m_allRadio{};
    QRadioButton *m_filesRadio{};
    QRadioButton *m_directoriesRadio{};
    quint32 m_specialBits{};
};

} // namespace noxshell::ui
