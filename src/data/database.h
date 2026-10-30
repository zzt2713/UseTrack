#ifndef PATINA_DATA_DATABASE_H
#define PATINA_DATA_DATABASE_H

#include "data/types.h"

#include <QSqlDatabase>
#include <QString>

namespace patina::data
{

// 单连接 SQLite 封装（单连接，不把历史明细读进常驻内存）。
//
// 线程约束：一个 Database 只能在创建它的线程里使用，QSqlDatabase 不能跨线程共享。
// 本类不做内部加锁；采样线程与 GUI 线程各自持有自己的实例。
//
// 分工：本类只负责连接、pragma、事务；建表与查询归 schema.h 和各 repository。
class Database
{
public:
    Database() = default;
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool isOpen() const;
    QString filePath() const { return _filePath; }

    // 打开（不存在则创建）数据库文件，":memory:" 表示内存库。
    // 只设置连接参数，不建表。
    Status open(const QString& filePath);

    // 打开 + 迁移到当前 schema 版本。
    Status openAndMigrate(const QString& filePath);

    // 迁移到当前版本；已是最新则什么都不做。
    Status migrate();

    // 关闭当前连接（数据目录搬迁等需要临时关库的流程用）。
    // 之后可再次 open/openAndMigrate（同一实例或同一路径）。
    void closeConnectionPublic() { closeConnection(); }

    // 给 schema/ 与 repository 层执行 SQL 用。
    // 约定：ui/ 不得调用本方法，也不得自己持有 QSqlDatabase（UI 不写 SQL）。
    QSqlDatabase connection() const;

private:
    void closeConnection();

    QString _connectionName;
    QString _filePath;
};

// RAII 事务：析构时未提交则回滚。
//
// 用 BEGIN IMMEDIATE：写事务一开始就拿写锁，避免“先读后写”升级锁时的 SQLITE_BUSY。
// 不支持嵌套。repository 的方法本身不开事务，需要把「写事实 + 写派生状态」
// 放在一起时由调用方组合：
//
//     Transaction transaction(database);
//     sessionRepository.closeOpen(nowMs);
//     sessionRepository.open(appId, title, nowMs);
//     transaction.commit();
//
// BEGIN 失败时 isActive() 为假，此时后续写入不在事务里，调用方必须立刻放弃。
class Transaction
{
public:
    explicit Transaction(Database& database);
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    bool isActive() const { return _active; }

    Status commit();
    Status rollback();

private:
    Database& _database;
    bool _active = false;
};

} // namespace patina::data

#endif // PATINA_DATA_DATABASE_H
