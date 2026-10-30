#ifndef PATINA_DATA_TOOL_MODELS_H
#define PATINA_DATA_TOOL_MODELS_H

#include <QString>
#include <QVector>

#include <optional>

namespace patina::data
{

// 工具页的持久化模型（schema v7 的六张 tool_* 表）。
//
// 与 ui/tools/tools_types.h 的内存态结构一一对应，但住在 data/：
// ui/ 经 ToolsRepository 读写这些类型，不碰 SQL。
// 时间一律 epoch 毫秒；id 用 TEXT（提醒）/单行表（计时器、番茄钟）。

enum class ToolReminderStatus
{
    Scheduled,
    Fired,
    Cancelled,
};

enum class ToolTimerMode
{
    Stopwatch,
    Countdown,
};

enum class ToolTimerStatus
{
    Idle,
    Running,
    Paused,
    Completed,
};

enum class ToolPomodoroPhase
{
    Focus,
    ShortBreak,
    LongBreak,
};

enum class ToolPomodoroStatus
{
    Idle,
    Running,
    Paused,
    Completed,
};

struct ToolReminderRow
{
    QString id;
    QString label;
    qint64 scheduledAtMs = 0;
    qint64 createdAtMs = 0;
    ToolReminderStatus status = ToolReminderStatus::Scheduled;
    std::optional<qint64> firedAtMs;

    bool isPending() const { return status == ToolReminderStatus::Scheduled; }
};

struct ToolTimerRow
{
    ToolTimerMode mode = ToolTimerMode::Stopwatch;
    ToolTimerStatus status = ToolTimerStatus::Idle;
    QString label;
    qint64 durationMs = 0;
    qint64 accumulatedMs = 0;
    qint64 startedAtMs = 0;
    qint64 completedAtMs = 0;
};

struct ToolLapRow
{
    int index = 0;
    qint64 startedAtMs = 0;
    qint64 endedAtMs = 0;
    qint64 durationMs = 0;
};

struct ToolPomodoroRow
{
    ToolPomodoroPhase phase = ToolPomodoroPhase::Focus;
    ToolPomodoroStatus status = ToolPomodoroStatus::Idle;
    int cycleIndex = 1;
    int completedFocusCount = 0;
    int longBreakEvery = 4;
    qint64 focusMs = 25 * 60 * 1000;
    qint64 shortBreakMs = 5 * 60 * 1000;
    qint64 longBreakMs = 15 * 60 * 1000;
    qint64 phaseStartedAtMs = 0;
    qint64 phaseRemainingAtStartMs = 0;
};

// 活动提醒规则（target_kind 只有 app / category 两种——web 目标已裁剪，不存在）。
struct ToolActivityReminderRule
{
    qint64 id = 0;
    QString targetKind;       // "app" | "category"
    QString appName;          // app 规则用
    QString exeName;          // app 规则可选
    QString categoryId;       // category 规则用
    QString labelSnapshot;
    qint64 limitMs = 0;       // 1 分钟～24 小时
    QString message;
    qint64 createdAtMs = 0;
    qint64 updatedAtMs = 0;
    std::optional<qint64> disabledAtMs;
    QString lastFiredDateKey;

    bool isActive() const { return !disabledAtMs.has_value(); }
};

QString toolReminderStatusToText(ToolReminderStatus status);
ToolReminderStatus toolReminderStatusFromText(const QString& text);
QString toolTimerStatusToText(ToolTimerStatus status);
ToolTimerStatus toolTimerStatusFromText(const QString& text);
QString toolPomodoroPhaseToText(ToolPomodoroPhase phase);
ToolPomodoroPhase toolPomodoroPhaseFromText(const QString& text);
QString toolPomodoroStatusToText(ToolPomodoroStatus status);
ToolPomodoroStatus toolPomodoroStatusFromText(const QString& text);

} // namespace patina::data

#endif // PATINA_DATA_TOOL_MODELS_H
