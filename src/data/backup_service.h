#ifndef PATINA_DATA_BACKUP_SERVICE_H
#define PATINA_DATA_BACKUP_SERVICE_H

#include "data/types.h"

#include <QString>

namespace patina::data
{

class Database;

// 备份文件校验结果。
//
// 界面直接展示 message，不需要自己拼文案：这里说清楚「能不能恢复、为什么不能」。
struct BackupInspection
{
    bool restorable = false;
    QString message;
    int schemaVersion = 0;
    qint64 sessionCount = 0;
    qint64 appCount = 0;
    qint64 settingCount = 0;
    qint64 sizeBytes = 0;
};

// 备份 / 恢复 / 清理历史的唯一入口。
//
// 三条硬约束（破坏性操作前先校验目标路径与数据完整性）：
//   - 导出走 VACUUM INTO，得到的是一个干净的单文件，不含 WAL 里的未合并页。
//   - 恢复前必须先 inspect()：文件可读、SQLite 完整性检查通过、schema 版本与当前一致。
//   - 恢复与清理都在一个事务里完成，任何一步失败整体回滚，当前数据不受影响。
//
// 为什么恢复用 ATTACH + 复制而不是直接覆盖 .db 文件：
// 进程正握着这条连接（WAL 模式），文件级替换会留下不一致的 -wal / -shm，
// 而且必须重启程序才能重开连接。ATTACH 复制只用一条连接、当场生效。
class BackupService
{
public:
    explicit BackupService(Database& database) : _database(database) {}

    // 导出到 targetPath，已存在则覆盖。父目录不存在会自动创建。
    Status exportTo(const QString& targetPath) const;

    // 用一条临时连接读备份文件，不改动当前库。
    Result<BackupInspection> inspect(const QString& backupPath) const;

    // 用备份整体替换当前库的应用、活动段与设置。
    // schema 版本高于或低于当前程序都拒绝：列可能对不上，硬搬会写坏数据。
    Status restoreFrom(const QString& backupPath);

    // 统计 / 删除起点早于 cutoffMs 的活动段（按段起点判断）。
    // 计数只算已闭合段，与删除口径严格一致（预览说删 N 个就真删 N 个）。
    Result<qint64> countSessionsBefore(qint64 cutoffMs) const;
    Result<qint64> deleteSessionsBefore(qint64 cutoffMs);

    // 库里活动段总数（含正在记录的那一条），给「存储」卡的统计行用。
    Result<qint64> countAllSessions() const;

    // 默认备份目录与文件名（界面的「用默认位置」按钮用）。
    static QString defaultBackupDirectory();
    static QString defaultBackupFileName(qint64 nowMs);

private:
    Database& _database;
};

} // namespace patina::data

#endif // PATINA_DATA_BACKUP_SERVICE_H
