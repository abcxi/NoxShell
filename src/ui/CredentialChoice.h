#pragma once
#include "../core/SavedCredential.h"
#include <QComboBox>
#include <QVector>
#include <optional>
namespace noxshell { class ServerRepository; }
namespace noxshell::ui {
class CredentialChoice final : public QComboBox {
public:
    explicit CredentialChoice(QWidget *parent = nullptr);
    void setRepository(ServerRepository *repository, const QString &reference);
    [[nodiscard]] std::optional<SavedCredential> selection() const;
private:
    QVector<SavedCredential> m_entries;
};
}
