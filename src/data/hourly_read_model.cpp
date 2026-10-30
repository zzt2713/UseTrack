#include "data/hourly_read_model.h"

#include "data/app_category.h"
#include "data/app_links.h"
#include "data/category_registry.h"
#include "data/database.h"

#include <QDateTime>
#include <QHash>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>
#include <optional>

namespace patina::data
{
namespace
{

// 单日区间扫描也要有硬上限。正常一天是几百到几千段，
// 12 万段已经是「每 0.7 秒切一次段」的极端情形。
constexpr int kMaxHourlySegments = 120000;

QString describeError(const QSqlQuery& query)
{
    const QSqlError error = query.lastError();
    return error.isValid() ? error.text() : QStringLiteral("SQL 执行失败");
}

// 把一段交叠时间按小时分桶。start/end 已夹在查询区间内。
void addToBuckets(qint64 startMs, qint64 endMs, qint64* buckets)
{
    if (endMs <= startMs) {
        return;
    }
    QDateTime moment = QDateTime::fromMSecsSinceEpoch(startMs);
    qint64 cursor = startMs;
    while (cursor < endMs) {
        const QDateTime hourStart(moment.date(), QTime(moment.time().hour(), 0));
        const qint64 hourEndMs = hourStart.addSecs(3600).toMSecsSinceEpoch();
        const qint64 sliceEnd = std::min(endMs, hourEndMs);
        const int hour = moment.time().hour();
        if (hour >= 0 && hour < 24) {
            buckets[hour] += sliceEnd - cursor;
        }
        cursor = sliceEnd;
        moment = QDateTime::fromMSecsSinceEpoch(cursor);
    }
}

} // namespace

Result<HourlyActivityDay> HourlyReadModel::loadTotal(qint64 rangeStartMs,
                                                     qint64 rangeEndMs) const
{
    if (!_database.isOpen()) {
        return fail<HourlyActivityDay>(QStringLiteral("数据库未打开"));
    }
    if (rangeEndMs <= rangeStartMs) {
        return succeed(HourlyActivityDay{});
    }

    HourlyActivityDay day;

    QSqlQuery query(_database.connection());
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
        "SELECT s.start_time_ms, s.end_time_ms "
        "FROM sessions s JOIN apps a ON a.id = s.app_id "
        "WHERE a.track_enabled = 1 AND a.hidden = 0 "
        "  AND s.start_time_ms < :end "
        "  AND COALESCE(s.end_time_ms, :now) > :start "
        "ORDER BY s.start_time_ms ASC "
        "LIMIT %1").arg(kMaxHourlySegments));
    query.bindValue(QStringLiteral(":end"), rangeEndMs);
    query.bindValue(QStringLiteral(":now"), rangeEndMs);
    query.bindValue(QStringLiteral(":start"), rangeStartMs);
    if (!query.exec()) {
        return fail<HourlyActivityDay>(describeError(query));
    }

    // 一次区间扫描（单日规模，几千段以内），逐段交叠分桶。
    while (query.next()) {
        const qint64 startMs = std::max<qint64>(query.value(0).toLongLong(), rangeStartMs);
        const qint64 endMs = std::min<qint64>(
            query.value(1).isNull() ? rangeEndMs : query.value(1).toLongLong(), rangeEndMs);
        addToBuckets(startMs, endMs, day.hourlyMs);
    }
    return succeed(day);
}

Result<QVector<HourlyCategorySlice>> HourlyReadModel::loadByCategory(
    qint64 rangeStartMs, qint64 rangeEndMs) const
{
    if (!_database.isOpen()) {
        return fail<QVector<HourlyCategorySlice>>(QStringLiteral("数据库未打开"));
    }
    if (rangeEndMs <= rangeStartMs) {
        return succeed(QVector<HourlyCategorySlice>{});
    }

    // 按应用聚合的每 exe 毫秒桶，再在内存里按生效分类归组：
    // 分类规则在 C++ 内置表 + 覆盖项里，SQL 算不了。
    struct ExeBucket
    {
        QString exeName;
        QString categoryId;
        qint64 hourlyMs[24] = {};
    };
    QHash<QString, int> exeIndex;
    QVector<ExeBucket> exeBuckets;

    // 分类展示值走注册表快照（名字/颜色可被用户改；悬空 id 落 other）。
    const Result<CategoryRegistrySnapshot> registry = loadCategoryRegistrySnapshot(_database);
    const CategoryRegistrySnapshot categories = registry.ok() ? registry.value
                                                              : CategoryRegistrySnapshot{};

    // 关联应用（成员并到父应用）——成员段按父的分类与身份归桶，
    // 否则同一个应用会同时出现在两个分类里。links 为空时行为与以前一致。
    const Result<AppLinks> loadedLinks = loadAppLinks(_database);
    const AppLinks links = loadedLinks.ok() ? loadedLinks.value : AppLinks{};
    QHash<QString, AppIdentity> identities;
    if (!links.isEmpty()) {
        const Result<QHash<QString, AppIdentity>> loadedIdentities =
            loadAppIdentities(_database);
        if (!loadedIdentities.ok()) {
            return fail<QVector<HourlyCategorySlice>>(loadedIdentities.error);
        }
        identities = loadedIdentities.value;
    }

    QSqlQuery query(_database.connection());
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
        "SELECT a.exe_name, a.category_id, s.start_time_ms, s.end_time_ms "
        "FROM sessions s JOIN apps a ON a.id = s.app_id "
        "WHERE a.track_enabled = 1 AND a.hidden = 0 "
        "  AND s.start_time_ms < :end "
        "  AND COALESCE(s.end_time_ms, :now) > :start "
        "ORDER BY s.start_time_ms ASC "
        "LIMIT %1").arg(kMaxHourlySegments));
    query.bindValue(QStringLiteral(":end"), rangeEndMs);
    query.bindValue(QStringLiteral(":now"), rangeEndMs);
    query.bindValue(QStringLiteral(":start"), rangeStartMs);
    if (!query.exec()) {
        return fail<QVector<HourlyCategorySlice>>(describeError(query));
    }

    while (query.next()) {
        const QString exeName = query.value(0).toString();
        const QString overrideId = query.value(1).toString();
        const qint64 startMs = std::max<qint64>(query.value(2).toLongLong(), rangeStartMs);
        const qint64 endMs = std::min<qint64>(
            query.value(3).isNull() ? rangeEndMs : query.value(3).toLongLong(), rangeEndMs);

        // 归并键：未关联时是自己，有关联时是父。成员的身份与分类都换成父的。
        const QString key = statisticalAppKey(exeName, links);
        int index = exeIndex.value(key, -1);
        if (index < 0) {
            ExeBucket bucket;
            bucket.exeName = exeName;
            bucket.categoryId = resolveCategoryId(exeName, overrideId);
            if (const std::optional<AppIdentity> parent = linkedIdentity(exeName, links, identities)) {
                bucket.exeName = parent->exeName;
                bucket.categoryId = resolveCategoryId(parent->exeName, parent->categoryId);
            }
            // 悬空 id 落 other（注册表可用时）；否则保持原 id，token 回退链兜底。
            if (!categories.entries.isEmpty()) {
                bucket.categoryId = categories.resolveId(bucket.categoryId);
            }
            index = exeBuckets.size();
            exeIndex.insert(key, index);
            exeBuckets.append(bucket);
        }
        addToBuckets(startMs, endMs, exeBuckets[index].hourlyMs);
    }

    // exe 桶 → 分类片。
    QHash<QString, int> sliceIndex;
    QVector<HourlyCategorySlice> slices;
    for (const ExeBucket& bucket : exeBuckets) {
        int index = sliceIndex.value(bucket.categoryId, -1);
        if (index < 0) {
            HourlyCategorySlice slice;
            slice.categoryId = bucket.categoryId;
            if (categories.entries.isEmpty()) {
                const CategoryToken token = categoryToken(bucket.categoryId);
                slice.label = token.label;
                slice.colorHex = token.colorHex;
            } else {
                slice.label = categories.labelFor(bucket.categoryId);
                slice.colorHex = categories.colorFor(bucket.categoryId);
            }
            index = slices.size();
            sliceIndex.insert(bucket.categoryId, index);
            slices.append(slice);
        }
        for (int hour = 0; hour < 24; ++hour) {
            slices[index].hourlyMs[hour] += bucket.hourlyMs[hour];
        }
    }

    // 按总时长降序，图例顺序稳定。
    std::sort(slices.begin(), slices.end(),
              [](const HourlyCategorySlice& left, const HourlyCategorySlice& right) {
                  qint64 leftTotal = 0;
                  qint64 rightTotal = 0;
                  for (int hour = 0; hour < 24; ++hour) {
                      leftTotal += left.hourlyMs[hour];
                      rightTotal += right.hourlyMs[hour];
                  }
                  return leftTotal > rightTotal;
              });
    return succeed(slices);
}

} // namespace patina::data
