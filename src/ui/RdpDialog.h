#pragma once

#include "../core/ServerProfile.h"

#include <QDialog>
#include <QStringList>

class QComboBox;
class QAction;
class QLineEdit;
class QSpinBox;

namespace noxshell::ui {

class RdpDialog final : public QDialog {
    Q_OBJECT

public:
    explicit RdpDialog(QWidget *parent = nullptr);
    explicit RdpDialog(const ServerProfile &profile, QWidget *parent = nullptr);

    [[nodiscard]] ServerProfile profile() const;
    void setAvailableGroups(const QStringList &groups);
    void setInitialGroup(const QString &group);

private:
    void populate(const ServerProfile &profile);
    void validateAndAccept();

    QLineEdit *m_name{};
    QLineEdit *m_host{};
    QSpinBox *m_port{};
    QLineEdit *m_user{};
    QLineEdit *m_password{};
    QAction *m_passwordReveal{};
    QComboBox *m_group{};
    ServerProfile m_original;
};

} // namespace noxshell::ui
