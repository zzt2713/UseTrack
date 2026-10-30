#include "data/session_repository.h"

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

const char* const kSelectColumns =
    "SELECT id, app_id, window_title, start_time_ms, end_time_ms, "
    "continuity_group_start_time_ms FROM sessions";

// 关掉标题录制时 engine 传下来的是默认构造的 null QString，QVariant 会把它
// 绑成 SQL NULL，撞上 window_title 的 NOT NULL 约束——整条写事务跟着回滚，
// 连带同一事务里的心跳时间戳都写不进去。空标题要落成空串，不是 NULL。
QVariant titleBinding(const QString& windowTitle)
{
    return windowTitle.isNull() ? QStringLiteral("") : windowTitle;
}

SessionSegment readSegment(const QSqlQuery& query)
{
    SessionSegment segment;
    segment.id = query.value(0).toLongLong();
    segment.appId = query.value(1).toLongLong();
    segment.windowTitle = query.value(2).toString();
    segment.startTimeMs = query.value(3).toLongLong();
    if (!query.value(4).isNull()) {
        segment.endTimeMs = query.value(4).toLongLong();
    }
    // 老数据在 v4 迁移里已回填成 start_time_ms，这里再兜一次 NULL，
    // 免得读到空值时连续性判断被一个 0 带偏。
    const QVariant groupStart = query.value(5);
    segment.continuityGroupStartTimeMs =
        groupStart.isNull() ? segment.startTimeMs : groupStart.toLongLong();
    return segment;
}

} // namespace

Result<SessionSegment> SessionRepository::openSegment(qint64 appId,
                                                      const QString& windowTitle,
                                                      qint64 startTimeMs,
                                                      qint64 continuityGroupStartTimeMs)
{
    if (!_database.isOpen()) {
        return fail<SessionSegment>(QStringLiteral("数据库未打开"));
    }
    if (appId <= 0) {
        return fail<SessionSegment>(QStringLiteral("app id 非法"));
    }
    if (startTimeMs <= 0) {
        return fail<SessionSegment>(QStringLiteral("开始时间非法"));
    }

    // 先自己判一次，好给出比“UNIQUE constraint failed”更可读的错误；
    // 真正的防重仍由部分唯一索引兜底（并发下这里可能判漏）。
    const Result<std::optional<SessionSegment>> existing = findOpen();
    if (!existing.ok()) {
        return fail<SessionSegment>(existing.error);
    }
    if (existing.value.has_value()) {
        return fail<SessionSegment>(
            QStringLiteral("已有未闭合的段 %1，必须先闭合").arg(existing.value->id));
    }

    // 组起点不得晚于段起点：切回原应用时传的组起点可能比本段起点更早，
    // 那是正常的；但如果传进来一个更晚的值，说明调用方算错了，夹回来。
    const qint64 groupStartMs =
        (continuityGroupStartTimeMs > 0 && continuityGroupStartTimeMs < startTimeMs)
            ? continuityGroupStartTimeMs
            : startTimeMs;

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "INSERT INTO sessions (app_id, window_title, start_time_ms, end_time_ms, "
        "continuity_group_start_time_ms) VALUES (?, ?, ?, NULL, ?)"));
    query.addBindValue(appId);
    query.addBindValue(titleBinding(windowTitle));
    query.addBindValue(startTimeMs);
    query.addBindValue(groupStartMs);
    if (!query.exec()) {
        return fail<SessionSegment>(describeError(query));
    }

    const qint64 id = query.lastInsertId().toLongLong();
    const Result<std::optional<SessionSegment>> created = findById(id);
    if (!created.ok()) {
        return fail<SessionSegment>(created.error);
    }
    if (!created.value.has_value()) {
        return fail<SessionSegment>(QStringLiteral("写入段落后读回失败"));
    }
    return succeed(*created.value);
}

Status SessionRepository::closeOpen(qint64 endTimeMs)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }
    if (endTimeMs <= 0) {
        return Status::failure(QStringLiteral("结束时间非法"));
    }

    QSqlQuery query(_database.connection());
    // WHERE end_time_ms IS NULL 让这条语句天然幂等：重复闭合不会改动已闭合段。
    // 时间回拨导致 end < start 时不动手——留给调用方判断，不静默写坏数据。
    query.prepare(QStringLiteral(
        "UPDATE sessions SET end_time_ms = ? "
        "WHERE end_time_ms IS NULL AND start_time_ms <= ?"));
    query.addBindValue(endTimeMs);
    query.addBindValue(endTimeMs);
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

Status SessionRepository::updateOpenTitle(const QString& windowTitle)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "UPDATE sessions SET window_title = ? WHERE end_time_ms IS NULL"));
    query.addBindValue(titleBinding(windowTitle));
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

Result<std::optional<SessionSegment>> SessionRepository::findOpen() const
{
    if (!_database.isOpen()) {
        return fail<std::optional<SessionSegment>>(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    if (!query.exec(QString::fromLatin1(kSelectColumns)
                    + QStringLiteral(" WHERE end_time_ms IS NULL LIMIT 1"))) {
        return fail<std::optional<SessionSegment>>(describeError(query));
    }
    if (!query.next()) {
        return succeed(std::optional<SessionSegment>{});
    }
    return succeed(std::optional<SessionSegment>{readSegment(query)});
}

Result<std::optional<SessionSegment>> SessionRepository::findById(qint64 id) const
{
    if (!_database.isOpen()) {
        return fail<std::optional<SessionSegment>>(QStringLiteral("数据库未打开"));
    }
    if (id <= 0) {
        return fail<std::optional<SessionSegment>>(QStringLiteral("段 id 非法"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QString::fromLatin1(kSelectColumns) + QStringLiteral(" WHERE id = ?"));
    query.addBindValue(id);
    if (!query.exec()) {
        return fail<std::optional<SessionSegment>>(describeError(query));
    }
    if (!query.next()) {
        return succeed(std::optional<SessionSegment>{});
    }
    return succeed(std::optional<SessionSegment>{readSegment(query)});
}

Result<QVector<SessionSegment>> SessionRepository::listRange(
    qint64 fromMs, qint64 toMs, int limit) const
{
    if (!_database.isOpen()) {
        return fail<QVector<SessionSegment>>(QStringLiteral("数据库未打开"));
    }
    if (fromMs < 0 || toMs < fromMs) {
        return fail<QVector<SessionSegment>>(QStringLiteral("时间区间非法"));
    }
    if (limit <= 0) {
        return fail<QVector<SessionSegment>>(QStringLiteral("limit 必须为正"));
    }

    QSqlQuery query(_database.connection());
    // 与区间有交叠的段都要：起点早于区间尾、且（未闭合或终点晚于区间头）。
    // 这样跨零点的段不会在按天查询时丢掉。
    query.prepare(QString::fromLatin1(kSelectColumns)
                  + QStringLiteral(" WHERE start_time_ms < ?"
                                   " AND (end_time_ms IS NULL OR end_time_ms > ?)"
                                   " ORDER BY start_time_ms ASC, id ASC LIMIT ?"));
    query.addBindValue(toMs);
    query.addBindValue(fromMs);
    query.addBindValue(limit);
    if (!query.exec()) {
        return fail<QVector<SessionSegment>>(describeError(query));
    }

    QVector<SessionSegment> segments;
    while (query.next()) {
        segments.append(readSegment(query));
    }
    return succeed(segments);
}

Result<qint64> SessionRepository::totalClosedDurationMs(qint64 fromMs, qint64 toMs) const
{
    if (!_database.isOpen()) {
        return fail<qint64>(QStringLiteral("数据库未打开"));
    }
    if (fromMs < 0 || toMs < fromMs) {
        return fail<qint64>(QStringLiteral("时间区间非法"));
    }

    QSqlQuery query(_database.connection());
    // 只累加交叠部分，不把整段时长算给这一天；MAX/MIN 夹在区间边界内。
    query.prepare(QStringLiteral(
        "SELECT COALESCE(SUM(MIN(end_time_ms, ?) - MAX(start_time_ms, ?)), 0) "
        "FROM sessions "
        "WHERE end_time_ms IS NOT NULL "
        "  AND start_time_ms < ? AND end_time_ms > ?"));
    query.addBindValue(toMs);
    query.addBindValue(fromMs);
    query.addBindValue(toMs);
    query.addBindValue(fromMs);
    if (!query.exec()) {
        return fail<qint64>(describeError(query));
    }
    if (!query.next()) {
        return succeed<qint64>(0);
    }
    return succeed(query.value(0).toLongLong());
}

} // namespace patina::data
