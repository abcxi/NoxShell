#pragma once

#include "../core/ServerProfile.h"

#include <QDialog>
#include <QStringList>

class QComboBox;
class QAction;
class QLineEdit;
class QSpinBox;
class QStackedWidget;
class QPushButton;
class QLabel;

namespace noxshell {
class CredentialStore;
class ServerRepository;
}

namespace noxshell::ui {

class ServerDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ServerDialog(QWidget *parent = nullptr);
    explicit ServerDialog(const ServerProfile &profile, QWidget *parent = nullptr);

    [[nodiscard]] ServerProfile profile() const;
    void setConnectionServices(ServerRepository *repository, CredentialStore *credentialStore);
    void setAvailableGroups(const QStringList &groups);
    void setInitialGroup(const QString &group);

private:
    void updateAuthenticationPage();
    void choosePrivateKey();
    void validateAndAccept();
    void testConnection();
    void updatePasswordHint();
    void normalizeCredentialInput(QLineEdit *editor, const QString &text, bool showPasswordHint);
    [[nodiscard]] bool validateProfile(const ServerProfile &profile, bool allowStoredPassword);
    [[nodiscard]] bool prepareConnectionTestProfile(ServerProfile &profile);
    void populate(const ServerProfile &profile);

    QLineEdit *m_name{};
    QLineEdit *m_host{};
    QSpinBox *m_port{};
    QLineEdit *m_user{};
    QComboBox *m_group{};
    QLineEdit *m_os{};
    QComboBox *m_authentication{};
    QStackedWidget *m_authPages{};
    QLineEdit *m_password{};
    QAction *m_passwordReveal{};
    QLabel *m_passwordHint{};
    QLineEdit *m_privateKey{};
    QLineEdit *m_publicKey{};
    QLineEdit *m_passphrase{};
    QLineEdit *m_fingerprint{};
    QPushButton *m_testButton{};
    QLabel *m_testStatus{};
    ServerRepository *m_repository{};
    CredentialStore *m_credentialStore{};
    ServerProfile m_original;
    bool m_editing{false};
    bool m_testUsesStoredPassword{false};
};

} // namespace noxshell::ui
