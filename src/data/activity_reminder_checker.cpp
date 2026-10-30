#include "data/activity_reminder_checker.h"

#include "data/app_category.h"
#include "data/app_links.h"
#include "data/database.h"
#include "data/tools_repository.h"

#include <QHash>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

#include <optional>

namespace patina::data
{
namespace
{

QString describeError(const QSqlQuery& query)
{
    const QSqlError error = query.lastError();
    return error.isValid() ? error.text() : QStringLiteral("SQL 执行失败");
}

// 单个 exe 今日的累计使用毫秒（已闭合段与 [dayStartMs, nowMs] 的交叠 + 未闭合段）。
// 一天内的应用个位数到几十个，逐 exe 一条聚合查询即可，不必整表进内存。
// exeName 可以是多个（父应用 + 关联成员），用命名占位符展开成 IN 列表。
Result<qint64> exeUsageToday(Database& database, const QStringList& exeNames,
                             qint64 dayStartMs, qint64 nowMs)
{
    if (exeNames.isEmpty()) {
        return succeed<qint64>(0);
    }

    QString clause;
    for (int index = 0; index < exeNames.size(); ++index) {
        if (index > 0) {
            clause += QStringLiteral(", ");
        }
        clause += QStringLiteral(":e%1").arg(index);
    }

    QSqlQuery query(database.connection());
    query.prepare(QStringLiteral(
        "SELECT COALESCE(SUM("
        "    MIN(COALESCE(end_time_ms, :now), :now2) - MAX(start_time_ms, :dayStart)"
        "), 0) "
        "FROM sessions s JOIN apps a ON a.id = s.app_id "
        "WHERE a.exe_name COLLATE NOCASE IN (%1) AND a.hidden = 0 "
        "  AND start_time_ms < :now3 AND COALESCE(end_time_ms, :now4) > :dayStart2").arg(clause));
    for (int index = 0; index < exeNames.size(); ++index) {
        query.bindValue(QStringLiteral(":e%1").arg(index), exeNames.at(index));
    }
    query.bindValue(QStringLiteral(":now"), nowMs);
    query.bindValue(QStringLiteral(":now2"), nowMs);
    query.bindValue(QStringLiteral(":now3"), nowMs);
    query.bindValue(QStringLiteral(":now4"), nowMs);
    query.bindValue(QStringLiteral(":dayStart"), dayStartMs);
    query.bindValue(QStringLiteral(":dayStart2"), dayStartMs);
    if (!query.exec() || !query.next()) {
        return fail<qint64>(describeError(query));
    }
    return succeed(query.value(0).toLongLong());
}

// 分类今日的累计使用毫秒：分类按「生效分类 id」分组，覆盖项与内置规则
// 都要在 SQL 之外解析（分类规则在 data/app_category 的 C++ 表里），
// 所以这里按 exe 拉聚合再在内存里归组——一天的应用数有限，代价可控。
// 关联应用的成员按父应用的分类归组，否则同一条规则会漏算成员。
Result<QHash<QString, qint64>> categoryUsageToday(Database& database,
                                                  const AppLinks& links,
                                                  const QHash<QString, AppIdentity>& identities,
                                                  qint64 dayStartMs, qint64 nowMs)
{
    QSqlQuery query(database.connection());
    query.prepare(QStringLiteral(
        "SELECT a.exe_name, a.category_id, "
        "       COALESCE(SUM("
        "           MIN(COALESCE(s.end_time_ms, ?), ?) - MAX(s.start_time_ms, ?)"
        "       ), 0) "
        "FROM sessions s JOIN apps a ON a.id = s.app_id "
        "WHERE a.hidden = 0 "
        "  AND s.start_time_ms < ? AND COALESCE(s.end_time_ms, ?) > ? "
        "GROUP BY a.exe_name, a.category_id"));
    query.addBindValue(nowMs);
    query.addBindValue(nowMs);
    query.addBindValue(dayStartMs);
    query.addBindValue(nowMs);
    query.addBindValue(nowMs);
    query.addBindValue(dayStartMs);
    if (!query.exec()) {
        return fail<QHash<QString, qint64>>(describeError(query));
    }

    QHash<QString, qint64> usage;
    while (query.next()) {
        const QString exeName = query.value(0).toString();
        const QString overrideId = query.value(1).toString();
        const qint64 durationMs = query.value(2).toLongLong();
        QString categoryId;
        if (const std::optional<AppIdentity> parent = linkedIdentity(exeName, links, identities)) {
            categoryId = resolveCategoryId(parent->exeName, parent->categoryId);
        } else {
            categoryId = resolveCategoryId(exeName, overrideId);
        }
        usage[categoryId] += durationMs;
    }
    return succeed(usage);
}

} // namespace

Result<QVector<ActivityReminderHit>> ActivityReminderChecker::fireDueRules(
    const QString& dateKey, qint64 dayStartMs, qint64 nowMs)
{
    if (!_database.isOpen()) {
        return fail<QVector<ActivityReminderHit>>(QStringLiteral("数据库未打开"));
    }

    ToolsRepository repository(_database);
    const Result<QVector<ToolActivityReminderRule>> rules = repository.listActivityRules(false);
    if (!rules.ok()) {
        return fail<QVector<ActivityReminderHit>>(rules.error);
    }

    QVector<ActivityReminderHit> hits;
    bool categoryUsageLoaded = false;
    QHash<QString, qint64> categoryUsage;

    // 关联应用（成员并到父应用）——app 规则按父 + 成员求和，
    // category 规则把成员算进父的分类。links 为空时行为与以前一致。
    const Result<AppLinks> loadedLinks = loadAppLinks(_database);
    const AppLinks links = loadedLinks.ok() ? loadedLinks.value : AppLinks{};
    QHash<QString, AppIdentity> identities;
    if (!links.isEmpty()) {
        const Result<QHash<QString, AppIdentity>> loadedIdentities =
            loadAppIdentities(_database);
        if (!loadedIdentities.ok()) {
            return fail<QVector<ActivityReminderHit>>(loadedIdentities.error);
        }
        identities = loadedIdentities.value;
    }

    for (const ToolActivityReminderRule& rule : rules.value) {
        // 当日已触发过的直接跳过：每天最多提醒一次（last_fired_date_key 语义）。
        if (rule.lastFiredDateKey == dateKey) {
            continue;
        }

        qint64 usageMs = 0;
        if (rule.targetKind == QLatin1String("app")) {
            const QString exeName = rule.exeName.isEmpty() ? rule.appName : rule.exeName;
            // 规则盯的是「这个应用」——有关联时它的成员也要算进去。
            QStringList exeNames = linkedAppKeys(exeName, links);
            if (exeNames.isEmpty()) {
                exeNames.append(exeName);
            }
            const Result<qint64> usage =
                exeUsageToday(_database, exeNames, dayStartMs, nowMs);
            if (!usage.ok()) {
                return fail<QVector<ActivityReminderHit>>(usage.error);
            }
            usageMs = usage.value;
        } else if (rule.targetKind == QLatin1String("category")) {
            if (!categoryUsageLoaded) {
                const Result<QHash<QString, qint64>> loaded =
                    categoryUsageToday(_database, links, identities, dayStartMs, nowMs);
                if (!loaded.ok()) {
                    return fail<QVector<ActivityReminderHit>>(loaded.error);
                }
                categoryUsage = loaded.value;
                categoryUsageLoaded = true;
            }
            usageMs = categoryUsage.value(rule.categoryId, 0);
        } else {
            continue;
        }

        if (usageMs < rule.limitMs) {
            continue;
        }

        // 先占位再上报：UPDATE 带 last_fired_date_key 守卫，抢到的才算触发。
        const Status marked = repository.markActivityRuleFired(rule.id, dateKey, nowMs);
        if (!marked.ok()) {
            return fail<QVector<ActivityReminderHit>>(marked.error);
        }

        ActivityReminderHit hit;
        hit.ruleId = rule.id;
        hit.label = rule.labelSnapshot;
        hit.message = rule.message;
        hit.limitMs = rule.limitMs;
        hit.usageMs = usageMs;
        hits.append(hit);
    }

    return succeed(hits);
}

} // namespace patina::data
