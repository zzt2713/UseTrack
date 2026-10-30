#ifndef PATINA_DATA_APP_REPOSITORY_H
#define PATINA_DATA_APP_REPOSITORY_H

#include "data/types.h"

#include <QString>

namespace patina::data
{

class Database;

// apps 表的读写。exe_name 是唯一身份（COLLATE NOCASE：
// "Chrome.exe" 与 "chrome.exe" 是同一个应用）。
class AppRepository
{
public:
    explicit AppRepository(Database& database) : _database(database) {}

    Result<App> findById(qint64 id) const;
    Result<std::optional<App>> findByExeName(const QString& exeName) const;

    // 采样时记录“又见到这个应用了”：不存在则插入，存在则刷新 last_seen_ms
    // 与 exe_path（程序升级后路径可能变），first_seen_ms 只在首次插入时定。
    Result<App> touch(const QString& exeName, const QString& exePath, qint64 nowMs);

    // 补显示名，只在库里那一格还是空的时候写。
    // 显示名是用户可以在设置页改的，所以自动解析出来的值不许覆盖
    // 已有内容——否则用户改完一次，重启就被版本资源里的名字冲掉。
    Status ensureDisplayName(const QString& exeName, const QString& displayName);

private:
    Database& _database;
};

} // namespace patina::data

#endif // PATINA_DATA_APP_REPOSITORY_H
