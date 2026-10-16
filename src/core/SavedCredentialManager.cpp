#include "SavedCredentialManager.h"
#include "ServerRepository.h"
#include "CredentialStore.h"
#include <QUuid>

namespace noxshell {
SavedCredentialManager::SavedCredentialManager(ServerRepository *repository, CredentialStore *store, QObject *parent)
    : QObject(parent), m_repository(repository), m_store(store) {}

bool SavedCredentialManager::save(SavedCredential &value, const QString &password, const SavedCredential *expected)
{
    m_error.clear();
    if (!m_repository || !m_store) { m_error = QStringLiteral("密码管理暂不可用"); return false; }
    auto candidate = value;
    candidate.name = candidate.name.trimmed();
    candidate.user = candidate.user.trimmed();
    if (candidate.name.isEmpty() || candidate.user.isEmpty() || (!expected && password.isEmpty())) {
        m_error = QStringLiteral("名称、用户名和新建账密的密码不能为空"); return false;
    }
    if (expected && candidate.id != expected->id) { m_error = QStringLiteral("账密记录不匹配"); return false; }
    if (!expected) candidate.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const bool newSecret = !password.isEmpty();
    candidate.credentialRef = newSecret
        ? QStringLiteral("managed/%1/%2").arg(candidate.id, QUuid::createUuid().toString(QUuid::WithoutBraces))
        : expected->credentialRef;
    if (newSecret && !m_store->save(candidate.credentialRef, {password, {}})) {
        m_error = m_store->lastError(); return false;
    }
    if (!m_repository->saveSavedCredential(candidate, expected)) {
        m_error = m_repository->lastError();
        if (newSecret) m_store->remove(candidate.credentialRef);
        return false;
    }
    // Commit references before retiring the old version. Existing channels
    // keep their authenticated session; future connects use the new version.
    if (newSecret && expected && candidate.credentialRef != expected->credentialRef
        && !m_store->remove(expected->credentialRef))
        m_error = QStringLiteral("账密已保存；旧加密条目清理失败：%1").arg(m_store->lastError());
    value = candidate;
    emit changed();
    return true;
}

bool SavedCredentialManager::remove(const SavedCredential &value)
{
    m_error.clear();
    if (!m_repository || !m_store) { m_error = QStringLiteral("密码管理暂不可用"); return false; }
    if (!m_repository->deleteSavedCredential(value)) { m_error = m_repository->lastError(); return false; }
    if (!m_store->remove(value.credentialRef))
        m_error = QStringLiteral("账密已从列表移除；加密条目清理失败：%1").arg(m_store->lastError());
    emit changed();
    return true;
}
}
