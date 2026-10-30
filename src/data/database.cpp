#include "data/database.h"

#include "data/schema.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace patina::data
{

Database::~Database()
{
    closeConnection();
}

void Database::closeConnection()
{
    if (_connectionName.isEmpty()) {
        return;
    }

    // 先让 QSqlDatabase 的引用计数归零，再移除连接，否则会警告连接仍在使用。
    {
        QSqlDatabase handle = QSqlDatabase::database(_connectionName, false);
        if (handle.isOpen()) {
            handle.close();
        }
    }
    if (QSqlDatabase::contains(_connectionName)) {
        QSqlDatabase::removeDatabase(_connectionName);
    }
    _connectionName.clear();
    _filePath.clear();
}

bool Database::isOpen() const
{
    if (_connectionName.isEmpty()) {
        return false;
    }
    return QSqlDatabase::database(_connectionName, false).isOpen();
}

QSqlDatabase Database::connection() const
{
    return QSqlDatabase::database(_connectionName, false);
}

Status Database::open(const QString& filePath)
{
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        return Status::failure(QStringLiteral("缺少 QSQLITE 驱动，无法打开数据库"));
    }

    closeConnection();

    // 每个实例一个独立连接名，避免多实例互相覆盖默认连接。
    _connectionName = QStringLiteral("patina-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    _filePath = filePath;

    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), _connectionName);
    // 内存库不落盘，文件名保持原样给测试看。
    database.setDatabaseName(filePath);
    if (!database.open()) {
        const QString message = database.lastError().text();
        // 先释放这个副本：removeDatabase 要求没有别的 QSqlDatabase 引用同一连接，
        // 否则会打 "connection is still in use" 警告。
        database = QSqlDatabase();
        closeConnection();
        return Status::failure(QStringLiteral("打开数据库失败：%1").arg(message));
    }

    // WAL：读写不互相阻塞，崩溃后恢复也更可靠。
    // 内存库不支持 WAL，失败时忽略即可，不影响正确性。
    QSqlQuery pragma(database);
    pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    // 外键默认是关的，必须每条连接单独打开，否则 ON DELETE CASCADE 不生效。
    pragma.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    // 事务拿不到锁时最多等 5 秒再报 busy。
    pragma.exec(QStringLiteral("PRAGMA busy_timeout = 5000"));
    // 内存红线显式化：页缓存 2MB（负数 = KB），mmap 关闭。
    // SQLite 默认缓存会随库大小膨胀，这里定死上限。
    pragma.exec(QStringLiteral("PRAGMA cache_size = -2000"));
    pragma.exec(QStringLiteral("PRAGMA mmap_size = 0"));

    return Status::success();
}

Status Database::migrate()
{
    return applyMigrations(*this);
}

Status Database::openAndMigrate(const QString& filePath)
{
    const Status opened = open(filePath);
    if (!opened.ok()) {
        return opened;
    }

    const Status migrated = migrate();
    if (!migrated.ok()) {
        closeConnection();
        return migrated;
    }
    return Status::success();
}

Transaction::Transaction(Database& database)
    : _database(database)
{
    if (!database.isOpen()) {
        return;
    }

    QSqlQuery query(database.connection());
    // IMMEDIATE：立刻获取写锁。默认的 DEFERRED 会在读事务升级为写事务时
    // 才去要锁，多连接下容易出现无法立刻重试的 SQLITE_BUSY。
    if (query.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        _active = true;
    }
}

Transaction::~Transaction()
{
    if (_active) {
        // 没提交就是没走完成功路径：回滚，不留半个事务。
        QSqlQuery query(_database.connection());
        query.exec(QStringLiteral("ROLLBACK"));
        _active = false;
    }
}

Status Transaction::commit()
{
    if (!_active) {
        return Status::failure(QStringLiteral("事务未开启"));
    }

    QSqlQuery query(_database.connection());
    if (!query.exec(QStringLiteral("COMMIT"))) {
        const QString message = query.lastError().text();
        QSqlQuery rollback(_database.connection());
        rollback.exec(QStringLiteral("ROLLBACK"));
        _active = false;
        return Status::failure(QStringLiteral("提交事务失败：%1").arg(message));
    }

    _active = false;
    return Status::success();
}

Status Transaction::rollback()
{
    if (!_active) {
        return Status::success();
    }

    QSqlQuery query(_database.connection());
    const bool ok = query.exec(QStringLiteral("ROLLBACK"));
    _active = false;
    if (!ok) {
        return Status::failure(QStringLiteral("回滚事务失败：%1").arg(query.lastError().text()));
    }
    return Status::success();
}

} // namespace patina::data
