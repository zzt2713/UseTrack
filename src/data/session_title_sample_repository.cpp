#include "data/session_title_sample_repository.h"

#include "data/database.h"

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

} // namespace

Status SessionTitleSampleRepository::start(qint64 sessionId,
                                           const QString& title,
                                           qint64 startTimeMs)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }
    if (sessionId <= 0) {
        return Status::failure(QStringLiteral("会话 id 非法"));
    }

    // 关掉标题录制时 engine 传的是空标题：不写样本，也不算失败。
    const QString trimmed = title.trimmed();
    if (trimmed.isEmpty()) {
        return Status::success();
    }

    // 样本起点不能早于所在段起点：AFK 回溯与时钟抖动都可能把时间推到段前。
    QSqlQuery sessionQuery(_database.connection());
    sessionQuery.prepare(QStringLiteral("SELECT start_time_ms FROM sessions WHERE id = ? LIMIT 1"));
    sessionQuery.addBindValue(sessionId);
    if (!sessionQuery.exec()) {
        return Status::failure(describeError(sessionQuery));
    }
    if (!sessionQuery.next()) {
        // 段已经被删掉了，样本没有归属，静默跳过。
        return Status::success();
    }
    const qint64 sessionStartMs = sessionQuery.value(0).toLongLong();
    const qint64 clampedStartMs = startTimeMs > sessionStartMs ? startTimeMs : sessionStartMs;

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "INSERT INTO session_title_samples (session_id, title, start_time_ms, end_time_ms) "
        "VALUES (?, ?, ?, NULL)"));
    query.addBindValue(sessionId);
    query.addBindValue(trimmed);
    query.addBindValue(clampedStartMs);
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

Status SessionTitleSampleRepository::finishActive(qint64 sessionId, qint64 endTimeMs)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }
    if (sessionId <= 0) {
        return Status::success();
    }

    // 只把「还没闭合、或闭合点比这次更晚」的样本收掉：
    // 重复封口是幂等的，也不会把已经收好的样本越改越短。
    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "UPDATE session_title_samples "
        "SET end_time_ms = CASE WHEN ? < start_time_ms THEN start_time_ms ELSE ? END "
        "WHERE session_id = ? "
        "  AND (end_time_ms IS NULL OR end_time_ms > MAX(start_time_ms, ?))"));
    query.addBindValue(endTimeMs);
    query.addBindValue(endTimeMs);
    query.addBindValue(sessionId);
    query.addBindValue(endTimeMs);
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

Status SessionTitleSampleRepository::replaceActive(qint64 sessionId,
                                                   const QString& title,
                                                   qint64 timestampMs)
{
    const Status finished = finishActive(sessionId, timestampMs);
    if (!finished.ok()) {
        return finished;
    }
    return start(sessionId, title, timestampMs);
}

Result<QVector<SessionTitleSample>> SessionTitleSampleRepository::listBySession(qint64 sessionId) const
{
    if (!_database.isOpen()) {
        return fail<QVector<SessionTitleSample>>(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "SELECT id, session_id, title, start_time_ms, end_time_ms "
        "FROM session_title_samples WHERE session_id = ? "
        "ORDER BY start_time_ms ASC, id ASC LIMIT 500"));
    query.addBindValue(sessionId);
    if (!query.exec()) {
        return fail<QVector<SessionTitleSample>>(describeError(query));
    }

    QVector<SessionTitleSample> samples;
    while (query.next()) {
        SessionTitleSample sample;
        sample.id = query.value(0).toLongLong();
        sample.sessionId = query.value(1).toLongLong();
        sample.title = query.value(2).toString();
        sample.startTimeMs = query.value(3).toLongLong();
        if (!query.value(4).isNull()) {
            sample.endTimeMs = query.value(4).toLongLong();
        }
        samples.append(sample);
    }
    return succeed(samples);
}

} // namespace patina::data
