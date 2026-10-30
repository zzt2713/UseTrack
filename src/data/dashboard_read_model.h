#ifndef PATINA_DATA_DASHBOARD_READ_MODEL_H
#define PATINA_DATA_DASHBOARD_READ_MODEL_H

#include "data/types.h"

#include <QString>
#include <QVector>

#include <optional>

namespace patina::data
{

class Database;

// 今日概览的读模型（ui/ 不写 SQL，聚合一律在这一层做完）。
//
// 口径（与 hourly_read_model 的 loadTotal 保持一致，三处数字对得上）：
//   - 每段只累加它与 [dayStartMs, cutoffMs] 的交叠部分，跨零点的段不会被整天算给今天；
//   - 未闭合段按 cutoffMs 封底计入（即 COALESCE(end_time, ?)），
//     所以「当前正在用的应用」即时出现在分布与排行里，不必等它切走；
//   - 同一段同时经 live 暴露一次，供界面单独渲染「进行中」那一行。

// 一个应用在今日区间内的累计时长。
//
// displayName / categoryId / colorHex 是 apps 表里的用户覆盖项原样带出来：
// 空字符串表示「走默认」，由调用方（或 resolveCategoryId）决定回落到什么。
struct AppUsage
{
    qint64 appId = 0;
    QString exeName;
    // exe 的完整路径：界面靠它取应用图标，取不到时自己画首字母占位。
    QString exePath;
    QString displayName;
    QString categoryId;
    QString colorHex;
    qint64 durationMs = 0;
    int segmentCount = 0;
};

// 一个分类的累计时长。id/label/colorHex 来自内置分类表。
struct CategoryUsage
{
    QString categoryId;
    QString label;
    QString colorHex;
    qint64 durationMs = 0;
};

// 进行中的那一段（未闭合）。durationMs 已按 cutoffMs 裁剪。
struct LiveSession
{
    qint64 appId = 0;
    QString exeName;
    // exe 完整路径，供界面取图标。
    QString exePath;
    // 用户改过的显示名，空表示走默认（与 AppUsage::displayName 同一口径）。
    QString displayName;
    QString windowTitle;
    qint64 startTimeMs = 0;
    qint64 durationMs = 0;
};

struct TodayOverview
{
    qint64 dayStartMs = 0;
    qint64 cutoffMs = 0;

    // 有效活动总时长：等于各应用 durationMs 之和；未闭合段按 cutoffMs 封底计入。
    qint64 totalDurationMs = 0;

    // 按 durationMs 降序；时长相同时按 exeName 升序，保证结果稳定可比对。
    QVector<AppUsage> apps;

    // 按 durationMs 降序；只包含出现过的分类。
    QVector<CategoryUsage> categories;

    std::optional<LiveSession> live;
};

// 今日概览的读模型。
//
// 只读：不建表、不写任何东西。调用方负责数据库已打开。
// 同一事务内读三张表，避免界面刷新时拿到互相矛盾的快照。
class DashboardReadModel
{
public:
    explicit DashboardReadModel(Database& database) : _database(database) {}

    // cutoffMs 是统计截止时刻（通常取 now）。区间是 [dayStartMs, cutoffMs)。
    // cutoffMs 早于 dayStartMs 时返回空概览，不算错误。
    Result<TodayOverview> loadToday(qint64 dayStartMs, qint64 cutoffMs) const;

private:
    Database& _database;
};

// 今天 00:00 的 epoch 毫秒（本地时区）。
qint64 localDayStartMs(qint64 timestampMs);

} // namespace patina::data

#endif // PATINA_DATA_DASHBOARD_READ_MODEL_H
