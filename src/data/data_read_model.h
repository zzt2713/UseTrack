#ifndef PATINA_DATA_DATA_READ_MODEL_H
#define PATINA_DATA_DATA_READ_MODEL_H

#include "data/types.h"

#include <QString>
#include <QVector>
#include <QtGlobal>

#include <array>

namespace patina::data
{

class Database;

// 数据页（趋势折线 / 热力图 / 应用曲线）的读模型（ui/ 不写 SQL）。
//
// 口径与 DashboardReadModel 完全一致：
//   - 只统计已闭合的活动段；
//   - 每段只累加它与查询区间的交叠部分，跨零点的段按本地日边界切开分给各自的天；
//   - 未闭合段不计入任何数字。
//
// 实现上一次性把区间内所有交叠段取回内存再切桶。最长区间是热力图的 53 周（371 天），
// 段数在万级，一次切桶换来的是「日总时长 == 各应用日时长之和」构造上成立：
// 两者出自同一次切桶，不可能对不上。

using HourlyMinutes = std::array<qint64, 24>;

// 一个本地日的累计时长与小时分布。hourlyMs 的 24 项是该小时内的累计毫秒数。
struct TrendDay
{
    qint64 dayStartMs = 0;  // 本地日 00:00
    qint64 durationMs = 0;
    HourlyMinutes hourlyMs{};
};

// 一个应用在区间内的逐日时长。
//
// displayName / categoryId / colorHex 是 apps 表里的用户覆盖项原样带出来：
// 空字符串表示「走默认」，由调用方（或 resolveCategoryId）决定回落到什么。
struct AppTrendSeries
{
    QString exeName;
    QString displayName;
    QString categoryId;
    QString colorHex;
    qint64 totalDurationMs = 0;
    QVector<qint64> dayDurationsMs;  // 与 TrendData::days 一一对应
};

struct TrendData
{
    qint64 rangeStartMs = 0;  // 已对齐到本地日 00:00
    qint64 rangeEndMs = 0;    // 开区间；最后一天可能是不完整的一天

    // 等于各 days[i].durationMs 之和，也等于各 apps[i].totalDurationMs 之和。
    qint64 totalDurationMs = 0;

    // 覆盖区间内的每一个本地日，没有记录的日 durationMs 为 0。
    QVector<TrendDay> days;

    // 按 totalDurationMs 降序；时长相同时按 exeName 升序，保证结果稳定可比对。
    QVector<AppTrendSeries> apps;

    int activeDayCount = 0;  // durationMs > 0 的天数
    qint64 peakDayMs = 0;    // 单日峰值
};

class DataReadModel
{
public:
    explicit DataReadModel(Database& database) : _database(database) {}

    // rangeStartMs 会先对齐到它所在本地日的 00:00；rangeEndMs 可以是任意时刻（通常取 now），
    // 因此最后一天可能是不完整的一天。
    Result<TrendData> loadTrend(qint64 rangeStartMs, qint64 rangeEndMs) const;

private:
    Database& _database;
};

} // namespace patina::data

#endif // PATINA_DATA_DATA_READ_MODEL_H
