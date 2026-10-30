#include "data/destination_read_model.h"

#include "data/app_category.h"
#include "data/app_links.h"
#include "data/category_registry.h"
#include "data/database.h"

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

#include <algorithm>
#include <optional>

namespace patina::data
{
namespace
{

// 单日明细的段数与标题样本数都要有上限。单日正常几百到几千段，
// 两万段已经是异常数据；样本按段数 ×8 估，超过就截断（时间轴照常画）。
constexpr int kMaxDestinationSessions = 20000;
constexpr int kMaxTitleSamplesPerBatch = 160000;

QString describeError(const QSqlQuery& query)
{
    const QSqlError error = query.lastError();
    return error.isValid() ? error.text() : QStringLiteral("SQL 执行失败");
}

QString dateKeyOf(QDate date)
{
    return date.toString(QStringLiteral("yyyy-MM-dd"));
}

// 一行原始记录（未裁剪）。
struct RawRecord
{
    qint64 startMs = 0;
    qint64 endMs = 0;
    QString title;
    QString exeName;
    bool isLive = false;
};

void appendRecord(QVector<DestinationRecord>& records, const RawRecord& raw,
                  qint64 dayStartMs, qint64 clipEndMs)
{
    const qint64 start = std::max(raw.startMs, dayStartMs);
    const qint64 end = std::min(raw.endMs, clipEndMs);
    if (end <= start) {
        return;
    }

    // 相邻段合并：同一应用内首尾相接的段并成一行，不再要求标题相同——
    // 窗口标题每次切换都会产生新段，按标题合并会把同一次使用切成十几行
    // 0m 碎片。合并行显示占用时间最长的标题。
    //
    // 只合并 gap 恰为 0 的段，并且必须同应用：曾经用「gap ≤ 180 秒」当合并
    // 门槛，结果是两个应用来回切换时中间的空隙（切走的那段时间）被算进了
    // 上一行的时长，详情合计比首页多出将近一倍（曾出现详情 22 分钟、首页
    // 4 分钟的对不上）。首尾相接的段之间没有空隙，合并后时长等于真实时长；
    // 不相接的段各自成行，明细行数会多一些，但「当日合计」与首页同口径。
    if (!records.isEmpty()) {
        DestinationRecord& previous = records.last();
        const bool sameApp = previous.exeName.compare(raw.exeName, Qt::CaseInsensitive) == 0;
        if (sameApp && start == previous.endTimeMs) {
            if (raw.title.size() > previous.title.size()) {
                previous.title = raw.title;
            }
            previous.endTimeMs = end;
            previous.durationMs += end - start;
            previous.isLive = previous.isLive || raw.isLive;
            return;
        }
    }

    DestinationRecord record;
    record.startTimeMs = start;
    record.endTimeMs = end;
    record.durationMs = end - start;
    record.title = raw.title;
    record.exeName = raw.exeName;
    record.isLive = raw.isLive;
    records.append(record);
}

} // namespace

Result<QStringList> DestinationReadModel::categoryMembers(const QString& categoryId,
                                                          QDate date) const
{
    if (!_database.isOpen()) {
        return fail<QStringList>(QStringLiteral("数据库未打开"));
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 dayStartMs = QDateTime(date.startOfDay()).toMSecsSinceEpoch();
    const qint64 dayEndMs = QDateTime(date.addDays(1).startOfDay()).toMSecsSinceEpoch();

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "SELECT a.exe_name, a.category_id, "
        "       COALESCE(SUM(MIN(COALESCE(s.end_time_ms, :now), :dayEnd) - MAX(s.start_time_ms, :dayStart)), 0) "
        "FROM sessions s JOIN apps a ON a.id = s.app_id "
        "WHERE a.track_enabled = 1 AND a.hidden = 0 AND s.start_time_ms < :dayEnd "
        "  AND COALESCE(s.end_time_ms, :now2) > :dayStart "
        "GROUP BY a.exe_name, a.category_id"));
    query.bindValue(QStringLiteral(":now"), nowMs);
    query.bindValue(QStringLiteral(":now2"), nowMs);
    query.bindValue(QStringLiteral(":dayEnd"), dayEndMs);
    query.bindValue(QStringLiteral(":dayStart"), dayStartMs);
    if (!query.exec()) {
        return fail<QStringList>(describeError(query));
    }

    struct Member
    {
        QString exeName;
        qint64 durationMs = 0;
    };
    QVector<Member> members;
    // 成员判定走注册表快照——目标分类若是墓碑/悬空 id，成员已在删除时
    // 回退，这里按解析后的 id 比对（悬空引用落 other，与统计同口径）。
    const Result<CategoryRegistrySnapshot> registry = loadCategoryRegistrySnapshot(_database);
    // 关联应用成员的分类归父应用——否则父应用在「开发」，成员在「其他」，
    // 点开两个分类都能看到这个应用的一半。
    const Result<AppLinks> loadedLinks = loadAppLinks(_database);
    const AppLinks links = loadedLinks.ok() ? loadedLinks.value : AppLinks{};
    QHash<QString, AppIdentity> identities;
    if (!links.isEmpty()) {
        const Result<QHash<QString, AppIdentity>> loadedIdentities =
            loadAppIdentities(_database);
        if (!loadedIdentities.ok()) {
            return fail<QStringList>(loadedIdentities.error);
        }
        identities = loadedIdentities.value;
    }
    while (query.next()) {
        const QString exeName = query.value(0).toString();
        const QString overrideId = query.value(1).toString();
        QString resolved;
        if (const std::optional<AppIdentity> parent = linkedIdentity(exeName, links, identities)) {
            resolved = resolveCategoryId(parent->exeName, parent->categoryId);
        } else {
            resolved = resolveCategoryId(exeName, overrideId);
        }
        if (registry.ok()) {
            resolved = registry.value.resolveId(resolved);
        }
        if (resolved != categoryId) {
            continue;
        }
        members.append({exeName, query.value(2).toLongLong()});
    }
    std::sort(members.begin(), members.end(),
              [](const Member& left, const Member& right) {
                  return left.durationMs > right.durationMs;
              });

    QStringList result;
    for (const Member& member : members) {
        result.append(member.exeName);
    }
    return succeed(result);
}

Result<DestinationDetailDay> DestinationReadModel::loadDay(const DestinationTarget& target,
                                                           QDate date,
                                                           qint64 minDurationMs) const
{
    if (!_database.isOpen()) {
        return fail<DestinationDetailDay>(QStringLiteral("数据库未打开"));
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 dayStartMs = QDateTime(date.startOfDay()).toMSecsSinceEpoch();
    const qint64 requestedEndMs = QDateTime(date.addDays(1).startOfDay()).toMSecsSinceEpoch();
    const qint64 clipEndMs = std::min(requestedEndMs, nowMs);

    DestinationDetailDay day;
    day.dateKey = dateKeyOf(date);
    day.dayStartMs = dayStartMs;
    day.dayEndMs = requestedEndMs;
    if (clipEndMs <= dayStartMs) {
        return succeed(day);
    }

    // 目标成员集合：应用维度是它自己（加上它名下关联的成员）；
    // 分类维度先解析成员。
    QStringList exeNames;
    if (target.mode == DestinationTarget::Mode::App) {
        const Result<AppLinks> loadedLinks = loadAppLinks(_database);
        const AppLinks links = loadedLinks.ok() ? loadedLinks.value : AppLinks{};
        exeNames = linkedAppKeys(target.key, links);
        if (exeNames.isEmpty()) {
            exeNames.append(target.key);
        }
    } else {
        const Result<QStringList> members = categoryMembers(target.key, date);
        if (!members.ok()) {
            return fail<DestinationDetailDay>(members.error);
        }
        exeNames = members.value;
    }
    if (exeNames.isEmpty()) {
        return succeed(day);
    }

    // Qt 的 QSqlQuery 同一条语句里不能混用位置占位符（? + addBindValue）
    // 与命名占位符（:name + bindValue）——SQLite 驱动会把两者计入同一绑定
    // 序列，总数对不上时 SQLite 直接抛 "Parameter count mismatch"（应用分布
    // 点开详情必炸）。成员列表也改成命名占位符。
    QString memberClause;
    QStringList memberKeys;
    for (int index = 0; index < exeNames.size(); ++index) {
        const QString key = QStringLiteral(":m%1").arg(index);
        if (index > 0) {
            memberClause += QStringLiteral(", ");
        }
        memberClause += key;
        memberKeys.append(key);
    }

    QSqlQuery query(_database.connection());
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
        "SELECT s.id, s.app_id, s.window_title, s.start_time_ms, s.end_time_ms, a.exe_name "
        "FROM sessions s JOIN apps a ON a.id = s.app_id "
        "WHERE a.exe_name COLLATE NOCASE IN (%1) AND a.track_enabled = 1 AND a.hidden = 0 "
        "  AND s.start_time_ms < :clipEnd AND COALESCE(s.end_time_ms, :now) > :dayStart "
        "ORDER BY s.start_time_ms ASC, s.id ASC "
        "LIMIT %2").arg(memberClause).arg(kMaxDestinationSessions));
    for (int index = 0; index < exeNames.size(); ++index) {
        query.bindValue(memberKeys.at(index), exeNames.at(index));
    }
    query.bindValue(QStringLiteral(":clipEnd"), clipEndMs);
    query.bindValue(QStringLiteral(":now"), nowMs);
    query.bindValue(QStringLiteral(":dayStart"), dayStartMs);
    if (!query.exec()) {
        return fail<DestinationDetailDay>(describeError(query));
    }

    struct SessionRow
    {
        qint64 id = 0;
        QString windowTitle;
        qint64 startMs = 0;
        qint64 endMs = 0;
        bool isLive = false;
        QString exeName;
    };
    QVector<SessionRow> sessions;
    QVector<qint64> sessionIds;
    while (query.next()) {
        SessionRow row;
        row.id = query.value(0).toLongLong();
        row.windowTitle = query.value(2).toString();
        row.startMs = query.value(3).toLongLong();
        row.isLive = query.value(4).isNull();
        row.endMs = row.isLive ? clipEndMs : query.value(4).toLongLong();
        row.exeName = query.value(5).toString();
        sessions.append(row);
        sessionIds.append(row.id);
    }
    if (sessions.isEmpty()) {
        return succeed(day);
    }

    // 同 loadDay：成员列表用命名占位符（:s0 :s1 ...），不与任何位置绑定混用。
    QString sampleClause;
    QStringList sampleKeys;
    for (int index = 0; index < sessionIds.size(); ++index) {
        const QString key = QStringLiteral(":s%1").arg(index);
        if (index > 0) {
            sampleClause += QStringLiteral(", ");
        }
        sampleClause += key;
        sampleKeys.append(key);
    }
    QSqlQuery sampleQuery(_database.connection());
    sampleQuery.setForwardOnly(true);
    sampleQuery.prepare(QStringLiteral(
        "SELECT session_id, title, start_time_ms, end_time_ms "
        "FROM session_title_samples "
        "WHERE session_id IN (%1) "
        "ORDER BY session_id ASC, start_time_ms ASC, id ASC "
        "LIMIT %2").arg(sampleClause).arg(kMaxTitleSamplesPerBatch));
    for (int index = 0; index < sessionIds.size(); ++index) {
        sampleQuery.bindValue(sampleKeys.at(index), sessionIds.at(index));
    }
    if (!sampleQuery.exec()) {
        return fail<DestinationDetailDay>(describeError(sampleQuery));
    }

    QHash<qint64, QVector<RawRecord>> samplesBySession;
    QHash<qint64, qint64> endBySession;
    for (const SessionRow& row : sessions) {
        endBySession.insert(row.id, row.endMs);
    }
    while (sampleQuery.next()) {
        const qint64 sessionId = sampleQuery.value(0).toLongLong();
        RawRecord sample;
        sample.title = sampleQuery.value(1).toString().trimmed();
        sample.startMs = sampleQuery.value(2).toLongLong();
        const QVariant endValue = sampleQuery.value(3);
        // 样本未闭合时按段终点封底。
        sample.endMs = endValue.isNull() ? endBySession.value(sessionId) : endValue.toLongLong();
        samplesBySession[sessionId].append(sample);
    }

    // 组明细行：有样本按样本切；无样本整段一行（用段标题，可能为空）。
    QVector<DestinationRecord> allRecords;
    for (const SessionRow& row : sessions) {
        const QVector<RawRecord> samples = samplesBySession.value(row.id);
        if (samples.isEmpty()) {
            RawRecord raw;
            raw.startMs = row.startMs;
            raw.endMs = row.endMs;
            raw.title = row.windowTitle.trimmed();
            raw.exeName = row.exeName;
            raw.isLive = row.isLive;
            appendRecord(allRecords, raw, dayStartMs, clipEndMs);
            continue;
        }

        qint64 cursor = std::max<qint64>(row.startMs, dayStartMs);
        for (const RawRecord& sample : samples) {
            const qint64 sampleStart = std::max<qint64>(sample.startMs, dayStartMs);
            const qint64 sampleEnd = std::min<qint64>(sample.endMs, clipEndMs);
            if (sampleStart > cursor) {
                RawRecord gap;
                gap.startMs = cursor;
                gap.endMs = sampleStart;
                gap.exeName = row.exeName;
                appendRecord(allRecords, gap, dayStartMs, clipEndMs);
            }
            RawRecord clipped = sample;
            clipped.startMs = std::max(cursor, sampleStart);
            clipped.endMs = sampleEnd;
            clipped.exeName = row.exeName;
            clipped.isLive = row.isLive && sampleEnd >= clipEndMs;
            appendRecord(allRecords, clipped, dayStartMs, clipEndMs);
            cursor = std::max(cursor, sampleEnd);
        }
        if (cursor < row.endMs) {
            RawRecord tail;
            tail.startMs = cursor;
            tail.endMs = row.endMs;
            tail.exeName = row.exeName;
            tail.isLive = row.isLive;
            appendRecord(allRecords, tail, dayStartMs, clipEndMs);
        }
    }

    // 最短时长过滤只作用于明细行（时间轴不受影响）。
    for (const DestinationRecord& record : allRecords) {
        if (minDurationMs > 0 && record.durationMs < minDurationMs) {
            continue;
        }
        day.records.append(record);
        day.totalDurationMs += record.durationMs;
    }
    return succeed(day);
}

} // namespace patina::data
