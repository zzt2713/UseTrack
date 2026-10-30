#ifndef PATINA_DATA_DATA_DIRECTORY_MIGRATOR_H
#define PATINA_DATA_DATA_DIRECTORY_MIGRATOR_H

#include "data/types.h"

#include <QSqlDatabase>
#include <QString>

namespace patina::data
{

class Database;

// 数据目录搬迁（简化版，不做 staging/身份校验状态机）。
//
// 流程（由 SettingsPage 驱动，本类只做单步原子操作）：
//   1. validateTarget：校验新目录（可创建、可写、不是旧目录本身）。
//   2. migrateDatabaseFile：关库 → WAL checkpoint 合并 → 拷贝 .db 单文件
//      → 开新库校验行数一致。
//   3. 成功后由调用方写 config.json 并重开库；失败时旧文件不动（回滚 =
//      原地不动 + 删半截临时文件）。
//
// 关库由调用方负责（TrackingService 持有库句柄），本类不碰连接生命周期；
// openDatabase 归调用方，本类提供行数校验用的辅助。
namespace DataDirectoryMigrator
{

// 目标路径校验：非空、不存在同名文件占位、目录可创建/可写、与当前目录不同。
// 返回错误消息；空串表示通过。
QString validateTarget(const QString& targetDirectory, const QString& currentDirectory);

// 搬库文件。要求调用方已经关掉库（database.isOpened() == false）。
// currentPath 是旧库文件（含路径），targetPath 是新位置。
// 成功后旧文件原样保留（搬走 = 拷贝 + 校验，不删源——失败可回滚）。
Status migrateDatabaseFile(const QString& currentPath, const QString& targetPath);

// 行数一致性校验：两个库文件的 sessions 行数一致（目录搬迁成功判据）。
// 只读打开两个文件，用完即关。
Result<bool> verifyRowCountsMatch(const QString& currentPath, const QString& targetPath);

// 简易打开辅助：为校验临时打开一个库（只读）。内部用独立连接名。
class TemporaryConnection
{
public:
    TemporaryConnection(const QString& filePath, bool readOnly);
    ~TemporaryConnection();
    TemporaryConnection(const TemporaryConnection&) = delete;
    TemporaryConnection& operator=(const TemporaryConnection&) = delete;

    bool isOpen() const { return _open; }
    QString error() const { return _error; }
    qint64 sessionCount() const;
    // 供 checkpoint 之类的一次性操作用；连接生命周期仍归本类。
    QSqlDatabase connection() const;

private:
    QString _name;
    QString _error;
    bool _open = false;
};

} // namespace DataDirectoryMigrator

} // namespace patina::data

#endif // PATINA_DATA_DATA_DIRECTORY_MIGRATOR_H
