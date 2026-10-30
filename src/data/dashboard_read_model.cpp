#include "data/dashboard_read_model.h"

#include "data/app_category.h"
#include "data/app_links.h"
#include "data/category_registry.h"
#include "data/database.h"

#include <QChar>
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

// 单日 GROUP BY app 之后行数等于当日应用数；显式上限只是防御。
constexpr int kMaxTodayApps = 2048;

QString describeError(const QSqlQuery& query)
{
    const QSqlError error = query.lastError();
    return error.isValid() ? error.text() : QStringLiteral("SQL 执行失败");
}

// 显示名净化：与 AppRepository::readApp 同一语义（版本资源带的尾部 NUL
// 在 SQL 侧不可见，只能读出时清洗）。本文件聚合查询直接读 display_name 列，
// 需要自己的拷贝；改动净化逻辑时与 app_repository / classification_repository
// 的同名函数同步。
QString sanitizeDisplayName(QString name)
{
    if (name.isEmpty()) {
        return name;
    }
    for (int i = 0; i < name.size(); ++i) {
        const QChar ch = name.at(i);
        if (ch.category() == QChar::Other_Control || ch.unicode() == 0) {
            name.truncate(i);
            break;
        }
    }
    name.remove(QChar(0xFEFF));   // BOM
    name.remove(QChar(0x200B));   // zero-width space
    return name.trimmed();
}

bool appUsageLess(const AppUsage& left, const AppUsage& right)
{
    if (left.durationMs != right.durationMs) {
        return left.durationMs > right.durationMs;
    }
    return left.exeName.compare(right.exeName, Qt::CaseInsensitive) < 0;
}

// 读事务：析构时提交。读事务里不写数据，提交失败也没法补救，忽略返回值。
class ReadTransaction
{
public:
    explicit ReadTransaction(QSqlDatabase connection) : _connection(std::move(connection))
    {
        _active = QSqlQuery(_connection).exec(QStringLiteral("BEGIN"));
    }

    ~ReadTransaction()
    {
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

} // namespace

qint64 localDayStartMs(qint64 timestampMs)
{
    // 走 QDateTime 的本地时区规则，夏令时与跨年不用自己算。
    QDateTime start = QDateTime::fromMSecsSinceEpoch(timestampMs);
    start.setTime(QTime(0, 0));
    return start.toMSecsSinceEpoch();
}

Result<TodayOverview> DashboardReadModel::loadToday(qint64 dayStartMs, qint64 cutoffMs) const
{
    if (!_database.isOpen()) {
        return fail<TodayOverview>(QStringLiteral("数据库未打开"));
    }
    if (dayStartMs <= 0) {
        return fail<TodayOverview>(QStringLiteral("日期起点非法"));
    }

    TodayOverview overview;
    overview.dayStartMs = dayStartMs;
    overview.cutoffMs = std::max(dayStartMs, cutoffMs);
    if (overview.cutoffMs == dayStartMs) {
        // 刚过零点：区间为空，不必查库。
        return succeed(overview);
    }

    QSqlDatabase connection = _database.connection();
    // 两次查询要在同一个快照上：否则追踪线程刚闭合的段会让「总时长」与
    // 「按应用求和」出现两个不同答案。
    ReadTransaction transaction(connection);

    // 交叠裁剪与 HourlyReadModel::loadTotal 同一口径，所以总时长、按应用求和、
    // 小时柱状图三处对得上。未闭合段按 cutoffMs 封底计入（即
    // COALESCE(end_time, ?)）：否则「当前正在用的应用」要等它切走才出现在分布里，
    // 界面看起来就是慢一拍。
    QSqlQuery query(connection);
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
        "SELECT s.app_id, a.exe_name, a.exe_path, a.display_name, a.category_id, a.color_hex, "
        "       SUM(MIN(COALESCE(s.end_time_ms, ?), ?) - MAX(s.start_time_ms, ?)) AS duration_ms, "
        "       COUNT(*) AS segment_count "
        "FROM sessions s "
        "JOIN apps a ON a.id = s.app_id "
        "WHERE a.track_enabled = 1 AND a.hidden = 0 "
        "  AND s.start_time_ms < ? AND COALESCE(s.end_time_ms, ?) > ? "
        "GROUP BY s.app_id "
        "ORDER BY duration_ms DESC, a.exe_name ASC "
        "LIMIT %1").arg(kMaxTodayApps));
    query.addBindValue(overview.cutoffMs);
    query.addBindValue(overview.cutoffMs);
    query.addBindValue(dayStartMs);
    query.addBindValue(overview.cutoffMs);
    query.addBindValue(overview.cutoffMs);
    query.addBindValue(dayStartMs);
    if (!query.exec()) {
        return fail<TodayOverview>(describeError(query));
    }

    // 分类展示值（名字/颜色）走注册表快照——分类可增删改名配色之后，
    // 内置静态表不再是唯一事实源。注册表读失败时退回内置表（分类功能退化为
    // 界面照常出数）。
    const Result<CategoryRegistrySnapshot> registry = loadCategoryRegistrySnapshot(_database);
    const CategoryRegistrySnapshot categories = registry.ok() ? registry.value : CategoryRegistrySnapshot{};

    // 关联应用（成员并到父应用）是读时归并口径，SQL 侧仍按 app_id 分组，
    // 归并在下面逐行做。links 为空时整段逻辑退化成原来的行为。
    const Result<AppLinks> loadedLinks = loadAppLinks(_database);
    const AppLinks links = loadedLinks.ok() ? loadedLinks.value : AppLinks{};
    QHash<QString, AppIdentity> identities;
    if (!links.isEmpty()) {
        const Result<QHash<QString, AppIdentity>> loadedIdentities =
            loadAppIdentities(_database);
        if (!loadedIdentities.ok()) {
            return fail<TodayOverview>(loadedIdentities.error);
        }
        identities = loadedIdentities.value;
    }
    QHash<QString, int> appIndex;

    while (query.next()) {
        AppUsage usage;
        usage.appId = query.value(0).toLongLong();
        usage.exeName = query.value(1).toString();
        usage.exePath = query.value(2).toString();
        usage.displayName = sanitizeDisplayName(query.value(3).toString());
        usage.categoryId = query.value(4).toString();
        usage.colorHex = query.value(5).toString();
        usage.durationMs = std::max<qint64>(0, query.value(6).toLongLong());
        usage.segmentCount = query.value(7).toInt();
        overview.totalDurationMs += usage.durationMs;

        // 归并键：未关联时就是自己的小写 exe 名；有关联时是父应用。
        const QString mergedKey = statisticalAppKey(usage.exeName, links);

        // 成员行换成父应用的身份：图标、展示名、分类、配色都取父的覆盖项。
        // 这一步在归并判断之前做——分类累加在下面，用的必须是归并后的身份，
        // 否则成员会把自己那份时长算进它自己的分类里。
        if (const std::optional<AppIdentity> parent =
                linkedIdentity(usage.exeName, links, identities)) {
            usage.appId = parent->appId;
            usage.exeName = parent->exeName;
            usage.exePath = parent->exePath;
            usage.displayName = sanitizeDisplayName(parent->displayName);
            usage.categoryId = parent->categoryId;
            usage.colorHex = parent->colorHex;
        }

        const auto merged = appIndex.constFind(mergedKey);
        if (merged != appIndex.constEnd()) {
            // 同一父应用下的第二行起：只累加时长与段数，身份沿用第一行（父的）。
            AppUsage& existing = overview.apps[merged.value()];
            existing.durationMs += usage.durationMs;
            existing.segmentCount += usage.segmentCount;
        } else {
            appIndex.insert(mergedKey, overview.apps.size());
            overview.apps.append(usage);
        }

        // 分类分布按「归并后的应用」算：成员并到父之后，分类也应该是父的分类，
        // 否则同一个应用会同时出现在两个分类里。
        QString resolvedId = resolveCategoryId(usage.exeName, usage.categoryId);
        if (!categories.entries.isEmpty()) {
            resolvedId = categories.resolveId(resolvedId);
        }
        const auto existing = std::find_if(
            overview.categories.begin(), overview.categories.end(),
            [&resolvedId](const CategoryUsage& item) { return item.categoryId == resolvedId; });
        if (existing != overview.categories.end()) {
            existing->durationMs += usage.durationMs;
            continue;
        }
        CategoryUsage category;
        category.categoryId = resolvedId;
        if (categories.entries.isEmpty()) {
            const CategoryToken token = categoryToken(resolvedId);
            category.label = token.label;
            category.colorHex = token.colorHex;
        } else {
            category.label = categories.labelFor(resolvedId);
            category.colorHex = categories.colorFor(resolvedId);
        }
        category.durationMs = usage.durationMs;
        overview.categories.append(category);
    }

    // 进行中的段：上面的聚合已按 cutoffMs 封底计入总时长与分布，这里再单独暴露一次，
    // 供界面渲染「进行中」那一行（含窗口标题与起始时刻）。单活跃段索引保证最多一行。
    // 关掉统计的应用要一起挡掉：它的段没进聚合，界面上就不该说「已计入总时长」。
    QSqlQuery liveQuery(connection);
    liveQuery.setForwardOnly(true);
    if (!liveQuery.exec(QStringLiteral(
            "SELECT s.app_id, a.exe_name, a.exe_path, a.display_name, s.window_title, "
            "       s.start_time_ms "
            "FROM sessions s JOIN apps a ON a.id = s.app_id "
            "WHERE s.end_time_ms IS NULL AND a.track_enabled = 1 AND a.hidden = 0 LIMIT 1"))) {
        return fail<TodayOverview>(describeError(liveQuery));
    }
    if (liveQuery.next()) {
        LiveSession live;
        live.appId = liveQuery.value(0).toLongLong();
        live.exeName = liveQuery.value(1).toString();
        live.exePath = liveQuery.value(2).toString();
        live.displayName = sanitizeDisplayName(liveQuery.value(3).toString());
        live.windowTitle = liveQuery.value(4).toString();
        live.startTimeMs = liveQuery.value(5).toLongLong();
        // 进行中的是成员应用时，这一行也显示成父应用——
        // 否则排行里已经并到父名下，顶上却还挂着成员的名字。
        if (const std::optional<AppIdentity> parent =
                linkedIdentity(live.exeName, links, identities)) {
            live.appId = parent->appId;
            live.exeName = parent->exeName;
            live.exePath = parent->exePath;
            live.displayName = sanitizeDisplayName(parent->displayName);
        }
        // 进行中的段从今天零点前就开着时，只从零点算起，与其它数字同口径。
        live.durationMs = std::max<qint64>(
            0, overview.cutoffMs - std::max(live.startTimeMs, dayStartMs));
        overview.live = live;
    }

    std::sort(overview.apps.begin(), overview.apps.end(), appUsageLess);
    std::stable_sort(overview.categories.begin(), overview.categories.end(),
                     [](const CategoryUsage& left, const CategoryUsage& right) {
                         return left.durationMs > right.durationMs;
                     });

    return succeed(overview);
}

} // namespace patina::data
