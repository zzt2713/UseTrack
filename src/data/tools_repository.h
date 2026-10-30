#ifndef PATINA_DATA_TOOLS_REPOSITORY_H
#define PATINA_DATA_TOOLS_REPOSITORY_H

#include "data/tool_models.h"
#include "data/types.h"

#include <QVector>

#include <optional>

namespace patina::data
{

class Database;

// 工具专用表的读写（schema v7）。ui/ 的 ToolsRuntime 经这里落库，不碰 SQL。
//
// 单行表（tool_timers / tool_pomodoro_runs）约定 id = 1：整个程序只有一份
// 计时器与番茄钟状态，读不到就是还没用过（返回默认值，不算错误）。
class ToolsRepository
{
public:
    explicit ToolsRepository(Database& database) : _database(database) {}

    // —— 提醒 ——
    Result<QVector<ToolReminderRow>> listReminders() const;
    Status upsertReminder(const ToolReminderRow& reminder);
    Status deleteReminder(const QString& id);

    // —— 计时器 ——
    Result<ToolTimerRow> loadTimer() const;
    Status saveTimer(const ToolTimerRow& timer);

    // —— 计次（秒表圈次）——
    Result<QVector<ToolLapRow>> listLaps() const;
    Status appendLap(const ToolLapRow& lap);
    Status clearLaps();

    // —— 番茄钟 ——
    Result<ToolPomodoroRow> loadPomodoro() const;
    Status savePomodoro(const ToolPomodoroRow& pomodoro);

    // —— 每日统计 ——
    // 指定日期的完成数；没有记录返回 0。
    Result<int> dailyFocusCount(const QString& dateKey) const;
    Status setDailyFocusCount(const QString& dateKey, int count);

    // —— 活动提醒规则 ——
    Result<QVector<ToolActivityReminderRule>> listActivityRules(bool includeDisabled) const;
    Result<ToolActivityReminderRule> createActivityRule(const ToolActivityReminderRule& rule);
    Status disableActivityRule(qint64 ruleId, qint64 nowMs);
    // 当日已触发过返回 0 行；规则数量是个位数，逐条触发即可。
    Status markActivityRuleFired(qint64 ruleId, const QString& dateKey, qint64 nowMs);

private:
    Database& _database;
};

} // namespace patina::data

#endif // PATINA_DATA_TOOLS_REPOSITORY_H
