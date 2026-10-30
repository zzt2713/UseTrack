#include "data/tools_repository.h"

#include "data/database.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace patina::data
{
namespace
{

QString describeError(const QSqlQuery& query)
{
    const QSqlError error = query.lastError();
    return error.isValid() ? error.text() : QStringLiteral("SQL 执行失败");
}

ToolReminderRow readReminder(const QSqlQuery& query)
{
    ToolReminderRow reminder;
    reminder.id = query.value(0).toString();
    reminder.label = query.value(1).toString();
    reminder.scheduledAtMs = query.value(2).toLongLong();
    reminder.createdAtMs = query.value(3).toLongLong();
    reminder.status = toolReminderStatusFromText(query.value(4).toString());
    if (!query.value(5).isNull()) {
        reminder.firedAtMs = query.value(5).toLongLong();
    }
    return reminder;
}

ToolActivityReminderRule readRule(const QSqlQuery& query)
{
    ToolActivityReminderRule rule;
    rule.id = query.value(0).toLongLong();
    rule.targetKind = query.value(1).toString();
    rule.appName = query.value(2).toString();
    rule.exeName = query.value(3).toString();
    rule.categoryId = query.value(4).toString();
    rule.labelSnapshot = query.value(5).toString();
    rule.limitMs = query.value(6).toLongLong();
    rule.message = query.value(7).toString();
    rule.createdAtMs = query.value(8).toLongLong();
    rule.updatedAtMs = query.value(9).toLongLong();
    if (!query.value(10).isNull()) {
        rule.disabledAtMs = query.value(10).toLongLong();
    }
    rule.lastFiredDateKey = query.value(11).toString();
    return rule;
}

const char* const kRuleColumns =
    "SELECT id, target_kind, app_name, exe_name, category_id, label_snapshot, limit_ms, "
    "message, created_at_ms, updated_at_ms, disabled_at_ms, last_fired_date_key "
    "FROM tool_activity_reminder_rules";

} // namespace

// —— 提醒 ——

Result<QVector<ToolReminderRow>> ToolsRepository::listReminders() const
{
    if (!_database.isOpen()) {
        return fail<QVector<ToolReminderRow>>(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    if (!query.exec(QStringLiteral(
            "SELECT id, label, scheduled_at_ms, created_at_ms, status, fired_at_ms "
            "FROM tool_reminders ORDER BY scheduled_at_ms ASC"))) {
        return fail<QVector<ToolReminderRow>>(describeError(query));
    }

    QVector<ToolReminderRow> reminders;
    while (query.next()) {
        reminders.append(readReminder(query));
    }
    return succeed(reminders);
}

Status ToolsRepository::upsertReminder(const ToolReminderRow& reminder)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "INSERT INTO tool_reminders (id, label, scheduled_at_ms, created_at_ms, status, fired_at_ms) "
        "VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "    label = excluded.label,"
        "    scheduled_at_ms = excluded.scheduled_at_ms,"
        "    status = excluded.status,"
        "    fired_at_ms = excluded.fired_at_ms"));
    query.addBindValue(reminder.id);
    query.addBindValue(reminder.label);
    query.addBindValue(reminder.scheduledAtMs);
    query.addBindValue(reminder.createdAtMs);
    query.addBindValue(toolReminderStatusToText(reminder.status));
    if (reminder.firedAtMs.has_value()) {
        query.addBindValue(*reminder.firedAtMs);
    } else {
        query.addBindValue(QVariant(QMetaType::fromType<qint64>()));
    }
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

Status ToolsRepository::deleteReminder(const QString& id)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral("DELETE FROM tool_reminders WHERE id = ?"));
    query.addBindValue(id);
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

// —— 计时器 ——

Result<ToolTimerRow> ToolsRepository::loadTimer() const
{
    if (!_database.isOpen()) {
        return fail<ToolTimerRow>(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    if (!query.exec(QStringLiteral(
            "SELECT mode, label, duration_ms, accumulated_ms, started_at_ms, completed_at_ms, status "
            "FROM tool_timers WHERE id = 1"))) {
        return fail<ToolTimerRow>(describeError(query));
    }
    if (!query.next()) {
        // 没用过计时器：默认值不算错误。
        return succeed(ToolTimerRow{});
    }

    ToolTimerRow timer;
    timer.mode = query.value(0).toString() == QLatin1String("countdown")
        ? ToolTimerMode::Countdown : ToolTimerMode::Stopwatch;
    timer.label = query.value(1).toString();
    timer.durationMs = query.value(2).toLongLong();
    timer.accumulatedMs = query.value(3).toLongLong();
    timer.startedAtMs = query.value(4).toLongLong();
    timer.completedAtMs = query.value(5).toLongLong();
    timer.status = toolTimerStatusFromText(query.value(6).toString());
    return succeed(timer);
}

Status ToolsRepository::saveTimer(const ToolTimerRow& timer)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "INSERT INTO tool_timers (id, mode, label, duration_ms, accumulated_ms, started_at_ms, "
        "completed_at_ms, status, updated_at_ms) "
        "VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "    mode = excluded.mode, label = excluded.label, duration_ms = excluded.duration_ms, "
        "    accumulated_ms = excluded.accumulated_ms, started_at_ms = excluded.started_at_ms, "
        "    completed_at_ms = excluded.completed_at_ms, status = excluded.status, "
        "    updated_at_ms = excluded.updated_at_ms"));
    query.addBindValue(timer.mode == ToolTimerMode::Countdown
                           ? QStringLiteral("countdown") : QStringLiteral("stopwatch"));
    query.addBindValue(timer.label);
    query.addBindValue(timer.durationMs);
    query.addBindValue(timer.accumulatedMs);
    query.addBindValue(timer.startedAtMs);
    query.addBindValue(timer.completedAtMs);
    query.addBindValue(toolTimerStatusToText(timer.status));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

// —— 计次 ——

Result<QVector<ToolLapRow>> ToolsRepository::listLaps() const
{
    if (!_database.isOpen()) {
        return fail<QVector<ToolLapRow>>(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    if (!query.exec(QStringLiteral(
            "SELECT lap_index, started_at_ms, ended_at_ms, duration_ms "
            "FROM tool_timer_laps ORDER BY lap_index ASC"))) {
        return fail<QVector<ToolLapRow>>(describeError(query));
    }

    QVector<ToolLapRow> laps;
    while (query.next()) {
        ToolLapRow lap;
        lap.index = query.value(0).toInt();
        lap.startedAtMs = query.value(1).toLongLong();
        lap.endedAtMs = query.value(2).toLongLong();
        lap.durationMs = query.value(3).toLongLong();
        laps.append(lap);
    }
    return succeed(laps);
}

Status ToolsRepository::appendLap(const ToolLapRow& lap)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "INSERT INTO tool_timer_laps (lap_index, started_at_ms, ended_at_ms, duration_ms) "
        "VALUES (?, ?, ?, ?)"));
    query.addBindValue(lap.index);
    query.addBindValue(lap.startedAtMs);
    query.addBindValue(lap.endedAtMs);
    query.addBindValue(lap.durationMs);
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

Status ToolsRepository::clearLaps()
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    if (!query.exec(QStringLiteral("DELETE FROM tool_timer_laps"))) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

// —— 番茄钟 ——

Result<ToolPomodoroRow> ToolsRepository::loadPomodoro() const
{
    if (!_database.isOpen()) {
        return fail<ToolPomodoroRow>(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    if (!query.exec(QStringLiteral(
            "SELECT phase, status, cycle_index, completed_focus_count, long_break_every, "
            "focus_ms, short_break_ms, long_break_ms, phase_started_at_ms, "
            "phase_remaining_at_start_ms FROM tool_pomodoro_runs WHERE id = 1"))) {
        return fail<ToolPomodoroRow>(describeError(query));
    }
    if (!query.next()) {
        return succeed(ToolPomodoroRow{});
    }

    ToolPomodoroRow pomodoro;
    pomodoro.phase = toolPomodoroPhaseFromText(query.value(0).toString());
    pomodoro.status = toolPomodoroStatusFromText(query.value(1).toString());
    pomodoro.cycleIndex = query.value(2).toInt();
    pomodoro.completedFocusCount = query.value(3).toInt();
    pomodoro.longBreakEvery = query.value(4).toInt();
    pomodoro.focusMs = query.value(5).toLongLong();
    pomodoro.shortBreakMs = query.value(6).toLongLong();
    pomodoro.longBreakMs = query.value(7).toLongLong();
    pomodoro.phaseStartedAtMs = query.value(8).toLongLong();
    pomodoro.phaseRemainingAtStartMs = query.value(9).toLongLong();
    return succeed(pomodoro);
}

Status ToolsRepository::savePomodoro(const ToolPomodoroRow& pomodoro)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "INSERT INTO tool_pomodoro_runs (id, phase, status, cycle_index, completed_focus_count, "
        "long_break_every, focus_ms, short_break_ms, long_break_ms, phase_started_at_ms, "
        "phase_remaining_at_start_ms) "
        "VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "    phase = excluded.phase, status = excluded.status, cycle_index = excluded.cycle_index, "
        "    completed_focus_count = excluded.completed_focus_count, "
        "    long_break_every = excluded.long_break_every, focus_ms = excluded.focus_ms, "
        "    short_break_ms = excluded.short_break_ms, long_break_ms = excluded.long_break_ms, "
        "    phase_started_at_ms = excluded.phase_started_at_ms, "
        "    phase_remaining_at_start_ms = excluded.phase_remaining_at_start_ms"));
    query.addBindValue(toolPomodoroPhaseToText(pomodoro.phase));
    query.addBindValue(toolPomodoroStatusToText(pomodoro.status));
    query.addBindValue(pomodoro.cycleIndex);
    query.addBindValue(pomodoro.completedFocusCount);
    query.addBindValue(pomodoro.longBreakEvery);
    query.addBindValue(pomodoro.focusMs);
    query.addBindValue(pomodoro.shortBreakMs);
    query.addBindValue(pomodoro.longBreakMs);
    query.addBindValue(pomodoro.phaseStartedAtMs);
    query.addBindValue(pomodoro.phaseRemainingAtStartMs);
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

// —— 每日统计 ——

Result<int> ToolsRepository::dailyFocusCount(const QString& dateKey) const
{
    if (!_database.isOpen()) {
        return fail<int>(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "SELECT completed_focus_count FROM tool_daily_stats WHERE date_key = ?"));
    query.addBindValue(dateKey);
    if (!query.exec()) {
        return fail<int>(describeError(query));
    }
    if (!query.next()) {
        return succeed(0);
    }
    return succeed(query.value(0).toInt());
}

Status ToolsRepository::setDailyFocusCount(const QString& dateKey, int count)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "INSERT INTO tool_daily_stats (date_key, completed_focus_count) VALUES (?, ?) "
        "ON CONFLICT(date_key) DO UPDATE SET completed_focus_count = excluded.completed_focus_count"));
    query.addBindValue(dateKey);
    query.addBindValue(count);
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

// —— 活动提醒规则 ——

Result<QVector<ToolActivityReminderRule>> ToolsRepository::listActivityRules(
    bool includeDisabled) const
{
    if (!_database.isOpen()) {
        return fail<QVector<ToolActivityReminderRule>>(QStringLiteral("数据库未打开"));
    }

    QString sql = QString::fromLatin1(kRuleColumns);
    if (!includeDisabled) {
        sql += QStringLiteral(" WHERE disabled_at_ms IS NULL");
    }
    sql += QStringLiteral(" ORDER BY id ASC");

    QSqlQuery query(_database.connection());
    if (!query.exec(sql)) {
        return fail<QVector<ToolActivityReminderRule>>(describeError(query));
    }

    QVector<ToolActivityReminderRule> rules;
    while (query.next()) {
        rules.append(readRule(query));
    }
    return succeed(rules);
}

Result<ToolActivityReminderRule> ToolsRepository::createActivityRule(
    const ToolActivityReminderRule& rule)
{
    if (!_database.isOpen()) {
        return fail<ToolActivityReminderRule>(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "INSERT INTO tool_activity_reminder_rules ("
        "    target_kind, app_name, exe_name, category_id, label_snapshot, limit_ms, message, "
        "    created_at_ms, updated_at_ms, disabled_at_ms, last_fired_date_key"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, NULL, NULL)"));
    query.addBindValue(rule.targetKind);
    query.addBindValue(rule.appName.isEmpty() ? QVariant(QMetaType::fromType<QString>())
                                              : QVariant(rule.appName));
    query.addBindValue(rule.exeName.isEmpty() ? QVariant(QMetaType::fromType<QString>())
                                              : QVariant(rule.exeName));
    query.addBindValue(rule.categoryId.isEmpty() ? QVariant(QMetaType::fromType<QString>())
                                                 : QVariant(rule.categoryId));
    query.addBindValue(rule.labelSnapshot);
    query.addBindValue(rule.limitMs);
    query.addBindValue(rule.message);
    query.addBindValue(rule.createdAtMs);
    query.addBindValue(rule.updatedAtMs);
    if (!query.exec()) {
        return fail<ToolActivityReminderRule>(describeError(query));
    }

    const qint64 id = query.lastInsertId().toLongLong();
    QSqlQuery readBack(_database.connection());
    readBack.prepare(QString::fromLatin1(kRuleColumns) + QStringLiteral(" WHERE id = ?"));
    readBack.addBindValue(id);
    if (!readBack.exec() || !readBack.next()) {
        return fail<ToolActivityReminderRule>(describeError(readBack));
    }
    return succeed(readRule(readBack));
}

Status ToolsRepository::disableActivityRule(qint64 ruleId, qint64 nowMs)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "UPDATE tool_activity_reminder_rules SET disabled_at_ms = ?, updated_at_ms = ? "
        "WHERE id = ? AND disabled_at_ms IS NULL"));
    query.addBindValue(nowMs);
    query.addBindValue(nowMs);
    query.addBindValue(ruleId);
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

Status ToolsRepository::markActivityRuleFired(qint64 ruleId, const QString& dateKey, qint64 nowMs)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    // WHERE 带上 last_fired_date_key 守卫：并发 tick 下两个调用方同时抢触发，
    // 只有第一个 UPDATE 生效，第二个影响 0 行——通知不会重复弹。
    query.prepare(QStringLiteral(
        "UPDATE tool_activity_reminder_rules SET last_fired_date_key = ?, updated_at_ms = ? "
        "WHERE id = ? AND disabled_at_ms IS NULL "
        "  AND (last_fired_date_key IS NULL OR last_fired_date_key <> ?)"));
    query.addBindValue(dateKey);
    query.addBindValue(nowMs);
    query.addBindValue(ruleId);
    query.addBindValue(dateKey);
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

} // namespace patina::data
