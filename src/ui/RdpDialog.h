#pragma once

#include "../core/ServerProfile.h"

#include <QDialog>
#include <QStringList>

class QComboBox;
class QAction;
class QLineEdit;
class QSpinBox;
namespace noxshell { class ServerRepository; }

namespace noxshell::ui {
class CredentialChoice;

class RdpDialog final : public QDialog {
    Q_OBJECT

public:
    explicit RdpDialog(QWidget *parent = nullptr);
    explicit RdpDialog(const ServerProfile &profile, QWidget *parent = nullptr);

    [[nodiscard]] ServerProfile profile() const;
    void setAvailableGroups(const QStringList &groups);
    void setInitialGroup(const QString &group);
    void setConnectionRepository(ServerRepository *repository);

private:
    void populate(const ServerProfile &profile);
    void validateAndAccept();
    void updateCredentialChoice();

    QLineEdit *m_name{};
    QLineEdit *m_host{};
    QSpinBox *m_port{};
    QLineEdit *m_user{};
    QLineEdit *m_password{};
    CredentialChoice *m_credentialChoice{};
    QAction *m_passwordReveal{};
    QComboBox *m_group{};
    ServerProfile m_original;
};

} // namespace noxshell::ui
