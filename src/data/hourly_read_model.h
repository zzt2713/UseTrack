#ifndef PATINA_DATA_HOURLY_READ_MODEL_H
#define PATINA_DATA_HOURLY_READ_MODEL_H

#include "data/types.h"

#include <QColor>
#include <QString>
#include <QVector>

namespace patina::data
{

class Database;

// 一个小时桶（0..23）的活跃毫秒数。
struct HourlyActivityDay
{
    // 24 项，与小时一一对应；无记录的小时为 0。
    qint64 hourlyMs[24] = {};
};

struct HourlyCategorySlice
{
    QString categoryId;
    QString label;
    QString colorHex;
    qint64 hourlyMs[24] = {};
};

// 小时活动读模型（从会话表实时聚合，不走预聚合表）。
//
// 口径（与 DashboardReadModel::loadToday 一致，先对账再画图）：
//   - 每段只累加它与查询区间的交叠部分，跨界段按边界切开；
//   - 未闭合段按 nowMs 封底计入（进行中的小时要能看到柱子在长）；
//   - trackEnabled = 0 的应用不进任何桶。
// 一次区间查询把总桶和分类桶算齐（同一遍扫描），两者之和构造上相等。
class HourlyReadModel
{
public:
    explicit HourlyReadModel(Database& database) : _database(database) {}

    // rangeStartMs/rangeEndMs 通常是一天的 [00:00, now)。
    // 返回总桶 + 分类桶（分类按生效分类 id 归组，只含有记录的分类）。
    Result<HourlyActivityDay> loadTotal(qint64 rangeStartMs, qint64 rangeEndMs) const;
    Result<QVector<HourlyCategorySlice>> loadByCategory(qint64 rangeStartMs,
                                                        qint64 rangeEndMs) const;

private:
    Database& _database;
};

} // namespace patina::data

#endif // PATINA_DATA_HOURLY_READ_MODEL_H
