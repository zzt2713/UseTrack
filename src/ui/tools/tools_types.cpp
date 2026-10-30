#include "ui/tools/tools_types.h"

#include <QDate>
#include <QDateTime>
#include <QTime>

#include <algorithm>

namespace patina::ui
{

namespace
{

QString pad2(qint64 value)
{
    return QStringLiteral("%1").arg(std::max<qint64>(0, value), 2, 10, QLatin1Char('0'));
}

} // namespace

qint64 ToolReminder::remainingMsAt(qint64 nowMs) const
{
    return std::max<qint64>(0, scheduledAtMs - nowMs);
}

qint64 ToolTimer::elapsedMsAt(qint64 nowMs) const
{
    if (status != TimerStatus::Running || startedAtMs <= 0) {
        return std::max<qint64>(0, accumulatedMs);
    }
    return std::max<qint64>(0, accumulatedMs + std::max<qint64>(0, nowMs - startedAtMs));
}

qint64 ToolTimer::remainingMsAt(qint64 nowMs) const
{
    return std::max<qint64>(0, durationMs - elapsedMsAt(nowMs));
}

bool ToolTimer::isCountdownDue(qint64 nowMs) const
{
    return mode == TimerMode::Countdown && status == TimerStatus::Running
        && remainingMsAt(nowMs) <= 0;
}

void ToolTimer::foldElapsed(qint64 nowMs)
{
    if (status != TimerStatus::Running) {
        return;
    }
    accumulatedMs = elapsedMsAt(nowMs);
    startedAtMs = nowMs;
}

qint64 ToolPomodoro::phaseDurationMs() const
{
    if (phase == PomodoroPhase::ShortBreak) {
        return shortBreakMs;
    }
    if (phase == PomodoroPhase::LongBreak) {
        return longBreakMs;
    }
    return focusMs;
}

qint64 ToolPomodoro::remainingMsAt(qint64 nowMs) const
{
    if (status != PomodoroStatus::Running || phaseStartedAtMs <= 0) {
        return std::max<qint64>(0, phaseRemainingAtStartMs);
    }
    return std::max<qint64>(0, phaseRemainingAtStartMs - std::max<qint64>(0, nowMs - phaseStartedAtMs));
}

bool ToolPomodoro::isPhaseDue(qint64 nowMs) const
{
    return status == PomodoroStatus::Running && remainingMsAt(nowMs) <= 0;
}

void ToolPomodoro::advanceAfterCompletion()
{
    const int every = std::max(1, longBreakEvery);
    if (phase == PomodoroPhase::Focus) {
        ++completedFocusCount;
        if (completedFocusCount % every == 0) {
            phase = PomodoroPhase::LongBreak;
            cycleIndex = every;
        } else {
            phase = PomodoroPhase::ShortBreak;
            cycleIndex = completedFocusCount % every;
        }
    } else {
        phase = PomodoroPhase::Focus;
        cycleIndex = completedFocusCount % every + 1;
    }
    phaseRemainingAtStartMs = phaseDurationMs();
    phaseStartedAtMs = 0;
}

QString formatHms(qint64 ms)
{
    const qint64 total = std::max<qint64>(0, ms) / 1000;
    return pad2(total / 3600) + QLatin1Char(':') + pad2((total % 3600) / 60) + QLatin1Char(':')
        + pad2(total % 60);
}

QString formatCompact(qint64 ms)
{
    const qint64 total = std::max<qint64>(0, ms) / 1000;
    if (total >= 3600) {
        return formatHms(ms);
    }
    return pad2(total / 60) + QLatin1Char(':') + pad2(total % 60);
}

QString formatClockTime(qint64 ms)
{
    const QDateTime moment = QDateTime::fromMSecsSinceEpoch(ms);
    return moment.toString(QStringLiteral("HH:mm"));
}

QString todayDateKey()
{
    return QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"));
}

} // namespace patina::ui
