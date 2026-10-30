#ifndef PATINA_DATA_SETTINGS_REPOSITORY_H
#define PATINA_DATA_SETTINGS_REPOSITORY_H

#include "data/types.h"

#include <QVector>

#include <optional>

namespace patina::data
{

class Database;

// settings 表的读写：纯 key/value 文本，不解释值的含义。
// 类型化的配置项（数据目录、AFK 阈值等）由上层封装，本层只管存取。
class SettingsRepository
{
public:
    explicit SettingsRepository(Database& database) : _database(database) {}

    Result<std::optional<QString>> value(const QString& key) const;

    // 不存在则插入，存在则覆盖。
    Status setValue(const QString& key, const QString& value);

    // 按 key 升序取全部，用于备份导出。
    Result<QVector<SettingRow>> listAll() const;

    Status remove(const QString& key);

private:
    Database& _database;
};

} // namespace patina::data

#endif // PATINA_DATA_SETTINGS_REPOSITORY_H
