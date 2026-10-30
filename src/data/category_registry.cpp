#include "data/category_registry.h"

#include "data/database.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>

#include <algorithm>

namespace patina::data
{
namespace
{

// 自定义分类 id：custom:category_<hex>（"custom:" + "category_" + 去横线 uuid）。
// 每次新建都开一局新 id，避免与标签撞名。
const QRegularExpression& customIdPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^custom:category_[0-9a-f]{16,}$"));
    return pattern;
}

bool isCustomCategoryId(const QString& categoryId)
{
    return customIdPattern().match(categoryId).hasMatch();
}

QString describeError(const QSqlQuery& query)
{
    const QSqlError error = query.lastError();
    return error.isValid() ? error.text() : QStringLiteral("SQL 执行失败");
}

// 生效显示名：标签覆盖 → 内置表 → 自定义 id 的兜底「未命名分类」。
QString effectiveLabel(const QString& categoryId,
                       const QHash<QString, QString>& labelOverrides)
{
    const auto overrideIt = labelOverrides.constFind(categoryId);
    if (overrideIt != labelOverrides.constEnd()) {
        return overrideIt.value();
    }
    const CategoryToken token = categoryToken(categoryId);
    // categoryToken 对未知 id 回落 other；自定义分类不该被吞成「其他」，
    // 这里按 id 是否命中内置表区分。
    const QVector<CategoryToken> builtin = categoryTokens();
    const bool known = std::any_of(builtin.cbegin(), builtin.cend(),
                                   [&categoryId](const CategoryToken& item) {
                                       return item.id == categoryId;
                                   });
    if (known) {
        return token.label;
    }
    return QStringLiteral("未命名分类");
}

// 生效颜色：颜色覆盖 → 内置表 → 自定义用 accent 兜底。
QString effectiveColor(const QString& categoryId,
                       const QHash<QString, QString>& colorOverrides)
{
    const auto overrideIt = colorOverrides.constFind(categoryId);
    if (overrideIt != colorOverrides.constEnd()) {
        return overrideIt.value();
    }
    const QVector<CategoryToken> builtin = categoryTokens();
    const bool known = std::any_of(builtin.cbegin(), builtin.cend(),
                                   [&categoryId](const CategoryToken& item) {
                                       return item.id == categoryId;
                                   });
    if (known) {
        return categoryToken(categoryId).colorHex;
    }
    return QStringLiteral("#4F5FD7");
}

} // namespace

QString categoryLabelOverrideKey(const QString& categoryId)
{
    return QStringLiteral("__category_label_override::") + categoryId;
}

QString categoryColorOverrideKey(const QString& categoryId)
{
    return QStringLiteral("__category_color_override::") + categoryId;
}

QString customCategoryKey(const QString& categoryId)
{
    return QStringLiteral("__custom_category::") + categoryId;
}

QString deletedCategoryKey(const QString& categoryId)
{
    return QStringLiteral("__deleted_category::") + categoryId;
}

QString categoryOrderKey()
{
    return QStringLiteral("__category_order::custom");
}

namespace
{

// settings 表按前缀捞键。SQL 只出现在 data/。
Result<QVector<SettingRow>> loadPrefixedRows(Database& database, const QString& prefix)
{
    QSqlQuery query(database.connection());
    query.prepare(QStringLiteral(
        "SELECT key, value FROM settings WHERE key LIKE ? ORDER BY key ASC"));
    query.bindValue(0, prefix + QStringLiteral("%"));
    if (!query.exec()) {
        return fail<QVector<SettingRow>>(describeError(query));
    }
    QVector<SettingRow> rows;
    while (query.next()) {
        SettingRow row;
        row.key = query.value(0).toString();
        row.value = query.value(1).toString();
        rows.append(row);
    }
    return succeed(rows);
}

} // namespace

Result<QVector<CategoryEntry>> loadCategoryRegistry(Database& database)
{
    if (!database.isOpen()) {
        return fail<QVector<CategoryEntry>>(QStringLiteral("数据库未打开"));
    }

    // 四个前缀各捞一遍：键数量是分类规模的个位数到两位数，四次 LIKE 扫描
    // 便宜且每条路径语义独立，比一次大查询后在内存里再分类更好读。
    const auto labels = loadPrefixedRows(database, QStringLiteral("__category_label_override::"));
    const auto colors = loadPrefixedRows(database, QStringLiteral("__category_color_override::"));
    const auto customs = loadPrefixedRows(database, QStringLiteral("__custom_category::"));
    const auto deleted = loadPrefixedRows(database, QStringLiteral("__deleted_category::"));
    const auto orderRows = loadPrefixedRows(database, QStringLiteral("__category_order::"));
    if (!labels.ok()) {
        return fail<QVector<CategoryEntry>>(labels.error);
    }
    if (!colors.ok()) {
        return fail<QVector<CategoryEntry>>(colors.error);
    }
    if (!customs.ok()) {
        return fail<QVector<CategoryEntry>>(customs.error);
    }
    if (!deleted.ok()) {
        return fail<QVector<CategoryEntry>>(deleted.error);
    }
    if (!orderRows.ok()) {
        return fail<QVector<CategoryEntry>>(orderRows.error);
    }

    QHash<QString, QString> labelOverrides;
    for (const SettingRow& row : labels.value) {
        const QString id = row.key.mid(QStringLiteral("__category_label_override::").size());
        const QString label = row.value.trimmed();
        if (!id.isEmpty() && !label.isEmpty()) {
            labelOverrides.insert(id, label);
        }
    }

    QHash<QString, QString> colorOverrides;
    for (const SettingRow& row : colors.value) {
        const QString id = row.key.mid(QStringLiteral("__category_color_override::").size());
        const QString color = row.value.trimmed().toUpper();
        if (!id.isEmpty() && isValidColorHex(color)) {
            colorOverrides.insert(id, color);
        }
    }

    QStringList customIds;
    for (const SettingRow& row : customs.value) {
        const QString id = row.key.mid(QStringLiteral("__custom_category::").size());
        // 键存在即登记（值只是标记）；id 形态非法的旧脏数据直接跳过。
        if (isCustomCategoryId(id)) {
            customIds.append(id);
        }
    }

    QSet<QString> deletedIds;
    for (const SettingRow& row : deleted.value) {
        const QString id = row.key.mid(QStringLiteral("__deleted_category::").size());
        if (!id.isEmpty()) {
            deletedIds.insert(id);
        }
    }

    // 顺序：存的是完整逗号分隔列表，直接解析；不在列表里的自定义分类按 key 升序补尾。
    QStringList orderedCustom;
    for (const SettingRow& row : orderRows.value) {
        if (row.key != categoryOrderKey()) {
            continue;
        }
        for (const QString& id : row.value.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString trimmed = id.trimmed();
            if (isCustomCategoryId(trimmed) && !orderedCustom.contains(trimmed)
                && std::any_of(customs.value.cbegin(), customs.value.cend(),
                               [&trimmed](const SettingRow& item) {
                                   return item.key == customCategoryKey(trimmed);
                               })) {
                orderedCustom.append(trimmed);
            }
        }
    }
    // 登记了但顺序表里没有的（新写入或顺序键损坏）：按 key 升序补在后面。
    QStringList tail = customIds;
    for (const QString& id : orderedCustom) {
        tail.removeAll(id);
    }
    std::sort(tail.begin(), tail.end());
    orderedCustom.append(tail);

    // ---- 组装：内置（去墓碑）→ 自定义 → other ----
    QVector<CategoryEntry> entries;
    const QVector<CategoryToken> builtin = categoryTokens();
    for (const CategoryToken& token : builtin) {
        if (token.id == QStringLiteral("other")) {
            continue; // other 固定排最后
        }
        if (deletedIds.contains(token.id)) {
            continue; // 内置「删除」= 墓碑，退出界面
        }
        CategoryEntry entry;
        entry.id = token.id;
        entry.label = effectiveLabel(token.id, labelOverrides);
        entry.colorHex = effectiveColor(token.id, colorOverrides);
        entry.builtin = true;
        entry.deletableBuiltin = true; // 内置可打墓碑
        entries.append(entry);
    }
    for (const QString& id : orderedCustom) {
        CategoryEntry entry;
        entry.id = id;
        entry.label = effectiveLabel(id, labelOverrides);
        entry.colorHex = effectiveColor(id, colorOverrides);
        entry.builtin = false;
        entry.deletable = true;
        entries.append(entry);
    }

    CategoryEntry other;
    other.id = QStringLiteral("other");
    other.label = effectiveLabel(QStringLiteral("other"), labelOverrides);
    other.colorHex = effectiveColor(QStringLiteral("other"), colorOverrides);
    other.builtin = true; // other：兜底，永不可删
    entries.append(other);

    return succeed(entries);
}

Result<QString> loadCategoryLabel(Database& database, const QString& categoryId)
{
    if (!database.isOpen()) {
        return fail<QString>(QStringLiteral("数据库未打开"));
    }
    QSqlQuery query(database.connection());
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    query.bindValue(0, categoryLabelOverrideKey(categoryId));
    if (!query.exec()) {
        return fail<QString>(describeError(query));
    }
    if (query.next()) {
        const QString label = query.value(0).toString().trimmed();
        if (!label.isEmpty()) {
            return succeed(label);
        }
    }
    const QVector<CategoryToken> builtin = categoryTokens();
    const bool known = std::any_of(builtin.cbegin(), builtin.cend(),
                                   [&categoryId](const CategoryToken& item) {
                                       return item.id == categoryId;
                                   });
    return succeed(known ? categoryToken(categoryId).label
                         : QStringLiteral("未命名分类"));
}

Result<QString> loadCategoryColor(Database& database, const QString& categoryId)
{
    if (!database.isOpen()) {
        return fail<QString>(QStringLiteral("数据库未打开"));
    }
    QSqlQuery query(database.connection());
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    query.bindValue(0, categoryColorOverrideKey(categoryId));
    if (!query.exec()) {
        return fail<QString>(describeError(query));
    }
    if (query.next()) {
        const QString color = query.value(0).toString().trimmed().toUpper();
        if (isValidColorHex(color)) {
            return succeed(color);
        }
    }
    const QVector<CategoryToken> builtin = categoryTokens();
    const bool known = std::any_of(builtin.cbegin(), builtin.cend(),
                                   [&categoryId](const CategoryToken& item) {
                                       return item.id == categoryId;
                                   });
    return succeed(known ? categoryToken(categoryId).colorHex
                         : QStringLiteral("#4F5FD7"));
}

// ---------------- 快照式解析（读模型用） ----------------

QString CategoryRegistrySnapshot::labelFor(const QString& categoryId) const
{
    for (const CategoryEntry& entry : entries) {
        if (entry.id == categoryId) {
            return entry.label;
        }
    }
    // 悬空 id：落 other 的显示值（entries 末尾必有 other）。
    const CategoryEntry* other = nullptr;
    for (const CategoryEntry& entry : entries) {
        if (entry.id == QStringLiteral("other")) {
            other = &entry;
            break;
        }
    }
    return other != nullptr ? other->label : QStringLiteral("其他");
}

QString CategoryRegistrySnapshot::colorFor(const QString& categoryId) const
{
    for (const CategoryEntry& entry : entries) {
        if (entry.id == categoryId) {
            return entry.colorHex;
        }
    }
    const CategoryEntry* other = nullptr;
    for (const CategoryEntry& entry : entries) {
        if (entry.id == QStringLiteral("other")) {
            other = &entry;
            break;
        }
    }
    return other != nullptr ? other->colorHex : QStringLiteral("#8F98A8");
}

QString CategoryRegistrySnapshot::resolveId(const QString& categoryId) const
{
    const QString trimmed = categoryId.trimmed();
    if (trimmed.isEmpty()) {
        return trimmed; // 空 = 让上游走内置规则，不在这里猜
    }
    for (const CategoryEntry& entry : entries) {
        if (entry.id == trimmed) {
            return trimmed;
        }
    }
    // 悬空 id（自定义被真删 / 内置被墓碑）统一落 other。
    return QStringLiteral("other");
}

Result<CategoryRegistrySnapshot> loadCategoryRegistrySnapshot(Database& database)
{
    CategoryRegistrySnapshot snapshot;
    const Result<QVector<CategoryEntry>> entries = loadCategoryRegistry(database);
    if (!entries.ok()) {
        return fail<CategoryRegistrySnapshot>(entries.error);
    }
    snapshot.entries = entries.value;
    return succeed(std::move(snapshot));
}

} // namespace patina::data
