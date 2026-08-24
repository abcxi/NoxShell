#include "ServerRepository.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

namespace noxshell {

namespace {
QVariant nonNullText(const QString &value)
{
    return QVariant(value.isNull() ? QStringLiteral("") : value);
}

QVector<ServerProfile> defaultDemoServers()
{
    return {
        {QStringLiteral("prod-web-01"), QStringLiteral("10.0.0.11"), QStringLiteral("root"), QStringLiteral("ubuntu"), QStringLiteral("生产环境"), ServerState::Online, 22, ConnectionMode::Demo},
        {QStringLiteral("prod-api-01"), QStringLiteral("10.0.0.12"), QStringLiteral("ops"), QStringLiteral("ubuntu"), QStringLiteral("生产环境"), ServerState::Online, 22, ConnectionMode::Demo},
        {QStringLiteral("db-master-01"), QStringLiteral("10.0.0.21"), QStringLiteral("dba"), QStringLiteral("centos"), QStringLiteral("生产环境"), ServerState::Warning, 22, ConnectionMode::Demo},
        {QStringLiteral("test-web-01"), QStringLiteral("10.0.2.11"), QStringLiteral("dev"), QStringLiteral("debian"), QStringLiteral("测试环境"), ServerState::Online, 22, ConnectionMode::Demo},
        {QStringLiteral("test-api-01"), QStringLiteral("10.0.2.12"), QStringLiteral("dev"), QStringLiteral("ubuntu"), QStringLiteral("测试环境"), ServerState::Online, 22, ConnectionMode::Demo},
    };
}

QString deterministicDemoId(const ServerProfile &profile)
{
    return QStringLiteral("demo-%1").arg(profile.name);
}

QString defaultDatabasePath()
{
    auto directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (directory.isEmpty()) directory = QDir::home().filePath(QStringLiteral(".noxshell-ops"));
    return QDir(directory).filePath(QStringLiteral("noxshell-ops.sqlite3"));
}
} // namespace

ServerRepository::ServerRepository(QString databasePath, bool seedDemoData, QObject *parent)
    : QObject(parent)
    , m_databasePath(databasePath.isEmpty() ? defaultDatabasePath() : std::move(databasePath))
    , m_connectionName(QStringLiteral("noxshell-repository-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
    , m_seedDemoData(seedDemoData)
{
}

ServerRepository::~ServerRepository()
{
    if (m_database.isValid()) m_database.close();
    m_database = {};
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool ServerRepository::initialize()
{
    const QFileInfo info(m_databasePath);
    if (!QDir().mkpath(info.absolutePath())) {
        setError(QStringLiteral("创建数据目录"), info.absolutePath());
        return false;
    }
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(m_databasePath);
    if (!m_database.open()) {
        setError(QStringLiteral("打开 SQLite"), m_database.lastError().text());
        return false;
    }
    QSqlQuery pragma(m_database);
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=3000"));
    return migrate() && seedIfEmpty();
}

bool ServerRepository::migrate()
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS servers ("
            "id TEXT PRIMARY KEY, name TEXT NOT NULL, host TEXT NOT NULL, port INTEGER NOT NULL DEFAULT 22,"
            "user_name TEXT NOT NULL, os TEXT NOT NULL DEFAULT 'linux', group_name TEXT NOT NULL DEFAULT '',"
            "state INTEGER NOT NULL DEFAULT 2, connection_mode INTEGER NOT NULL DEFAULT 1,"
            "authentication INTEGER NOT NULL DEFAULT 0, private_key_path TEXT NOT NULL DEFAULT '',"
            "public_key_path TEXT NOT NULL DEFAULT '', credential_ref TEXT NOT NULL DEFAULT '',"
            "created_at TEXT NOT NULL, updated_at TEXT NOT NULL)"))) {
        setError(QStringLiteral("迁移 servers"), query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS server_groups ("
            "name TEXT PRIMARY KEY, position INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL)"))) {
        setError(QStringLiteral("迁移 server_groups"), query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "INSERT OR IGNORE INTO server_groups(name,position,created_at) "
            "SELECT group_name,rowid,datetime('now') FROM servers WHERE group_name<>''"))) {
        setError(QStringLiteral("整理服务器分组"), query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS known_hosts ("
            "host TEXT NOT NULL, port INTEGER NOT NULL, algorithm TEXT NOT NULL DEFAULT '',"
            "fingerprint TEXT NOT NULL, first_seen_at TEXT NOT NULL, last_seen_at TEXT NOT NULL,"
            "PRIMARY KEY(host, port))"))) {
        setError(QStringLiteral("迁移 known_hosts"), query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS app_state ("
            "key TEXT PRIMARY KEY, value TEXT NOT NULL, updated_at TEXT NOT NULL)"))) {
        setError(QStringLiteral("迁移 app_state"), query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS metric_samples ("
            "server_id TEXT NOT NULL, captured_at INTEGER NOT NULL, cpu_valid INTEGER NOT NULL,"
            "cpu_percent REAL NOT NULL, memory_percent REAL NOT NULL, load_percent REAL NOT NULL,"
            "disk_percent REAL NOT NULL, PRIMARY KEY(server_id,captured_at),"
            "FOREIGN KEY(server_id) REFERENCES servers(id) ON DELETE CASCADE)"))) {
        setError(QStringLiteral("迁移 metric_samples"), query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_metric_samples_server_time ON metric_samples(server_id,captured_at DESC)"))) {
        setError(QStringLiteral("创建指标索引"), query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS monitoring_thresholds ("
            "server_id TEXT PRIMARY KEY, cpu_percent REAL NOT NULL, memory_percent REAL NOT NULL,"
            "load_percent REAL NOT NULL, disk_percent REAL NOT NULL, updated_at TEXT NOT NULL,"
            "FOREIGN KEY(server_id) REFERENCES servers(id) ON DELETE CASCADE)"))) {
        setError(QStringLiteral("迁移 monitoring_thresholds"), query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS monitoring_alerts ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, server_id TEXT NOT NULL, metric TEXT NOT NULL,"
            "metric_value REAL NOT NULL, threshold_value REAL NOT NULL, created_at TEXT NOT NULL,"
            "FOREIGN KEY(server_id) REFERENCES servers(id) ON DELETE CASCADE)"))) {
        setError(QStringLiteral("迁移 monitoring_alerts"), query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS transfer_tasks ("
            "server_id TEXT NOT NULL, task_id INTEGER NOT NULL, operation INTEGER NOT NULL,"
            "local_path TEXT NOT NULL, remote_path TEXT NOT NULL, completed INTEGER NOT NULL, total INTEGER NOT NULL,"
            "state INTEGER NOT NULL, message TEXT NOT NULL, updated_at TEXT NOT NULL,"
            "PRIMARY KEY(server_id,task_id), FOREIGN KEY(server_id) REFERENCES servers(id) ON DELETE CASCADE)"))) {
        setError(QStringLiteral("迁移 transfer_tasks"), query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS login_history ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, server_id TEXT NOT NULL, connected_at TEXT NOT NULL,"
            "FOREIGN KEY(server_id) REFERENCES servers(id) ON DELETE CASCADE)"))) {
        setError(QStringLiteral("迁移登录历史"), query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_login_history_time ON login_history(connected_at DESC,id DESC)"))) {
        setError(QStringLiteral("创建登录历史索引"), query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "DELETE FROM login_history WHERE EXISTS ("
            "SELECT 1 FROM login_history newer WHERE newer.server_id=login_history.server_id AND ("
            "newer.connected_at>login_history.connected_at OR "
            "(newer.connected_at=login_history.connected_at AND newer.id>login_history.id)))"))) {
        setError(QStringLiteral("整理登录历史"), query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_login_history_server ON login_history(server_id)"))) {
        setError(QStringLiteral("创建登录历史主机索引"), query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS command_history ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, server_id TEXT NOT NULL, command TEXT NOT NULL,"
            "note TEXT NOT NULL DEFAULT '', favorite INTEGER NOT NULL DEFAULT 0, executed_at TEXT NOT NULL,"
            "UNIQUE(server_id,command), FOREIGN KEY(server_id) REFERENCES servers(id) ON DELETE CASCADE)"))) {
        setError(QStringLiteral("迁移命令历史"), query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_command_history_server_time "
            "ON command_history(server_id,executed_at DESC,id DESC)"))) {
        setError(QStringLiteral("创建命令历史索引"), query.lastError().text());
        return false;
    }
    return true;
}

bool ServerRepository::seedIfEmpty()
{
    if (!m_seedDemoData) return true;
    QSqlQuery count(m_database);
    if (!count.exec(QStringLiteral("SELECT COUNT(*) FROM servers")) || !count.next()) {
        setError(QStringLiteral("检查种子数据"), count.lastError().text());
        return false;
    }
    if (count.value(0).toInt() > 0) return true;
    for (auto profile : defaultDemoServers()) {
        profile.id = deterministicDemoId(profile);
        if (!saveServer(profile)) return false;
    }
    return true;
}

QVector<ServerProfile> ServerRepository::loadServers()
{
    QVector<ServerProfile> result;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT id,name,host,port,user_name,os,group_name,state,connection_mode,authentication,"
            "private_key_path,public_key_path,credential_ref FROM servers ORDER BY rowid"))) {
        setError(QStringLiteral("读取服务器"), query.lastError().text());
        return result;
    }
    while (query.next()) {
        ServerProfile profile;
        profile.id = query.value(0).toString();
        profile.name = query.value(1).toString();
        profile.host = query.value(2).toString();
        profile.port = static_cast<quint16>(query.value(3).toUInt());
        profile.user = query.value(4).toString();
        profile.os = query.value(5).toString();
        profile.group = query.value(6).toString();
        profile.state = static_cast<ServerState>(query.value(7).toInt());
        profile.connectionMode = static_cast<ConnectionMode>(query.value(8).toInt());
        profile.authentication = static_cast<AuthenticationMethod>(query.value(9).toInt());
        profile.privateKeyPath = query.value(10).toString();
        profile.publicKeyPath = query.value(11).toString();
        profile.credentialRef = query.value(12).toString();
        result.append(profile);
    }
    return result;
}

QStringList ServerRepository::loadServerGroups()
{
    QStringList result;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT name FROM server_groups ORDER BY position,name COLLATE NOCASE"))) {
        setError(QStringLiteral("读取服务器分组"), query.lastError().text());
        return result;
    }
    while (query.next()) {
        const auto name = query.value(0).toString().trimmed();
        if (!name.isEmpty() && !result.contains(name)) result.append(name);
    }
    return result;
}

bool ServerRepository::saveServer(ServerProfile &profile)
{
    if (profile.id.isEmpty()) profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO servers(id,name,host,port,user_name,os,group_name,state,connection_mode,authentication,"
        "private_key_path,public_key_path,credential_ref,created_at,updated_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET name=excluded.name,host=excluded.host,port=excluded.port,"
        "user_name=excluded.user_name,os=excluded.os,group_name=excluded.group_name,state=excluded.state,"
        "connection_mode=excluded.connection_mode,authentication=excluded.authentication,"
        "private_key_path=excluded.private_key_path,public_key_path=excluded.public_key_path,"
        "credential_ref=excluded.credential_ref,updated_at=excluded.updated_at"));
    query.addBindValue(profile.id);
    query.addBindValue(nonNullText(profile.name));
    query.addBindValue(nonNullText(profile.host));
    query.addBindValue(profile.port);
    query.addBindValue(nonNullText(profile.user));
    query.addBindValue(nonNullText(profile.os));
    query.addBindValue(nonNullText(profile.group));
    query.addBindValue(static_cast<int>(profile.state));
    query.addBindValue(static_cast<int>(profile.connectionMode));
    query.addBindValue(static_cast<int>(profile.authentication));
    query.addBindValue(nonNullText(profile.privateKeyPath));
    query.addBindValue(nonNullText(profile.publicKeyPath));
    query.addBindValue(nonNullText(profile.credentialRef));
    query.addBindValue(now);
    query.addBindValue(now);
    if (!query.exec()) {
        setError(QStringLiteral("保存服务器"), query.lastError().text());
        return false;
    }
    if (!profile.group.trimmed().isEmpty() && !saveServerGroup(profile.group.trimmed())) return false;
    return true;
}

bool ServerRepository::saveServerGroup(const QString &name)
{
    const auto normalized = name.trimmed();
    if (normalized.isEmpty()) return false;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO server_groups(name,position,created_at) "
        "VALUES(?,COALESCE((SELECT MAX(position)+1 FROM server_groups),0),?)"));
    query.addBindValue(normalized);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        setError(QStringLiteral("保存服务器分组"), query.lastError().text());
        return false;
    }
    return true;
}

bool ServerRepository::renameServerGroup(const QString &oldName, const QString &newName)
{
    const auto oldNormalized = oldName.trimmed();
    const auto newNormalized = newName.trimmed();
    if (oldNormalized.isEmpty() || newNormalized.isEmpty()) return false;
    if (oldNormalized == newNormalized) return true;
    if (!m_database.transaction()) {
        setError(QStringLiteral("重命名服务器分组"), m_database.lastError().text());
        return false;
    }
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO server_groups(name,position,created_at) "
        "SELECT ?,position,created_at FROM server_groups WHERE name=? "
        "ON CONFLICT(name) DO NOTHING"));
    insert.addBindValue(newNormalized);
    insert.addBindValue(oldNormalized);
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE servers SET group_name=?,updated_at=? WHERE group_name=?"));
    update.addBindValue(newNormalized);
    update.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    update.addBindValue(oldNormalized);
    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral("DELETE FROM server_groups WHERE name=?"));
    remove.addBindValue(oldNormalized);
    if (!insert.exec() || !update.exec() || !remove.exec() || !m_database.commit()) {
        const auto detail = !insert.lastError().text().isEmpty() ? insert.lastError().text()
            : !update.lastError().text().isEmpty() ? update.lastError().text()
            : !remove.lastError().text().isEmpty() ? remove.lastError().text()
                                                  : m_database.lastError().text();
        m_database.rollback();
        setError(QStringLiteral("重命名服务器分组"), detail);
        return false;
    }
    return true;
}

bool ServerRepository::deleteServerGroup(const QString &name)
{
    const auto normalized = name.trimmed();
    if (normalized.isEmpty()) return false;
    if (!m_database.transaction()) {
        setError(QStringLiteral("删除服务器分组"), m_database.lastError().text());
        return false;
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE servers SET group_name='',updated_at=? WHERE group_name=?"));
    update.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    update.addBindValue(normalized);
    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral("DELETE FROM server_groups WHERE name=?"));
    remove.addBindValue(normalized);
    if (!update.exec() || !remove.exec() || !m_database.commit()) {
        const auto detail = !update.lastError().text().isEmpty() ? update.lastError().text()
            : !remove.lastError().text().isEmpty() ? remove.lastError().text()
                                                  : m_database.lastError().text();
        m_database.rollback();
        setError(QStringLiteral("删除服务器分组"), detail);
        return false;
    }
    return true;
}

bool ServerRepository::deleteServer(const QString &id)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM servers WHERE id=?"));
    query.addBindValue(id);
    if (!query.exec()) {
        setError(QStringLiteral("删除服务器"), query.lastError().text());
        return false;
    }
    if (query.numRowsAffected() != 1) {
        setError(QStringLiteral("删除服务器"), QStringLiteral("未找到服务器 %1").arg(id));
        return false;
    }
    return true;
}

bool ServerRepository::saveKnownHost(const QString &host, quint16 port, const QString &algorithm, const QString &fingerprint)
{
    const auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO known_hosts(host,port,algorithm,fingerprint,first_seen_at,last_seen_at) VALUES(?,?,?,?,?,?) "
        "ON CONFLICT(host,port) DO UPDATE SET algorithm=excluded.algorithm,fingerprint=excluded.fingerprint,last_seen_at=excluded.last_seen_at"));
    query.addBindValue(host);
    query.addBindValue(port);
    query.addBindValue(algorithm);
    query.addBindValue(fingerprint);
    query.addBindValue(now);
    query.addBindValue(now);
    if (!query.exec()) {
        setError(QStringLiteral("保存主机指纹"), query.lastError().text());
        return false;
    }
    return true;
}

QString ServerRepository::knownHostFingerprint(const QString &host, quint16 port)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT fingerprint FROM known_hosts WHERE host=? AND port=?"));
    query.addBindValue(host);
    query.addBindValue(port);
    if (!query.exec()) {
        setError(QStringLiteral("读取主机指纹"), query.lastError().text());
        return {};
    }
    return query.next() ? query.value(0).toString() : QString{};
}

TerminalRestoreState ServerRepository::loadTerminalState()
{
    TerminalRestoreState state;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT value FROM app_state WHERE key='terminal_sessions'"));
    if (!query.exec()) {
        setError(QStringLiteral("读取终端恢复状态"), query.lastError().text());
        return state;
    }
    if (!query.next()) return state;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(query.value(0).toByteArray(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(QStringLiteral("读取终端恢复状态"), parseError.errorString());
        return state;
    }
    const auto object = document.object();
    for (const auto value : object.value(QStringLiteral("serverIds")).toArray()) {
        if (value.isString() && !value.toString().isEmpty()) state.serverIds.append(value.toString());
    }
    state.currentIndex = qMax(0, object.value(QStringLiteral("currentIndex")).toInt());
    return state;
}

bool ServerRepository::saveTerminalState(const QStringList &serverIds, int currentIndex)
{
    QJsonArray identifiers;
    for (const auto &serverId : serverIds) identifiers.append(serverId);
    QJsonObject object;
    object.insert(QStringLiteral("serverIds"), identifiers);
    object.insert(QStringLiteral("currentIndex"), qMax(0, currentIndex));
    const auto payload = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO app_state(key,value,updated_at) VALUES('terminal_sessions',?,?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value,updated_at=excluded.updated_at"));
    query.addBindValue(payload);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        setError(QStringLiteral("保存终端恢复状态"), query.lastError().text());
        return false;
    }
    return true;
}

bool ServerRepository::recordSuccessfulLogin(const QString &serverId, const QDateTime &connectedAt)
{
    if (serverId.isEmpty()) return false;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO login_history(server_id,connected_at) VALUES(?,?) "
        "ON CONFLICT(server_id) DO UPDATE SET connected_at="
        "CASE WHEN excluded.connected_at>login_history.connected_at "
        "THEN excluded.connected_at ELSE login_history.connected_at END"));
    query.addBindValue(serverId);
    query.addBindValue((connectedAt.isValid() ? connectedAt : QDateTime::currentDateTime()).toUTC().toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        setError(QStringLiteral("记录成功登录"), query.lastError().text());
        return false;
    }
    QSqlQuery prune(m_database);
    prune.exec(QStringLiteral(
        "DELETE FROM login_history WHERE id NOT IN (SELECT id FROM login_history ORDER BY connected_at DESC,id DESC LIMIT 200)"));
    return true;
}

QVector<LoginHistoryEntry> ServerRepository::loadRecentLogins(int limit)
{
    QVector<LoginHistoryEntry> result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT h.id,h.server_id,s.name,s.host,s.user_name,s.port,h.connected_at "
        "FROM login_history h JOIN servers s ON s.id=h.server_id "
        "ORDER BY h.connected_at DESC,h.id DESC LIMIT ?"));
    query.addBindValue(qBound(1, limit, 100));
    if (!query.exec()) {
        setError(QStringLiteral("读取最近登录"), query.lastError().text());
        return result;
    }
    while (query.next()) {
        LoginHistoryEntry entry;
        entry.id = query.value(0).toLongLong();
        entry.serverId = query.value(1).toString();
        entry.serverName = query.value(2).toString();
        entry.host = query.value(3).toString();
        entry.user = query.value(4).toString();
        entry.port = static_cast<quint16>(query.value(5).toUInt());
        entry.connectedAt = QDateTime::fromString(query.value(6).toString(), Qt::ISODateWithMs).toLocalTime();
        result.append(entry);
    }
    return result;
}

bool ServerRepository::recordCommand(const QString &serverId, const QString &command, const QDateTime &executedAt)
{
    const auto normalized = command.trimmed();
    if (serverId.isEmpty() || normalized.isEmpty()) return false;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO command_history(server_id,command,note,favorite,executed_at) VALUES(?,?,'',0,?) "
        "ON CONFLICT(server_id,command) DO UPDATE SET executed_at=excluded.executed_at"));
    query.addBindValue(serverId);
    query.addBindValue(normalized.left(4096));
    query.addBindValue((executedAt.isValid() ? executedAt : QDateTime::currentDateTime())
                           .toUTC().toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        setError(QStringLiteral("记录命令历史"), query.lastError().text());
        return false;
    }
    QSqlQuery prune(m_database);
    prune.prepare(QStringLiteral(
        "DELETE FROM command_history WHERE server_id=? AND favorite=0 AND id NOT IN ("
        "SELECT id FROM command_history WHERE server_id=? ORDER BY executed_at DESC,id DESC LIMIT 500)"));
    prune.addBindValue(serverId);
    prune.addBindValue(serverId);
    prune.exec();
    return true;
}

QVector<CommandHistoryEntry> ServerRepository::loadCommandHistory(
    const QString &serverId, bool favoritesOnly, int limit)
{
    QVector<CommandHistoryEntry> result;
    if (serverId.isEmpty()) return result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id,server_id,command,note,favorite,executed_at FROM command_history "
        "WHERE server_id=? AND (?=0 OR favorite=1) ORDER BY executed_at DESC,id DESC LIMIT ?"));
    query.addBindValue(serverId);
    query.addBindValue(favoritesOnly ? 1 : 0);
    query.addBindValue(qBound(1, limit, 500));
    if (!query.exec()) {
        setError(QStringLiteral("读取命令历史"), query.lastError().text());
        return result;
    }
    while (query.next()) {
        CommandHistoryEntry entry;
        entry.id = query.value(0).toLongLong();
        entry.serverId = query.value(1).toString();
        entry.command = query.value(2).toString();
        entry.note = query.value(3).toString();
        entry.favorite = query.value(4).toBool();
        entry.executedAt = QDateTime::fromString(query.value(5).toString(), Qt::ISODateWithMs).toLocalTime();
        result.append(entry);
    }
    return result;
}

bool ServerRepository::setCommandFavorite(qint64 id, bool favorite)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE command_history SET favorite=? WHERE id=?"));
    query.addBindValue(favorite ? 1 : 0);
    query.addBindValue(id);
    if (!query.exec() || query.numRowsAffected() != 1) {
        setError(QStringLiteral("更新命令收藏"), query.lastError().text());
        return false;
    }
    return true;
}

bool ServerRepository::setCommandNote(qint64 id, const QString &note)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE command_history SET note=? WHERE id=?"));
    query.addBindValue(note.trimmed().left(500));
    query.addBindValue(id);
    if (!query.exec() || query.numRowsAffected() != 1) {
        setError(QStringLiteral("更新命令备注"), query.lastError().text());
        return false;
    }
    return true;
}

bool ServerRepository::deleteCommandHistory(qint64 id)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM command_history WHERE id=?"));
    query.addBindValue(id);
    if (!query.exec() || query.numRowsAffected() != 1) {
        setError(QStringLiteral("删除命令历史"), query.lastError().text());
        return false;
    }
    return true;
}

bool ServerRepository::clearCommandHistory(const QString &serverId)
{
    QSqlQuery query(m_database);
    // 收藏记录是独立资产；清空普通历史时必须保留收藏及其备注。
    query.prepare(QStringLiteral("DELETE FROM command_history WHERE server_id=? AND favorite=0"));
    query.addBindValue(serverId);
    if (!query.exec()) {
        setError(QStringLiteral("清空命令历史"), query.lastError().text());
        return false;
    }
    return true;
}

bool ServerRepository::clearCommandFavorites(const QString &serverId)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE command_history SET favorite=0 WHERE server_id=? AND favorite=1"));
    query.addBindValue(serverId);
    if (!query.exec()) {
        setError(QStringLiteral("清空命令收藏"), query.lastError().text());
        return false;
    }
    return true;
}

bool ServerRepository::saveMetricSample(const QString &serverId, const MetricSample &sample)
{
    if (serverId.isEmpty()) return false;
    const auto capturedAt = sample.capturedAt.isValid() ? sample.capturedAt : QDateTime::currentDateTime();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO metric_samples(server_id,captured_at,cpu_valid,cpu_percent,memory_percent,load_percent,disk_percent) "
        "VALUES(?,?,?,?,?,?,?)"));
    query.addBindValue(serverId);
    query.addBindValue(capturedAt.toMSecsSinceEpoch());
    query.addBindValue(sample.cpuReady ? 1 : 0);
    query.addBindValue(qBound(0.0, sample.cpuPercent, 100.0));
    query.addBindValue(qBound(0.0, sample.memoryPercent, 100.0));
    query.addBindValue(qBound(0.0, sample.load1 * 100.0 / qMax(1, sample.cpuCoreCount), 100.0));
    query.addBindValue(qBound(0.0, static_cast<double>(sample.primaryDisk.usagePercent), 100.0));
    if (!query.exec()) {
        setError(QStringLiteral("保存指标采样"), query.lastError().text());
        return false;
    }
    if (capturedAt.toSecsSinceEpoch() % 300 == 0) {
        QSqlQuery prune(m_database);
        prune.prepare(QStringLiteral("DELETE FROM metric_samples WHERE captured_at<?"));
        prune.addBindValue(QDateTime::currentDateTimeUtc().addDays(-7).toMSecsSinceEpoch());
        prune.exec();
    }
    return true;
}

QVector<MetricHistoryPoint> ServerRepository::loadMetricHistory(const QString &serverId, const QDateTime &since)
{
    QVector<MetricHistoryPoint> result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT captured_at,cpu_valid,cpu_percent,memory_percent,load_percent,disk_percent "
        "FROM metric_samples WHERE server_id=? AND captured_at>=? ORDER BY captured_at"));
    query.addBindValue(serverId);
    query.addBindValue(since.toMSecsSinceEpoch());
    if (!query.exec()) {
        setError(QStringLiteral("读取指标历史"), query.lastError().text());
        return result;
    }
    while (query.next()) {
        MetricHistoryPoint point;
        point.capturedAt = QDateTime::fromMSecsSinceEpoch(query.value(0).toLongLong());
        point.cpuValid = query.value(1).toBool();
        point.cpuPercent = query.value(2).toDouble();
        point.memoryPercent = query.value(3).toDouble();
        point.loadPercent = query.value(4).toDouble();
        point.diskPercent = query.value(5).toDouble();
        result.append(point);
    }
    return result;
}

MonitoringThresholds ServerRepository::loadMonitoringThresholds(const QString &serverId)
{
    MonitoringThresholds thresholds;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT cpu_percent,memory_percent,load_percent,disk_percent FROM monitoring_thresholds WHERE server_id=?"));
    query.addBindValue(serverId);
    if (!query.exec()) {
        setError(QStringLiteral("读取监控阈值"), query.lastError().text());
        return thresholds;
    }
    if (query.next()) {
        thresholds.cpuPercent = query.value(0).toDouble();
        thresholds.memoryPercent = query.value(1).toDouble();
        thresholds.loadPercent = query.value(2).toDouble();
        thresholds.diskPercent = query.value(3).toDouble();
    }
    return thresholds;
}

bool ServerRepository::saveMonitoringThresholds(const QString &serverId, const MonitoringThresholds &thresholds)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO monitoring_thresholds(server_id,cpu_percent,memory_percent,load_percent,disk_percent,updated_at) "
        "VALUES(?,?,?,?,?,?) ON CONFLICT(server_id) DO UPDATE SET cpu_percent=excluded.cpu_percent,"
        "memory_percent=excluded.memory_percent,load_percent=excluded.load_percent,"
        "disk_percent=excluded.disk_percent,updated_at=excluded.updated_at"));
    query.addBindValue(serverId);
    query.addBindValue(thresholds.cpuPercent);
    query.addBindValue(thresholds.memoryPercent);
    query.addBindValue(thresholds.loadPercent);
    query.addBindValue(thresholds.diskPercent);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        setError(QStringLiteral("保存监控阈值"), query.lastError().text());
        return false;
    }
    return true;
}

bool ServerRepository::recordMonitoringAlert(const MonitoringAlert &alert)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO monitoring_alerts(server_id,metric,metric_value,threshold_value,created_at) VALUES(?,?,?,?,?)"));
    query.addBindValue(alert.serverId);
    query.addBindValue(alert.metric);
    query.addBindValue(alert.value);
    query.addBindValue(alert.threshold);
    query.addBindValue((alert.createdAt.isValid() ? alert.createdAt : QDateTime::currentDateTime()).toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        setError(QStringLiteral("记录监控告警"), query.lastError().text());
        return false;
    }
    return true;
}

QVector<MonitoringAlert> ServerRepository::loadMonitoringAlerts(const QString &serverId, int limit)
{
    QVector<MonitoringAlert> result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id,server_id,metric,metric_value,threshold_value,created_at FROM monitoring_alerts "
        "WHERE server_id=? ORDER BY id DESC LIMIT ?"));
    query.addBindValue(serverId);
    query.addBindValue(qBound(1, limit, 500));
    if (!query.exec()) {
        setError(QStringLiteral("读取监控告警"), query.lastError().text());
        return result;
    }
    while (query.next()) {
        MonitoringAlert alert;
        alert.id = query.value(0).toLongLong();
        alert.serverId = query.value(1).toString();
        alert.metric = query.value(2).toString();
        alert.value = query.value(3).toDouble();
        alert.threshold = query.value(4).toDouble();
        alert.createdAt = QDateTime::fromString(query.value(5).toString(), Qt::ISODateWithMs);
        result.append(alert);
    }
    return result;
}

bool ServerRepository::saveTransferTask(const QString &serverId, const FileTransferTask &task)
{
    if (serverId.isEmpty()) return false;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO transfer_tasks(server_id,task_id,operation,local_path,remote_path,completed,total,state,message,updated_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(server_id,task_id) DO UPDATE SET completed=excluded.completed,"
        "total=excluded.total,state=excluded.state,message=excluded.message,updated_at=excluded.updated_at"));
    query.addBindValue(serverId);
    query.addBindValue(static_cast<qlonglong>(task.id));
    query.addBindValue(static_cast<int>(task.operation));
    query.addBindValue(task.localPath);
    query.addBindValue(task.remotePath);
    query.addBindValue(static_cast<qlonglong>(task.completed));
    query.addBindValue(static_cast<qlonglong>(task.total));
    query.addBindValue(static_cast<int>(task.state));
    query.addBindValue(task.message);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        setError(QStringLiteral("保存传输任务"), query.lastError().text());
        return false;
    }
    return true;
}

QVector<FileTransferTask> ServerRepository::loadTransferTasks(const QString &serverId, int limit)
{
    QVector<FileTransferTask> result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT task_id,operation,local_path,remote_path,completed,total,state,message FROM transfer_tasks "
        "WHERE server_id=? ORDER BY updated_at DESC LIMIT ?"));
    query.addBindValue(serverId);
    query.addBindValue(qBound(1, limit, 500));
    if (!query.exec()) {
        setError(QStringLiteral("读取传输任务"), query.lastError().text());
        return result;
    }
    while (query.next()) {
        FileTransferTask task;
        task.id = query.value(0).toULongLong();
        task.operation = static_cast<RemoteFileOperation>(query.value(1).toInt());
        task.localPath = query.value(2).toString();
        task.remotePath = query.value(3).toString();
        task.completed = query.value(4).toULongLong();
        task.total = query.value(5).toULongLong();
        task.state = static_cast<TransferState>(query.value(6).toInt());
        task.message = query.value(7).toString();
        result.prepend(task);
    }
    return result;
}

bool ServerRepository::clearTransferTasks(const QString &serverId)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM transfer_tasks WHERE server_id=?"));
    query.addBindValue(serverId);
    if (!query.exec()) {
        setError(QStringLiteral("清理传输任务"), query.lastError().text());
        return false;
    }
    return true;
}

void ServerRepository::setError(const QString &context, const QString &detail)
{
    m_lastError = QStringLiteral("%1失败：%2").arg(context, detail);
}

} // namespace noxshell
