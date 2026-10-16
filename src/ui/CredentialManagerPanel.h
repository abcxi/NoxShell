#pragma once
#include "../core/SavedCredential.h"
#include <QWidget>
#include <QVector>
class QTreeWidget;
class QLabel;
class QPushButton;
namespace noxshell { class ServerRepository; class CredentialStore; class SavedCredentialManager; }
namespace noxshell::ui {
class CredentialManagerPanel final : public QWidget {
    Q_OBJECT
public:
    CredentialManagerPanel(ServerRepository *repository, CredentialStore *store, QWidget *parent = nullptr);
    void refresh();
signals:
    void credentialsChanged();
private:
    void edit(bool adding);
    void removeSelected();
    ServerRepository *m_repository;
    SavedCredentialManager *m_manager;
    QTreeWidget *m_list;
    QLabel *m_status;
    QPushButton *m_edit;
    QPushButton *m_remove;
    QVector<SavedCredential> m_entries;
};
}
