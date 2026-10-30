#ifndef PATINA_UI_TOOLS_TYPES_H
#define PATINA_UI_TOOLS_TYPES_H

#include <QString>
#include <QVector>

namespace patina::ui
{

// 工具页的全部状态模型。
//
// 状态只活在本地：整份序列化成 JSON 文本，写进 settings 表的一个 key。
// 不新增表、不新增 schema 版本——工具是轻量的个人辅助，为它单开一套表与迁移
// 不划算，而 settings 本来就是「只存文本、不解释值含义」的容器。
//
// 时间一律 epoch 毫秒，与 data/ 层保持同一口径。

enum class ReminderStatus
{
    Scheduled,
    Fired,
    Cancelled,
};

enum class TimerMode
{
    Stopwatch,
    Countdown,
};

enum class TimerStatus
{
    Idle,
    Running,
    Paused,
    Completed,
};

enum class PomodoroPhase
{
    Focus,
    ShortBreak,
    LongBreak,
};

enum class PomodoroStatus
{
    Idle,
    Running,
    Paused,
    Completed,
};

struct ToolReminder
{
    QString id;
    QString label;
    qint64 scheduledAtMs = 0;
    qint64 createdAtMs = 0;
    ReminderStatus status = ReminderStatus::Scheduled;
    qint64 firedAtMs = 0;

    bool isPending() const { return status == ReminderStatus::Scheduled; }
    qint64 remainingMsAt(qint64 nowMs) const;
};

struct ToolLap
{
    int index = 0;
    qint64 startedAtMs = 0;
    qint64 endedAtMs = 0;
    qint64 durationMs = 0;
};

struct ToolTimer
{
    TimerMode mode = TimerMode::Stopwatch;
    TimerStatus status = TimerStatus::Idle;
    QString label;
    qint64 durationMs = 0;      // 仅倒计时有意义
    qint64 accumulatedMs = 0;
    qint64 startedAtMs = 0;     // 最近一次启动的时刻
    qint64 completedAtMs = 0;

    bool isActive() const
    {
        return status == TimerStatus::Running || status == TimerStatus::Paused;
    }

    qint64 elapsedMsAt(qint64 nowMs) const;
    qint64 remainingMsAt(qint64 nowMs) const;
    bool isCountdownDue(qint64 nowMs) const;

    // 把「运行中的这一段」折进 accumulatedMs，并把起点拨到现在。
    // 暂停与落盘都走它：落盘时折一次，重启后读到的就是最后一次落库的时长，
    // 关机那段不会被算进去。
    void foldElapsed(qint64 nowMs);
};

struct ToolPomodoro
{
    PomodoroPhase phase = PomodoroPhase::Focus;
    PomodoroStatus status = PomodoroStatus::Idle;
    int cycleIndex = 1;
    int completedFocusCount = 0;
    int longBreakEvery = 4;
    qint64 focusMs = 25 * 60 * 1000;
    qint64 shortBreakMs = 5 * 60 * 1000;
    qint64 longBreakMs = 15 * 60 * 1000;

    // 剩余量记「启动那一刻还剩多少」而不是记绝对结束时刻：
    // 暂停再继续时只要改起点，不必重算结束时刻。
    qint64 phaseStartedAtMs = 0;
    qint64 phaseRemainingAtStartMs = 0;

    QString statDateKey;
    int completedFocusToday = 0;

    qint64 phaseDurationMs() const;
    qint64 remainingMsAt(qint64 nowMs) const;
    bool isPhaseDue(qint64 nowMs) const;

    // 当前阶段记完成，并推进到下一阶段。不负责设置运行状态。
    void advanceAfterCompletion();
};

struct ToolsSettings
{
    int defaultCountdownMinutes = 25;
    int pomodoroFocusMinutes = 25;
    int pomodoroShortBreakMinutes = 5;
    int pomodoroLongBreakMinutes = 15;
    int pomodoroLongBreakEvery = 4;
};

QString formatHms(qint64 ms);
// 小于一小时用 MM:SS，否则退回 HH:MM:SS
QString formatCompact(qint64 ms);
QString formatClockTime(qint64 ms);
// 本地日期键 yyyy-MM-dd
QString todayDateKey();

} // namespace patina::ui

#endif // PATINA_UI_TOOLS_TYPES_H
