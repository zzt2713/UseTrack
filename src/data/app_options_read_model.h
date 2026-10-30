#ifndef PATINA_DATA_APP_OPTIONS_READ_MODEL_H
#define PATINA_DATA_APP_OPTIONS_READ_MODEL_H

#include "data/types.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace patina::data
{

class Database;

// 应用曲线选择面板里的一个应用选项。
struct AppOption
{
    QString exeName;
    QString displayName;     // 覆盖项优先，空则调用方回落 exe 推导名
    QString categoryId;
    QString colorHex;
    qint64 totalDurationMs = 0;
    int activeDayCount = 0;
};

// 区间内可选应用列表（按时长降序）。搜索过滤在 ui/ 内存里做
// （应用数有限，没必要把 LIKE 塞进 SQL）。
class AppOptionsReadModel
{
public:
    explicit AppOptionsReadModel(Database& database) : _database(database) {}

    Result<QVector<AppOption>> loadOptions(qint64 rangeStartMs, qint64 rangeEndMs) const;

private:
    Database& _database;
};

} // namespace patina::data

#endif // PATINA_DATA_APP_OPTIONS_READ_MODEL_H
