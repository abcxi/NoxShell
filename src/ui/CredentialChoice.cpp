#include "CredentialChoice.h"
#include "../core/ServerRepository.h"
#include <QSignalBlocker>
namespace noxshell::ui {
CredentialChoice::CredentialChoice(QWidget *parent) : QComboBox(parent)
{
    setObjectName(QStringLiteral("credentialChoice"));
    setMinimumHeight(32);
    addItem(QStringLiteral("手动输入 / 此连接已保存的密码"), QString{});
}
void CredentialChoice::setRepository(ServerRepository *repository, const QString &reference)
{
    const QSignalBlocker blocker(this);
    clear();
    addItem(QStringLiteral("手动输入 / 此连接已保存的密码"), QString{});
    m_entries = repository ? repository->loadSavedCredentials() : QVector<SavedCredential>{};
    for (const auto &entry : m_entries) addItem(QStringLiteral("%1 · %2").arg(entry.name, entry.user), entry.credentialRef);
    const int index = findData(reference);
    setCurrentIndex(index < 0 ? 0 : index);
}
std::optional<SavedCredential> CredentialChoice::selection() const
{
    const auto ref = currentData().toString();
    for (const auto &entry : m_entries) if (entry.credentialRef == ref) return entry;
    return std::nullopt;
}
}
