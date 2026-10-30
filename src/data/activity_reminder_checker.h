#ifndef PATINA_DATA_ACTIVITY_REMINDER_CHECKER_H
#define PATINA_DATA_ACTIVITY_REMINDER_CHECKER_H

#include "data/tool_models.h"
#include "data/types.h"

#include <QVector>

namespace patina::data
{

class Database;

// 一次命中：规则触发了，通知文案在这里拼好，调用方只管弹。
struct ActivityReminderHit
{
    qint64 ruleId = 0;
    QString label;
    QString message;
    qint64 limitMs = 0;
    qint64 usageMs = 0;
};

// 活动提醒规则的当日用量检查（只支持应用与分类两类目标）。
//
// 口径（与 DashboardReadModel 一致）：只统计已闭合段与查询区间的交叠；
// 今日总时长含未闭合段按 nowMs 裁剪的部分——「用了 60 分钟要提醒」必须
// 数上正在进行中的这一段，否则永远要到段闭合了才触发。
//
// 幂等：markFired 的 UPDATE 带 last_fired_date_key 守卫，一天只触发一次；
// 调用方按返回的 hits 逐条弹通知，弹之前不再查询。
class ActivityReminderChecker
{
public:
    explicit ActivityReminderChecker(Database& database) : _database(database) {}

    // 检查全部启用规则并触发到点的。dateKey 是本地日期 yyyy-MM-dd，
    // dayStartMs 是今天 00:00（本地），nowMs 是当前时刻。
    // 返回本次真正触发（已写库标记）的命中列表。
    Result<QVector<ActivityReminderHit>> fireDueRules(const QString& dateKey,
                                                      qint64 dayStartMs,
                                                      qint64 nowMs);

private:
    Database& _database;
};

} // namespace patina::data

#endif // PATINA_DATA_ACTIVITY_REMINDER_CHECKER_H
