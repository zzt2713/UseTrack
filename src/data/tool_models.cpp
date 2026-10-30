#include "data/tool_models.h"

namespace patina::data
{

QString toolReminderStatusToText(ToolReminderStatus status)
{
    switch (status) {
    case ToolReminderStatus::Fired:
        return QStringLiteral("fired");
    case ToolReminderStatus::Cancelled:
        return QStringLiteral("cancelled");
    case ToolReminderStatus::Scheduled:
        break;
    }
    return QStringLiteral("scheduled");
}

ToolReminderStatus toolReminderStatusFromText(const QString& text)
{
    if (text == QLatin1String("fired")) {
        return ToolReminderStatus::Fired;
    }
    if (text == QLatin1String("cancelled")) {
        return ToolReminderStatus::Cancelled;
    }
    return ToolReminderStatus::Scheduled;
}

QString toolTimerStatusToText(ToolTimerStatus status)
{
    switch (status) {
    case ToolTimerStatus::Running:
        return QStringLiteral("running");
    case ToolTimerStatus::Paused:
        return QStringLiteral("paused");
    case ToolTimerStatus::Completed:
        return QStringLiteral("completed");
    case ToolTimerStatus::Idle:
        break;
    }
    return QStringLiteral("idle");
}

ToolTimerStatus toolTimerStatusFromText(const QString& text)
{
    if (text == QLatin1String("running")) {
        return ToolTimerStatus::Running;
    }
    if (text == QLatin1String("paused")) {
        return ToolTimerStatus::Paused;
    }
    if (text == QLatin1String("completed")) {
        return ToolTimerStatus::Completed;
    }
    return ToolTimerStatus::Idle;
}

QString toolPomodoroPhaseToText(ToolPomodoroPhase phase)
{
    if (phase == ToolPomodoroPhase::ShortBreak) {
        return QStringLiteral("short_break");
    }
    if (phase == ToolPomodoroPhase::LongBreak) {
        return QStringLiteral("long_break");
    }
    return QStringLiteral("focus");
}

ToolPomodoroPhase toolPomodoroPhaseFromText(const QString& text)
{
    if (text == QLatin1String("short_break")) {
        return ToolPomodoroPhase::ShortBreak;
    }
    if (text == QLatin1String("long_break")) {
        return ToolPomodoroPhase::LongBreak;
    }
    return ToolPomodoroPhase::Focus;
}

QString toolPomodoroStatusToText(ToolPomodoroStatus status)
{
    switch (status) {
    case ToolPomodoroStatus::Running:
        return QStringLiteral("running");
    case ToolPomodoroStatus::Paused:
        return QStringLiteral("paused");
    case ToolPomodoroStatus::Completed:
        return QStringLiteral("completed");
    case ToolPomodoroStatus::Idle:
        break;
    }
    return QStringLiteral("idle");
}

ToolPomodoroStatus toolPomodoroStatusFromText(const QString& text)
{
    if (text == QLatin1String("running")) {
        return ToolPomodoroStatus::Running;
    }
    if (text == QLatin1String("paused")) {
        return ToolPomodoroStatus::Paused;
    }
    if (text == QLatin1String("completed")) {
        return ToolPomodoroStatus::Completed;
    }
    return ToolPomodoroStatus::Idle;
}

} // namespace patina::data
