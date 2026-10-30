#include "data/app_options_read_model.h"

#include "data/app_links.h"
#include "data/database.h"

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QSet>
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

// GROUP BY 之后行数等于应用数，本来就有限；这里再压一道显式上限。
constexpr int kMaxAppOptions = 4096;
// 关联应用的活跃天数要按「并集」算，不能把成员的活跃天数相加（同一天两个成员
// 都活跃只算一天）。只对参与关联的那几个 exe 单独查日键，行数 = 参与关联的
// exe 数 × 区间天数，量级很小；上限仍是防御性的。
constexpr int kMaxAppOptionDays = 20000;

QString describeError(const QSqlQuery& query)
{
    const QSqlError error = query.lastError();
    return error.isValid() ? error.text() : QStringLiteral("SQL 执行失败");
}

} // namespace

Result<QVector<AppOption>> AppOptionsReadModel::loadOptions(qint64 rangeStartMs,
                                                            qint64 rangeEndMs) const
{
    if (!_database.isOpen()) {
        return fail<QVector<AppOption>>(QStringLiteral("数据库未打开"));
    }
    if (rangeEndMs <= rangeStartMs) {
        return succeed(QVector<AppOption>{});
    }

    // 关联应用（成员并到父应用）是读时归并口径，SQL 仍按 exe 分组。
    const Result<AppLinks> loadedLinks = loadAppLinks(_database);
    const AppLinks links = loadedLinks.ok() ? loadedLinks.value : AppLinks{};
    QHash<QString, AppIdentity> identities;
    if (!links.isEmpty()) {
        const Result<QHash<QString, AppIdentity>> loadedIdentities =
            loadAppIdentities(_database);
        if (!loadedIdentities.ok()) {
            return fail<QVector<AppOption>>(loadedIdentities.error);
        }
        identities = loadedIdentities.value;
    }

    // 一条聚合查询出「区间内每个应用的合计与活跃天数」。
    // 活跃天数按日切桶（与 DataReadModel 同口径：段按本地日边界切开分给各天），
    // SQL 的 DATE(timestamp, 'unixepoch', 'localtime) 对毫秒时间戳要除 1000，
    // 直接在 SQL 里算日键，避免把全区间段拉回内存。
    QSqlQuery query(_database.connection());
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
        "SELECT a.exe_name, a.display_name, a.category_id, a.color_hex, "
        "       COALESCE(SUM(MIN(COALESCE(s.end_time_ms, :end), :end2) - MAX(s.start_time_ms, :start)), 0), "
        "       COUNT(DISTINCT DATE(s.start_time_ms / 1000, 'unixepoch', 'localtime')) "
        "FROM sessions s JOIN apps a ON a.id = s.app_id "
        "WHERE a.track_enabled = 1 AND a.hidden = 0 "
        "  AND s.start_time_ms < :end3 "
        "  AND COALESCE(s.end_time_ms, :now) > :start2 "
        "GROUP BY a.exe_name, a.display_name, a.category_id, a.color_hex "
        "HAVING SUM(MIN(COALESCE(s.end_time_ms, :end4), :end5) - MAX(s.start_time_ms, :start3)) > 0 "
        "ORDER BY 5 DESC "
        "LIMIT %1").arg(kMaxAppOptions));
    query.bindValue(QStringLiteral(":end"), rangeEndMs);
    query.bindValue(QStringLiteral(":end2"), rangeEndMs);
    query.bindValue(QStringLiteral(":end3"), rangeEndMs);
    query.bindValue(QStringLiteral(":end4"), rangeEndMs);
    query.bindValue(QStringLiteral(":end5"), rangeEndMs);
    query.bindValue(QStringLiteral(":start"), rangeStartMs);
    query.bindValue(QStringLiteral(":start2"), rangeStartMs);
    query.bindValue(QStringLiteral(":start3"), rangeStartMs);
    query.bindValue(QStringLiteral(":now"), QDateTime::currentMSecsSinceEpoch());
    if (!query.exec()) {
        return fail<QVector<AppOption>>(describeError(query));
    }

    QVector<AppOption> options;
    while (query.next()) {
        AppOption option;
        option.exeName = query.value(0).toString();
        option.displayName = query.value(1).toString();
        option.categoryId = query.value(2).toString();
        option.colorHex = query.value(3).toString();
        option.totalDurationMs = query.value(4).toLongLong();
        option.activeDayCount = query.value(5).toInt();
        if (option.totalDurationMs > 0) {
            options.append(option);
        }
    }

    if (links.isEmpty() || options.isEmpty()) {
        return succeed(options);
    }

    // 参与关联的 exe（成员 + 父）的日键集合：用来把活跃天数按并集重算。
    QStringList linkedExeNames;
    QSet<QString> seen;
    for (auto it = links.cbegin(); it != links.cend(); ++it) {
        for (const QString& exe : {it.key(), it.value()}) {
            if (seen.contains(exe)) {
                continue;
            }
            seen.insert(exe);
            linkedExeNames.append(exe);
        }
    }
    QHash<QString, QSet<QString>> dayKeys;
    {
        QStringList placeholders;
        placeholders.reserve(linkedExeNames.size());
        for (int index = 0; index < linkedExeNames.size(); ++index) {
            placeholders.append(QStringLiteral(":d%1").arg(index));
        }
        QSqlQuery dayQuery(_database.connection());
        dayQuery.setForwardOnly(true);
        dayQuery.prepare(QStringLiteral(
            "SELECT a.exe_name, DATE(s.start_time_ms / 1000, 'unixepoch', 'localtime') "
            "FROM sessions s JOIN apps a ON a.id = s.app_id "
            "WHERE a.track_enabled = 1 AND a.hidden = 0 "
            "  AND a.exe_name COLLATE NOCASE IN (%1) "
            "  AND s.start_time_ms < :end "
            "  AND COALESCE(s.end_time_ms, :now) > :start "
            "GROUP BY a.exe_name, 2 "
            "LIMIT %2").arg(placeholders.join(QStringLiteral(", ")))
                              .arg(kMaxAppOptionDays));
        for (int index = 0; index < linkedExeNames.size(); ++index) {
            dayQuery.bindValue(QStringLiteral(":d%1").arg(index), linkedExeNames.at(index));
        }
        dayQuery.bindValue(QStringLiteral(":end"), rangeEndMs);
        // 与主聚合查询同一口径：未闭合段的「现在」取真实墙钟，不用区间右端。
        dayQuery.bindValue(QStringLiteral(":now"), QDateTime::currentMSecsSinceEpoch());
        dayQuery.bindValue(QStringLiteral(":start"), rangeStartMs);
        if (!dayQuery.exec()) {
            return fail<QVector<AppOption>>(describeError(dayQuery));
        }
        while (dayQuery.next()) {
            const QString exe = dayQuery.value(0).toString().trimmed().toLower();
            const QString dayKey = dayQuery.value(1).toString();
            if (exe.isEmpty() || dayKey.isEmpty()) {
                continue;
            }
            dayKeys[exe].insert(dayKey);
        }
    }

    // 归并：同一个父应用只保留一行，时长相加，活跃天数取日键并集。
    QHash<QString, int> optionIndex;
    QVector<AppOption> merged;
    merged.reserve(options.size());
    QVector<QSet<QString>> mergedDays;
    for (const AppOption& option : options) {
        const QString key = statisticalAppKey(option.exeName, links);
        const auto existing = optionIndex.constFind(key);
        if (existing != optionIndex.constEnd()) {
            AppOption& target = merged[existing.value()];
            target.totalDurationMs += option.totalDurationMs;
            mergedDays[existing.value()].unite(dayKeys.value(option.exeName.trimmed().toLower()));
            continue;
        }
        AppOption row = option;
        if (const std::optional<AppIdentity> parent = linkedIdentity(option.exeName, links, identities)) {
            row.exeName = parent->exeName;
            row.displayName = parent->displayName;
            row.categoryId = parent->categoryId;
            row.colorHex = parent->colorHex;
        }
        optionIndex.insert(key, merged.size());
        merged.append(row);
        mergedDays.append(dayKeys.value(option.exeName.trimmed().toLower()));
    }
    for (int index = 0; index < merged.size(); ++index) {
        if (!mergedDays[index].isEmpty()) {
            merged[index].activeDayCount = static_cast<int>(mergedDays[index].size());
        }
    }
    std::sort(merged.begin(), merged.end(), [](const AppOption& left, const AppOption& right) {
        if (left.totalDurationMs != right.totalDurationMs) {
            return left.totalDurationMs > right.totalDurationMs;
        }
        return left.exeName.compare(right.exeName, Qt::CaseInsensitive) < 0;
    });
    return succeed(merged);
}

} // namespace patina::data
