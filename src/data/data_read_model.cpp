#include "data/data_read_model.h"

#include "data/app_links.h"
#include "data/database.h"

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTime>
#include <QVariant>

#include <algorithm>
#include <optional>

namespace patina::data
{
namespace
{

constexpr qint64 kHourMs = 60 * 60 * 1000;

// 读查询必须有界。这个上限约等于「三年、每天四百多段」，正常使用碰不到；
// 真碰到说明数据异常，宁可少算也不能让一条查询把内存撑爆。
// 真正的内存保障靠 setForwardOnly(true)：驱动逐行送，结果集不在客户端缓存。
constexpr int kMaxTrendSegments = 500000;
// apps 表是用户装过的软件数量（几百），这里只是防御性上限。
constexpr int kMaxTrendApps = 4096;

QString describeError(const QSqlQuery& query)
{
    const QSqlError error = query.lastError();
    return error.isValid() ? error.text() : QStringLiteral("SQL 执行失败");
}

class ReadTransaction
{
public:
    explicit ReadTransaction(QSqlDatabase connection) : _connection(std::move(connection))
    {
        _active = QSqlQuery(_connection).exec(QStringLiteral("BEGIN"));
    }

    ~ReadTransaction()
    {
        // 读事务里不写数据，提交失败也没法补救，忽略返回值。
        if (_active) {
            QSqlQuery(_connection).exec(QStringLiteral("COMMIT"));
        }
    }

    ReadTransaction(const ReadTransaction&) = delete;
    ReadTransaction& operator=(const ReadTransaction&) = delete;

    bool isActive() const { return _active; }

private:
    QSqlDatabase _connection;
    bool _active = false;
};

// 把一段 [startMs, endMs) 按本地日边界切开，同时累加到日桶、应用日桶和小时桶。
//
// 这是整层唯一的切桶实现：总时长、逐日时长、逐应用时长、小时分布全部出自它，
// 所以「日总时长 == 各应用日时长之和」是构造上成立的，不靠事后对齐。
void accumulateSegment(qint64 startMs,
                       qint64 endMs,
                       const QVector<qint64>& dayStarts,
                       QVector<qint64>* dayMs,
                       QVector<qint64>* appDayMs,
                       QVector<HourlyMinutes>* hourlyByDay)
{
    const int dayCount = static_cast<int>(dayMs->size());
    qint64 cursor = startMs;

    while (cursor < endMs) {
        // upper_bound 拿到第一个晚于 cursor 的日边界，它前面那个就是 cursor 所属的天。
        const auto it = std::upper_bound(dayStarts.cbegin(), dayStarts.cend(), cursor);
        int dayIndex = static_cast<int>(it - dayStarts.cbegin()) - 1;
        if (dayIndex < 0) {
            dayIndex = 0;
        }
        if (dayIndex >= dayCount) {
            break;
        }

        const qint64 dayStartMs = dayStarts[dayIndex];
        const qint64 sliceEndMs = std::min(dayStarts[dayIndex + 1], endMs);
        if (sliceEndMs <= cursor) {
            break;
        }

        (*dayMs)[dayIndex] += sliceEndMs - cursor;
        (*appDayMs)[dayIndex] += sliceEndMs - cursor;

        // 小时桶：日内第几个小时。跨零点的段已经在上面被日边界切开了。
        HourlyMinutes& dayHourly = (*hourlyByDay)[dayIndex];
        qint64 hourCursor = cursor;
        while (hourCursor < sliceEndMs) {
            const qint64 hourOffset = (hourCursor - dayStartMs) / kHourMs;
            const qint64 hourEndMs = dayStartMs + (hourOffset + 1) * kHourMs;
            const qint64 hourSliceEndMs = std::min(sliceEndMs, hourEndMs);
            const int hour = static_cast<int>(std::clamp<qint64>(hourOffset, 0, 23));
            dayHourly[static_cast<size_t>(hour)] += hourSliceEndMs - hourCursor;
            hourCursor = hourSliceEndMs;
        }

        cursor = sliceEndMs;
    }
}

} // namespace

Result<TrendData> DataReadModel::loadTrend(qint64 rangeStartMs, qint64 rangeEndMs) const
{
    if (!_database.isOpen()) {
        return fail<TrendData>(QStringLiteral("数据库未打开"));
    }
    if (rangeStartMs <= 0) {
        return fail<TrendData>(QStringLiteral("区间起点非法"));
    }

    TrendData data;

    // 起点对齐到本地日 00:00：调用方给的可能是任意时刻，但对齐后逐日桶才对得上。
    const QDate firstDate = QDateTime::fromMSecsSinceEpoch(rangeStartMs).date();
    data.rangeStartMs = QDateTime(firstDate, QTime(0, 0)).toMSecsSinceEpoch();
    if (rangeEndMs <= rangeStartMs) {
        // 终点不晚于起点：夹成空区间，别把起点所在那天整天都算进来。
        data.rangeEndMs = data.rangeStartMs;
        return succeed(data);
    }
    data.rangeEndMs = rangeEndMs;

    // 结束时刻是开区间：正好落在零点时不算新的一天，否则会多出一个空白的日桶。
    const QDate lastDate = QDateTime::fromMSecsSinceEpoch(data.rangeEndMs - 1).date();

    QVector<qint64> dayStarts;
    dayStarts.reserve(static_cast<int>(firstDate.daysTo(lastDate)) + 2);
    for (QDate date = firstDate; date <= lastDate; date = date.addDays(1)) {
        dayStarts.append(QDateTime(date, QTime(0, 0)).toMSecsSinceEpoch());
    }
    dayStarts.append(QDateTime(lastDate.addDays(1), QTime(0, 0)).toMSecsSinceEpoch());

    const int dayCount = static_cast<int>(dayStarts.size()) - 1;
    if (dayCount <= 0) {
        return succeed(data);
    }

    QVector<qint64> dayMs(dayCount, 0);
    QVector<HourlyMinutes> hourlyByDay(dayCount);
    QHash<QString, QVector<qint64>> appDayMs;
    // 归并键（小写 exe）→ 首个原始写法。未关联的应用曲线图例仍显示 apps 表里的大小写。
    QHash<QString, QString> mergedExeNames;

    ReadTransaction transaction(_database.connection());
    if (!transaction.isActive()) {
        return fail<TrendData>(QStringLiteral("无法开启读事务"));
    }

    // 应用信息单独查一次：段表可能有上万行，逐行 JOIN 或取 exe_name 都是纯浪费。
    // 关掉统计的应用在这里就滤掉，段表里的历史行照旧留着，只是不进任何汇总。
    struct AppMeta
    {
        QString exeName;
        QString displayName;
        QString categoryId;
        QString colorHex;
        bool trackEnabled = true;
        bool hidden = false;
    };
    QHash<qint64, AppMeta> appMetas;
    {
        QSqlQuery query(_database.connection());
        query.setForwardOnly(true);
        if (!query.exec(QStringLiteral(
                "SELECT id, exe_name, display_name, category_id, color_hex, track_enabled, hidden "
                "FROM apps LIMIT %1").arg(kMaxTrendApps))) {
            return fail<TrendData>(describeError(query));
        }
        while (query.next()) {
            AppMeta meta;
            meta.exeName = query.value(1).toString();
            meta.displayName = query.value(2).toString();
            meta.categoryId = query.value(3).toString();
            meta.colorHex = query.value(4).toString();
            meta.trackEnabled = query.value(5).toInt() != 0;
            meta.hidden = query.value(6).toInt() != 0;
            appMetas.insert(query.value(0).toLongLong(), meta);
        }
    }

    // 关联应用（成员并到父应用）是读时归并口径。趋势曲线按归并键聚合，
    // 未关联的应用归并键就是自己的小写 exe 名，行为与以前一致。
    const Result<AppLinks> loadedLinks = loadAppLinks(_database);
    const AppLinks links = loadedLinks.ok() ? loadedLinks.value : AppLinks{};
    QHash<QString, AppIdentity> identities;
    if (!links.isEmpty()) {
        const Result<QHash<QString, AppIdentity>> loadedIdentities =
            loadAppIdentities(_database);
        if (!loadedIdentities.ok()) {
            return fail<TrendData>(loadedIdentities.error);
        }
        identities = loadedIdentities.value;
    }

    {
        QSqlQuery query(_database.connection());
        // 逐行送，不在客户端攒结果集：区间再长，内存里也只有当前这一行。
        query.setForwardOnly(true);
        // 与 DashboardReadModel 同一口径：只统计已闭合段，只算与区间的交叠部分。
        query.prepare(QStringLiteral(
            "SELECT app_id, start_time_ms, end_time_ms FROM sessions "
            "WHERE end_time_ms IS NOT NULL "
            "  AND start_time_ms < ? AND end_time_ms > ? "
            "ORDER BY start_time_ms DESC "
            "LIMIT %1").arg(kMaxTrendSegments));
        query.addBindValue(data.rangeEndMs);
        query.addBindValue(data.rangeStartMs);
        if (!query.exec()) {
            return fail<TrendData>(describeError(query));
        }

        while (query.next()) {
            const qint64 appId = query.value(0).toLongLong();
            const auto metaIt = appMetas.constFind(appId);
            if (metaIt == appMetas.constEnd() || !metaIt->trackEnabled || metaIt->hidden) {
                continue;
            }
            const qint64 startMs = std::max(query.value(1).toLongLong(), data.rangeStartMs);
            const qint64 endMs = std::min(query.value(2).toLongLong(), data.rangeEndMs);
            if (endMs <= startMs) {
                continue;
            }

            const QString mergedKey = statisticalAppKey(metaIt->exeName, links);
            // 未关联时归并键是小写 exe 名，曲线图例要显示 apps 表里的原始写法，
            // 所以顺手记一份「归并键 → 首个原始 exe 名」。
            if (!mergedExeNames.contains(mergedKey)) {
                mergedExeNames.insert(mergedKey, metaIt->exeName);
            }
            QVector<qint64>& series = appDayMs[mergedKey];
            if (series.isEmpty()) {
                series.fill(0, dayCount);
            }
            accumulateSegment(startMs, endMs, dayStarts, &dayMs, &series, &hourlyByDay);
        }
    }

    data.days.resize(dayCount);
    for (int index = 0; index < dayCount; ++index) {
        data.days[index].dayStartMs = dayStarts[index];
        data.days[index].durationMs = dayMs[index];
        data.days[index].hourlyMs = hourlyByDay[index];
        data.totalDurationMs += dayMs[index];
        if (dayMs[index] > 0) {
            ++data.activeDayCount;
        }
        data.peakDayMs = std::max(data.peakDayMs, dayMs[index]);
    }

    data.apps.reserve(appDayMs.size());
    for (auto it = appDayMs.cbegin(); it != appDayMs.cend(); ++it) {
        AppTrendSeries series;
        // 归并键是父应用的小写 exe 名（未关联时就是自己）。成员行的身份统一换成
        // 父的，展示名/分类/配色也取父的覆盖项；父不在 apps 表里时退回归并键本身。
        const std::optional<AppIdentity> parent = linkedIdentity(it.key(), links, identities);
        if (parent.has_value()) {
            series.exeName = parent->exeName;
            series.displayName = parent->displayName;
            series.categoryId = parent->categoryId;
            series.colorHex = parent->colorHex;
        } else {
            // 未关联：用 apps 表里的原始写法，别把曲线图例全变成小写。
            series.exeName = mergedExeNames.value(it.key(), it.key());
        }
        if (series.exeName.isEmpty()) {
            series.exeName = QStringLiteral("未知应用");
        }
        series.dayDurationsMs = it.value();
        for (const qint64 durationMs : it.value()) {
            series.totalDurationMs += durationMs;
        }
        data.apps.append(series);
    }
    std::sort(data.apps.begin(),
              data.apps.end(),
              [](const AppTrendSeries& left, const AppTrendSeries& right) {
                  if (left.totalDurationMs != right.totalDurationMs) {
                      return left.totalDurationMs > right.totalDurationMs;
                  }
                  return left.exeName.compare(right.exeName, Qt::CaseInsensitive) < 0;
              });

    return succeed(data);
}

} // namespace patina::data
