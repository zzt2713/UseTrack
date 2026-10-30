#include "data/backup_service.h"

#include "data/database.h"
#include "data/schema.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStringList>
#include <QUuid>
#include <QVariant>

namespace patina::data
{
namespace
{

QString describeError(const QSqlQuery& query)
{
    const QSqlError error = query.lastError();
    return error.isValid() ? error.text() : QStringLiteral("SQL 执行失败");
}

// 拼进 SQL 字面量前把单引号打成两个：Windows 路径里出现 ' 不是不可能。
QString quoteLiteral(const QString& raw)
{
    QString escaped = raw;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QStringLiteral("'%1'").arg(escaped);
}

// 一条只读探测连接，专供校验备份文件用。
// 用完必须关掉并移除：QSqlDatabase 的连接名是全局的，泄漏会一直留着。
class ProbeConnection
{
public:
    ProbeConnection(const QString& filePath, bool readOnly)
    {
        _name = QStringLiteral("patina-probe-%1")
                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), _name);
        database.setDatabaseName(filePath);
        if (readOnly) {
            database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        }
        _open = database.open();
        if (!_open) {
            _error = database.lastError().text();
        }
    }

    ~ProbeConnection()
    {
        {
            QSqlDatabase database = QSqlDatabase::database(_name, false);
            if (database.isOpen()) {
                database.close();
            }
        }
        QSqlDatabase::removeDatabase(_name);
    }

    ProbeConnection(const ProbeConnection&) = delete;
    ProbeConnection& operator=(const ProbeConnection&) = delete;

    bool isOpen() const { return _open; }
    QString error() const { return _error; }
    QSqlDatabase connection() const { return QSqlDatabase::database(_name, false); }

private:
    QString _name;
    QString _error;
    bool _open = false;
};

// 读一个库的 schema 版本。表不存在（不是本程序的库，或文件根本不是 SQLite）时返回 -1。
int readSchemaVersion(QSqlDatabase connection)
{
    QSqlQuery table(connection);
    if (!table.exec(QStringLiteral(
            "SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'schema_migrations'"))) {
        return -1;
    }
    if (!table.next()) {
        return -1;
    }

    QSqlQuery version(connection);
    if (!version.exec(QStringLiteral("SELECT COALESCE(MAX(version), 0) FROM schema_migrations"))) {
        return -1;
    }
    if (!version.next()) {
        return -1;
    }
    return version.value(0).toInt();
}

qint64 countRows(QSqlDatabase connection, const QString& table)
{
    QSqlQuery query(connection);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(table))) {
        return -1;
    }
    if (!query.next()) {
        return -1;
    }
    return query.value(0).toLongLong();
}

QString formatMegabytes(qint64 bytes)
{
    return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
}

} // namespace

QString BackupService::defaultBackupDirectory()
{
    // 放到「文档」下面而不是 AppData：备份是给用户拿去存档的，
    // 藏在 %APPDATA% 里等于让人找不到。
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (documents.isEmpty()) {
        return QString();
    }
    return documents + QStringLiteral("/UseTrack/backups");
}

QString BackupService::defaultBackupFileName(qint64 nowMs)
{
    const QDateTime now = QDateTime::fromMSecsSinceEpoch(nowMs);
    return QStringLiteral("UseTrack-backup-%1.db").arg(now.toString(QStringLiteral("yyyyMMdd-HHmmss")));
}

Status BackupService::exportTo(const QString& targetPath) const
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    const QString target = targetPath.trimmed();
    if (target.isEmpty()) {
        return Status::failure(QStringLiteral("备份路径为空"));
    }

    const QFileInfo info(target);
    const QDir parent = info.absoluteDir();
    if (!parent.exists() && !QDir().mkpath(parent.absolutePath())) {
        return Status::failure(QStringLiteral("无法创建备份目录：%1").arg(parent.absolutePath()));
    }

    // VACUUM INTO 生成的是一个整理过的单文件快照：
    // 不走它而直接复制 .db，WAL 里还没合并的页会丢，刚记下的活动可能整段消失。
    // 它要求目标文件不存在，所以先写临时文件再换名。
    const QString temporary =
        target + QStringLiteral(".tmp-%1").arg(QCoreApplication::applicationPid());
    QFile::remove(temporary);

    {
        QSqlQuery vacuum(_database.connection());
        if (!vacuum.exec(QStringLiteral("VACUUM INTO %1").arg(quoteLiteral(temporary)))) {
            const QString message = describeError(vacuum);
            QFile::remove(temporary);
            return Status::failure(QStringLiteral("导出备份失败：%1").arg(message));
        }
    }

    // 覆盖同名文件：Windows 的 rename 不覆盖，只能先删。
    if (QFile::exists(target) && !QFile::remove(target)) {
        QFile::remove(temporary);
        return Status::failure(QStringLiteral("无法覆盖已有文件：%1").arg(target));
    }
    if (!QFile::rename(temporary, target)) {
        QFile::remove(temporary);
        return Status::failure(QStringLiteral("无法写入备份文件：%1").arg(target));
    }
    return Status::success();
}

Result<BackupInspection> BackupService::inspect(const QString& backupPath) const
{
    BackupInspection inspection;

    const QString path = backupPath.trimmed();
    if (path.isEmpty()) {
        inspection.message = QStringLiteral("还没有选择备份文件");
        return succeed(inspection);
    }

    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        inspection.message = QStringLiteral("文件不存在：%1").arg(path);
        return succeed(inspection);
    }
    if (info.size() <= 0) {
        inspection.message = QStringLiteral("文件是空的");
        return succeed(inspection);
    }
    inspection.sizeBytes = info.size();

    ProbeConnection probe(path, true);
    if (!probe.isOpen()) {
        inspection.message = QStringLiteral("无法打开这个文件（不是有效的 SQLite 数据库）：%1")
                                 .arg(probe.error());
        return succeed(inspection);
    }

    const QSqlDatabase connection = probe.connection();

    // 完整性检查：文件头合法不代表页没坏，SQLite 自带这个命令专门用来确认。
    {
        QSqlQuery check(connection);
        if (!check.exec(QStringLiteral("PRAGMA integrity_check"))) {
            inspection.message = QStringLiteral("完整性检查失败：%1").arg(describeError(check));
            return succeed(inspection);
        }
        if (!check.next() || check.value(0).toString() != QStringLiteral("ok")) {
            const QString detail = check.isValid() ? check.value(0).toString() : QString();
            inspection.message = detail.isEmpty()
                ? QStringLiteral("备份文件已损坏（完整性检查未通过）")
                : QStringLiteral("备份文件已损坏：%1").arg(detail);
            return succeed(inspection);
        }
    }

    const int version = readSchemaVersion(connection);
    if (version < 0) {
        inspection.message = QStringLiteral("这不像本程序的备份文件（找不到 schema 版本表）");
        return succeed(inspection);
    }
    inspection.schemaVersion = version;

    if (version > latestSchemaVersion()) {
        inspection.message = QStringLiteral("备份来自更新的版本（schema v%1，本程序支持到 v%2）")
                                 .arg(version)
                                 .arg(latestSchemaVersion());
        return succeed(inspection);
    }
    if (version < latestSchemaVersion()) {
        inspection.message = QStringLiteral("备份来自更旧的版本（schema v%1，本程序需要 v%2）")
                                 .arg(version)
                                 .arg(latestSchemaVersion());
        return succeed(inspection);
    }

    inspection.appCount = countRows(connection, QStringLiteral("apps"));
    inspection.sessionCount = countRows(connection, QStringLiteral("sessions"));
    inspection.settingCount = countRows(connection, QStringLiteral("settings"));
    if (inspection.appCount < 0 || inspection.sessionCount < 0 || inspection.settingCount < 0) {
        inspection.message = QStringLiteral("备份缺少必要的数据表");
        return succeed(inspection);
    }

    inspection.restorable = true;
    inspection.message = QStringLiteral("可以恢复：schema v%1，%2 个活动段 / %3 个应用，%4")
                             .arg(version)
                             .arg(inspection.sessionCount)
                             .arg(inspection.appCount)
                             .arg(formatMegabytes(inspection.sizeBytes));
    return succeed(inspection);
}

Status BackupService::restoreFrom(const QString& backupPath)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    // 破坏性操作前先校验。不通过就一步都不做。
    const Result<BackupInspection> inspected = inspect(backupPath);
    if (!inspected.ok()) {
        return Status::failure(inspected.error);
    }
    if (!inspected.value.restorable) {
        return Status::failure(inspected.value.message);
    }

    const QString path = QFileInfo(backupPath.trimmed()).absoluteFilePath();
    const QSqlDatabase connection = _database.connection();

    // ATTACH 不能放在事务里，所以先挂上、再开事务。
    {
        QSqlQuery attach(connection);
        if (!attach.exec(QStringLiteral("ATTACH DATABASE %1 AS backup_source").arg(quoteLiteral(path)))) {
            return Status::failure(QStringLiteral("无法挂载备份文件：%1").arg(describeError(attach)));
        }
    }

    auto detach = [&connection]() {
        QSqlQuery detachQuery(connection);
        detachQuery.exec(QStringLiteral("DETACH DATABASE backup_source"));
    };

    Status result = Status::success();
    {
        Transaction transaction(_database);
        if (!transaction.isActive()) {
            detach();
            return Status::failure(QStringLiteral("无法开启恢复事务"));
        }

        // 先记下备份里的条目数，写完再核一遍：数量对不上说明中途有行被约束挡掉了。
        const qint64 expectedApps = countRows(connection, QStringLiteral("backup_source.apps"));
        const qint64 expectedSessions = countRows(connection, QStringLiteral("backup_source.sessions"));
        const qint64 expectedSettings = countRows(connection, QStringLiteral("backup_source.settings"));

        const QStringList deletes{
            QStringLiteral("DELETE FROM sessions"),
            QStringLiteral("DELETE FROM apps"),
            QStringLiteral("DELETE FROM settings"),
        };
        for (const QString& statement : deletes) {
            QSqlQuery query(connection);
            if (!query.exec(statement)) {
                result = Status::failure(QStringLiteral("清空当前数据失败：%1").arg(describeError(query)));
                break;
            }
        }

        // apps 是 AUTOINCREMENT：不重置序列的话，恢复后新记的应用 id 会从旧最大值接着涨。
        // 只是编号变丑，但重置一下更干净；表可能还不存在，失败忽略。
        if (result.ok()) {
            QSqlQuery resetSequence(connection);
            resetSequence.exec(QStringLiteral("DELETE FROM sqlite_sequence WHERE name = 'apps'"));
        }

        const QStringList inserts{
            QStringLiteral("INSERT INTO apps (id, exe_name, exe_path, display_name, category_id, "
                           "color_hex, track_enabled, hidden, first_seen_ms, last_seen_ms) "
                           "SELECT id, exe_name, exe_path, display_name, category_id, color_hex, "
                           "track_enabled, hidden, first_seen_ms, last_seen_ms FROM backup_source.apps"),
            QStringLiteral("INSERT INTO sessions (id, app_id, window_title, start_time_ms, end_time_ms) "
                           "SELECT id, app_id, window_title, start_time_ms, end_time_ms "
                           "FROM backup_source.sessions"),
            QStringLiteral("INSERT INTO settings (key, value) "
                           "SELECT key, value FROM backup_source.settings"),
        };
        for (const QString& statement : inserts) {
            if (!result.ok()) {
                break;
            }
            QSqlQuery query(connection);
            if (!query.exec(statement)) {
                result = Status::failure(QStringLiteral("写入备份数据失败：%1").arg(describeError(query)));
            }
        }

        if (result.ok()) {
            const qint64 actualApps = countRows(connection, QStringLiteral("apps"));
            const qint64 actualSessions = countRows(connection, QStringLiteral("sessions"));
            const qint64 actualSettings = countRows(connection, QStringLiteral("settings"));
            if (actualApps != expectedApps || actualSessions != expectedSessions
                || actualSettings != expectedSettings) {
                result = Status::failure(
                    QStringLiteral("恢复后条目数不一致（应用 %1/%2，活动段 %3/%4），已回滚")
                        .arg(actualApps)
                        .arg(expectedApps)
                        .arg(actualSessions)
                        .arg(expectedSessions));
            }
        }

        if (result.ok()) {
            result = transaction.commit();
        }
        // 失败路径：Transaction 析构会自动回滚。
    }

    detach();
    return result;
}

Result<qint64> BackupService::countSessionsBefore(qint64 cutoffMs) const
{
    if (!_database.isOpen()) {
        return fail<qint64>(QStringLiteral("数据库未打开"));
    }

    // 只数已闭合的段：清理时未闭合段一定不删（见 deleteSessionsBefore），
    // 预览说「将删除 N 个」就必须真的是 N 个。
    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM sessions WHERE start_time_ms < ? AND end_time_ms IS NOT NULL"));
    query.addBindValue(cutoffMs);
    if (!query.exec()) {
        return fail<qint64>(describeError(query));
    }
    if (!query.next()) {
        return succeed<qint64>(0);
    }
    return succeed(query.value(0).toLongLong());
}

Result<qint64> BackupService::countAllSessions() const
{
    if (!_database.isOpen()) {
        return fail<qint64>(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM sessions"));
    if (!query.exec()) {
        return fail<qint64>(describeError(query));
    }
    if (!query.next()) {
        return succeed<qint64>(0);
    }
    return succeed(query.value(0).toLongLong());
}

Result<qint64> BackupService::deleteSessionsBefore(qint64 cutoffMs)
{
    if (!_database.isOpen()) {
        return fail<qint64>(QStringLiteral("数据库未打开"));
    }

    // 清理是一个破坏性操作，必须整体成败——要么这些表一起变短，
    // 要么一行都不动。所以先开事务，任何一步失败整体回滚。
    Transaction transaction(_database);
    if (!transaction.isActive()) {
        return fail<qint64>(QStringLiteral("无法开启清理事务"));
    }

    // 按段起点判断：跨零点的长段整段留下，
    // 不切成半截——切了之后「某天的总时长」会变，和用户看到的报告对不上。
    // 标题样本随 sessions 的 ON DELETE CASCADE 一起走，不单独删。
    //
    // 未闭合段（正在记录的那一条）一律不删：它没有 end_time，
    // 「起点早于 cutoff」不代表它是历史数据。删了引擎下一帧会重开一段，
    // 用户看到的是「刚清完又冒出来一条」，还会丢掉当前这一段的时长。
    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "DELETE FROM sessions WHERE start_time_ms < ? AND end_time_ms IS NOT NULL"));
    query.addBindValue(cutoffMs);
    if (!query.exec()) {
        return fail<qint64>(describeError(query));
    }
    const qint64 deleted = static_cast<qint64>(query.numRowsAffected());

    // 匿名段是独立时间线，不挂在 sessions 上，必须单独清：不清就会留下
    // 一段段比用户以为的「已清空」更早的匿名时长。
    QSqlQuery anonymous(_database.connection());
    anonymous.prepare(QStringLiteral("DELETE FROM anonymous_activity WHERE start_time_ms < ?"));
    anonymous.addBindValue(cutoffMs);
    if (!anonymous.exec()) {
        return fail<qint64>(describeError(anonymous));
    }

    // 导入的小时桶不挂 sessions，也按桶起点单独清。
    QSqlQuery buckets(_database.connection());
    buckets.prepare(QStringLiteral("DELETE FROM import_time_buckets WHERE bucket_start_ms < ?"));
    buckets.addBindValue(cutoffMs);
    if (!buckets.exec()) {
        return fail<qint64>(describeError(buckets));
    }

    // 批次计数是导入时一次性写入的快照，段被删掉后必须重算，否则界面上的
    // 「N 条」会一直虚高。清空的批次整体删掉，不留 0/0 的空壳。
    QSqlQuery refresh(_database.connection());
    if (!refresh.exec(QStringLiteral(
            "UPDATE import_batches SET "
            "  exact_session_count = (SELECT COUNT(*) FROM import_batch_sessions "
            "                         WHERE import_batch_sessions.batch_id = import_batches.id), "
            "  bucket_count = (SELECT COUNT(*) FROM import_time_buckets "
            "                  WHERE import_time_buckets.batch_id = import_batches.id)"))) {
        return fail<qint64>(describeError(refresh));
    }
    QSqlQuery prune(_database.connection());
    if (!prune.exec(QStringLiteral(
            "DELETE FROM import_batches WHERE exact_session_count = 0 AND bucket_count = 0"))) {
        return fail<qint64>(describeError(prune));
    }

    const Status committed = transaction.commit();
    if (!committed.ok()) {
        return fail<qint64>(committed.error);
    }
    return succeed(deleted);
}

} // namespace patina::data
