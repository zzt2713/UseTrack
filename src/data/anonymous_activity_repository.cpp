#include "data/anonymous_activity_repository.h"

#include "data/database.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
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

struct OpenAnonymousSegment
{
    QString id;
    qint64 startTimeMs = 0;
    qint64 observedUntilMs = 0;
};

} // namespace

Result<bool> AnonymousActivityRepository::observe(qint64 nowMs)
{
    if (!_database.isOpen()) {
        return fail<bool>(QStringLiteral("数据库未打开"));
    }
    if (nowMs <= 0) {
        return fail<bool>(QStringLiteral("采样时间非法"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "SELECT id, start_time_ms, observed_until_ms FROM anonymous_activity "
        "WHERE end_time_ms IS NULL LIMIT 1"));
    if (!query.exec()) {
        return fail<bool>(describeError(query));
    }

    std::optional<OpenAnonymousSegment> open;
    if (query.next()) {
        OpenAnonymousSegment segment;
        segment.id = query.value(0).toString();
        segment.startTimeMs = query.value(1).toLongLong();
        segment.observedUntilMs = query.value(2).toLongLong();
        open = segment;
    }

    if (open.has_value()) {
        // 时钟回拨：不写库，也不新开段。已有的未闭合段继续等墙钟追上来。
        if (nowMs < open->observedUntilMs) {
            return succeed(false);
        }
        QSqlQuery update(_database.connection());
        update.prepare(
            QStringLiteral("UPDATE anonymous_activity SET observed_until_ms = ? WHERE id = ?"));
        update.addBindValue(nowMs);
        update.addBindValue(open->id);
        if (!update.exec()) {
            return fail<bool>(describeError(update));
        }
        return succeed(false);
    }

    QSqlQuery insert(_database.connection());
    insert.prepare(QStringLiteral(
        "INSERT INTO anonymous_activity (id, start_time_ms, observed_until_ms, end_time_ms) "
        "VALUES (?, ?, ?, NULL)"));
    insert.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
    insert.addBindValue(nowMs);
    insert.addBindValue(nowMs);
    if (!insert.exec()) {
        return fail<bool>(describeError(insert));
    }
    return succeed(true);
}

Status AnonymousActivityRepository::seal(qint64 cutoffMs)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    // 终点不早于起点，也不早于已观察到的时刻：观察过的时间不该被回溯抹掉。
    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "UPDATE anonymous_activity "
        "SET end_time_ms = MAX(start_time_ms, MAX(observed_until_ms, ?)), "
        "    observed_until_ms = MAX(observed_until_ms, ?) "
        "WHERE end_time_ms IS NULL"));
    query.addBindValue(cutoffMs);
    query.addBindValue(cutoffMs);
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

Status AnonymousActivityRepository::sealInterrupted(qint64 cutoffMs)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    // 只有「观察到的时刻」落在 [起点, cutoff] 之内才采信它；否则说明观察
    // 记录本身不可信（时钟回拨/更晚的 cutoff），退回起点，不凭空造时长。
    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "UPDATE anonymous_activity "
        "SET end_time_ms = CASE WHEN observed_until_ms BETWEEN start_time_ms AND ? "
        "THEN observed_until_ms ELSE start_time_ms END "
        "WHERE end_time_ms IS NULL"));
    query.addBindValue(cutoffMs);
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

Result<qint64> AnonymousActivityRepository::totalClosedDurationMs(qint64 fromMs, qint64 toMs) const
{
    if (!_database.isOpen()) {
        return fail<qint64>(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "SELECT COALESCE(SUM(MIN(end_time_ms, ?) - MAX(start_time_ms, ?)), 0) "
        "FROM anonymous_activity "
        "WHERE end_time_ms IS NOT NULL AND start_time_ms < ? AND end_time_ms > ?"));
    query.addBindValue(toMs);
    query.addBindValue(fromMs);
    query.addBindValue(toMs);
    query.addBindValue(fromMs);
    if (!query.exec()) {
        return fail<qint64>(describeError(query));
    }
    if (!query.next()) {
        return succeed(qint64{0});
    }
    return succeed(query.value(0).toLongLong());
}

} // namespace patina::data
