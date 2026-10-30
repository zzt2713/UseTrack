#include "data/classification_repository.h"

#include "data/app_category.h"
#include "data/database.h"

#include <QChar>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

#include <algorithm>

namespace patina::data
{
namespace
{

QString describeError(const QSqlQuery& query)
{
    const QSqlError error = query.lastError();
    return error.isValid() ? error.text() : QStringLiteral("SQL 执行失败");
}

// 显示名净化：与 AppRepository::readApp 同一语义（版本资源带的尾部 NUL
// 在 SQL 侧不可见，只能读出时清洗）。两个实现各自持有本函数的拷贝，
// 都在 data/ 匿名名字空间里，改动时两处同步。
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

// 分类可用性由注册表（loadCategoryRegistry）判定：
// 内置分类要过墓碑、自定义分类要查登记键，静态表检查不够用了。

// 空 QString 有两种：null（默认构造出来的）和空串。
// Qt 把 null QString 绑成 SQL NULL，而这几列是 NOT NULL DEFAULT ''，
// 一绑 NULL 就报 "NOT NULL constraint failed"。写库前统一换成空串。
QString nonNullText(const QString& text)
{
    return text.isNull() ? QString::fromLatin1("") : text;
}

// 归一：空串保持空（表示恢复默认），否则要求合法并转大写。
// 返回 false 表示格式非法，调用方据此拒绝整批。
bool normalizeColorHex(const QString& input, QString* output)
{
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty()) {
        *output = QString::fromLatin1("");
        return true;
    }
    if (!isValidColorHex(trimmed)) {
        return false;
    }
    *output = trimmed.toUpper();
    return true;
}

} // namespace

Result<QVector<AppClassificationEntry>> ClassificationRepository::listApps() const
{
    if (!_database.isOpen()) {
        return fail<QVector<AppClassificationEntry>>(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    if (!query.exec(QStringLiteral(
            "SELECT id, exe_name, exe_path, display_name, category_id, color_hex, track_enabled, "
            "       hidden, first_seen_ms, last_seen_ms "
            "FROM apps "
            "ORDER BY last_seen_ms DESC, exe_name ASC"))) {
        return fail<QVector<AppClassificationEntry>>(describeError(query));
    }

    QVector<AppClassificationEntry> entries;
    while (query.next()) {
        AppClassificationEntry entry;
        entry.appId = query.value(0).toLongLong();
        entry.exeName = query.value(1).toString();
        entry.exePath = query.value(2).toString();
        // 与 AppRepository::readApp 同一净化语义：版本资源带的尾部 NUL
        // 在 SQL 侧不可见，只能读出时清洗。
        entry.displayName = sanitizeDisplayName(query.value(3).toString());
        entry.categoryId = query.value(4).toString();
        entry.colorHex = query.value(5).toString();
        entry.trackEnabled = query.value(6).toInt() != 0;
        entry.hidden = query.value(7).toInt() != 0;
        entry.firstSeenMs = query.value(8).toLongLong();
        entry.lastSeenMs = query.value(9).toLongLong();
        entries.append(entry);
    }
    return succeed(entries);
}

Status ClassificationRepository::applyPatches(const QVector<AppClassificationPatch>& patches)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }
    if (patches.isEmpty()) {
        return Status::success();
    }

    // 先全部校验完再开事务：校验失败不该留下任何痕迹。
    // 目标分类必须当前「可见」（内置未删或已登记自定义），否则拒绝整批。
    // 一致性快照：注册表 + 现有应用列表在同一次读取里对齐。
    const Result<QVector<CategoryEntry>> registry = loadCategoryRegistry(_database);
    if (!registry.ok()) {
        return Status::failure(registry.error);
    }
    struct Row
    {
        qint64 appId = 0;
        QString displayName;
        QString categoryId;
        QString colorHex;
        bool trackEnabled = true;
        bool hidden = false;
    };
    QVector<Row> rows;
    rows.reserve(patches.size());
    for (const AppClassificationPatch& patch : patches) {
        if (patch.appId <= 0) {
            return Status::failure(QStringLiteral("app id 非法"));
        }
        Row row;
        row.appId = patch.appId;
        row.displayName = nonNullText(patch.displayName.trimmed());
        row.categoryId = nonNullText(patch.categoryId.trimmed());
        if (!row.categoryId.isEmpty()) {
            const bool available = std::any_of(
                registry.value.cbegin(), registry.value.cend(),
                [&row](const CategoryEntry& entry) { return entry.id == row.categoryId; });
            if (!available) {
                return Status::failure(
                    QStringLiteral("分类不可用或不存在：%1").arg(row.categoryId));
            }
        }
        if (!normalizeColorHex(patch.colorHex, &row.colorHex)) {
            return Status::failure(
                QStringLiteral("颜色格式非法：%1").arg(patch.colorHex));
        }
        row.trackEnabled = patch.trackEnabled;
        row.hidden = patch.hidden;
        rows.append(row);
    }

    Transaction transaction(_database);
    if (!transaction.isActive()) {
        return Status::failure(QStringLiteral("无法开启写事务"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "UPDATE apps SET display_name = ?, category_id = ?, color_hex = ?, track_enabled = ?, "
        "hidden = ? WHERE id = ?"));
    for (const Row& row : rows) {
        query.bindValue(0, row.displayName);
        query.bindValue(1, row.categoryId);
        query.bindValue(2, row.colorHex);
        query.bindValue(3, row.trackEnabled ? 1 : 0);
        query.bindValue(4, row.hidden ? 1 : 0);
        query.bindValue(5, row.appId);
        if (!query.exec()) {
            const QString error = describeError(query);
            transaction.rollback();
            return Status::failure(error);
        }
        // 行数对不上说明 app 已经被删了（追踪线程不会删，但库可能被外部改过）。
        if (query.numRowsAffected() == 0) {
            transaction.rollback();
            return Status::failure(
                QStringLiteral("应用 %1 不存在").arg(row.appId));
        }
    }

    return transaction.commit();
}

// ---------------- 分类本身的增删改名 / 配色 ----------------

namespace
{

// settings 键 upsert / remove（事务内复用）。
// 防御：null QString 会被 Qt 绑成 SQL NULL，settings.value 是 NOT NULL——
// 调用方语义上的「空串」必须是空串（QString("")）而不是 QString()。
Status upsertSetting(Database& database, const QString& key, const QString& value)
{
    QSqlQuery query(database.connection());
    query.prepare(QStringLiteral(
        "INSERT INTO settings (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    query.bindValue(0, key.isNull() ? QString(QStringLiteral("")) : key);
    query.bindValue(1, value.isNull() ? QString(QStringLiteral("")) : value);
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

Status removeSetting(Database& database, const QString& key)
{
    QSqlQuery query(database.connection());
    query.prepare(QStringLiteral("DELETE FROM settings WHERE key = ?"));
    query.bindValue(0, key);
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

QString settingValue(Database& database, const QString& key, bool* ok)
{
    QSqlQuery query(database.connection());
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    query.bindValue(0, key);
    if (!query.exec()) {
        if (ok != nullptr) {
            *ok = false;
        }
        return QString();
    }
    if (ok != nullptr) {
        *ok = true;
    }
    if (!query.next()) {
        return QString();
    }
    return query.value(0).toString();
}

// 当前自定义分类 id 列表（登记键为准），按 key 升序。
Result<QStringList> listCustomCategoryIds(Database& database)
{
    QSqlQuery query(database.connection());
    query.prepare(QStringLiteral(
        "SELECT key FROM settings WHERE key LIKE '__custom_category::%' ORDER BY key ASC"));
    if (!query.exec()) {
        return fail<QStringList>(describeError(query));
    }
    QStringList ids;
    const QString prefix = QStringLiteral("__custom_category::");
    while (query.next()) {
        const QString id = query.value(0).toString().mid(prefix.size());
        if (!id.isEmpty()) {
            ids.append(id);
        }
    }
    return succeed(ids);
}

// 名字归一：去首尾空白、内部空白折叠成单空格。
QString normalizeCategoryLabel(QString label)
{
    label = label.trimmed().simplified();
    return label;
}

} // namespace

Result<QString> ClassificationRepository::createCategory(const QString& labelInput)
{
    if (!_database.isOpen()) {
        return fail<QString>(QStringLiteral("数据库未打开"));
    }

    const QString label = normalizeCategoryLabel(labelInput);
    if (label.isEmpty()) {
        return fail<QString>(QStringLiteral("分类名称不能为空"));
    }
    if (label.size() > 20) {
        return fail<QString>(QStringLiteral("分类名称过长（最多 20 字）"));
    }

    // 与现有分类重名直接拒绝：静默不建会让人以为点错了，给出明确错误更好排查。
    const Result<QVector<CategoryEntry>> registry = loadCategoryRegistry(_database);
    if (!registry.ok()) {
        return fail<QString>(registry.error);
    }
    for (const CategoryEntry& entry : registry.value) {
        if (entry.label.compare(label, Qt::CaseInsensitive) == 0) {
            return fail<QString>(QStringLiteral("已存在同名分类：%1").arg(label));
        }
    }

    // custom:category_<32hex>：即 "custom:" + "category_" + 去横线 uuid。
    QString categoryId;
    for (int attempt = 0; attempt < 8; ++attempt) {
        categoryId = QStringLiteral("custom:category_%1")
                         .arg(QUuid::createUuid().toString(QUuid::Id128));
        const auto registered = listCustomCategoryIds(_database);
        if (!registered.ok()) {
            return fail<QString>(registered.error);
        }
        if (!registered.value.contains(categoryId)) {
            break;
        }
        categoryId.clear();
    }
    if (categoryId.isEmpty()) {
        return fail<QString>(QStringLiteral("无法生成唯一分类 id，请重试"));
    }

    // 顺序键：登记前先读现值，登记后追加。
    bool orderOk = false;
    QString orderValue = settingValue(_database, categoryOrderKey(), &orderOk);
    if (!orderOk) {
        return fail<QString>(QStringLiteral("读取分类顺序失败"));
    }

    Transaction transaction(_database);
    if (!transaction.isActive()) {
        return fail<QString>(QStringLiteral("无法开启写事务"));
    }

    Status status = upsertSetting(_database, customCategoryKey(categoryId),
                                  QStringLiteral("1"));
    if (!status.ok()) {
        transaction.rollback();
        return fail<QString>(status.error);
    }
    status = upsertSetting(_database, categoryLabelOverrideKey(categoryId), label);
    if (!status.ok()) {
        transaction.rollback();
        return fail<QString>(status.error);
    }
    if (!orderValue.isEmpty()) {
        orderValue += QLatin1Char(',');
    }
    orderValue += categoryId;
    status = upsertSetting(_database, categoryOrderKey(), orderValue);
    if (!status.ok()) {
        transaction.rollback();
        return fail<QString>(status.error);
    }

    const Status committed = transaction.commit();
    if (!committed.ok()) {
        return fail<QString>(committed.error);
    }
    return succeed(categoryId);
}

Status ClassificationRepository::deleteCategory(const QString& categoryId)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    const QString id = categoryId.trimmed();
    if (id.isEmpty()) {
        return Status::failure(QStringLiteral("分类 id 为空"));
    }
    if (id == QStringLiteral("other")) {
        // 兜底分类：一切回退的终点，永远不可删（任务验证项）。
        return Status::failure(QStringLiteral("「其他」是兜底分类，不可删除"));
    }

    const QVector<CategoryToken> builtin = categoryTokens();
    const bool isBuiltin = std::any_of(builtin.cbegin(), builtin.cend(),
                                       [&id](const CategoryToken& token) {
                                           return token.id == id;
                                       });
    const bool isCustom =
        QRegularExpression(QStringLiteral("^custom:category_[0-9a-f]{16,}$")).match(id).hasMatch();
    if (!isBuiltin && !isCustom) {
        return Status::failure(QStringLiteral("分类不存在：%1").arg(id));
    }

    Transaction transaction(_database);
    if (!transaction.isActive()) {
        return Status::failure(QStringLiteral("无法开启写事务"));
    }

    // 成员应用回退：清空 category_id 覆盖（同事务）。
    // 内置分类的历史 sessions.app_id → apps.category_id 为空后按内置规则回分类；
    // 自定义分类同理落 other。统计时长不受影响（不动 sessions）。
    {
        QSqlQuery query(_database.connection());
        query.prepare(QStringLiteral(
            "UPDATE apps SET category_id = '' WHERE category_id = ?"));
        query.bindValue(0, id);
        if (!query.exec()) {
            transaction.rollback();
            return Status::failure(describeError(query));
        }
    }

    if (isCustom) {
        // 真删：登记/标签/颜色键全清 + 顺序键里摘除。
        Status status = removeSetting(_database, customCategoryKey(id));
        if (!status.ok()) {
            transaction.rollback();
            return status;
        }
        status = removeSetting(_database, categoryLabelOverrideKey(id));
        if (!status.ok()) {
            transaction.rollback();
            return status;
        }
        status = removeSetting(_database, categoryColorOverrideKey(id));
        if (!status.ok()) {
            transaction.rollback();
            return status;
        }
        // 删除登记键之后，顺序键按「其余自定义分类的当前列表」重写，
        // 防止顺序键残留悬空 id。
        // 剩余为空时直接删掉顺序键：QStringList().join() 返回的是 null QString，
        // upsert 进 NOT NULL 的 settings.value 会报约束失败（「删除最后一个
        // 自定义分类」必触发）；且空列表本就不该留顺序键。
        QSqlQuery orderQuery(_database.connection());
        orderQuery.prepare(QStringLiteral(
            "SELECT key FROM settings WHERE key LIKE '__custom_category::%' ORDER BY key ASC"));
        if (!orderQuery.exec()) {
            transaction.rollback();
            return Status::failure(describeError(orderQuery));
        }
        QStringList remaining;
        const QString prefix = QStringLiteral("__custom_category::");
        while (orderQuery.next()) {
            remaining.append(orderQuery.value(0).toString().mid(prefix.size()));
        }
        if (remaining.isEmpty()) {
            status = removeSetting(_database, categoryOrderKey());
        } else {
            status = upsertSetting(_database, categoryOrderKey(),
                                   remaining.join(QLatin1Char(',')));
        }
        if (!status.ok()) {
            transaction.rollback();
            return status;
        }
    } else {
        // 内置分类：打墓碑（退出界面；历史数据里的 id 仍可识别，统计不丢）。
        const Status status =
            upsertSetting(_database, deletedCategoryKey(id), QStringLiteral("1"));
        if (!status.ok()) {
            transaction.rollback();
            return status;
        }
    }

    return transaction.commit();
}

Status ClassificationRepository::renameCategory(const QString& categoryId, const QString& labelInput)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    const QString id = categoryId.trimmed();
    const QString label = normalizeCategoryLabel(labelInput);
    if (id.isEmpty()) {
        return Status::failure(QStringLiteral("分类 id 为空"));
    }
    if (label.isEmpty()) {
        return Status::failure(QStringLiteral("分类名称不能为空"));
    }
    if (label.size() > 20) {
        return Status::failure(QStringLiteral("分类名称过长（最多 20 字）"));
    }
    if (id == QStringLiteral("other")) {
        return Status::failure(QStringLiteral("「其他」是兜底分类，不可改名"));
    }

    // 内置分类改回默认名 = 清覆盖键。
    const CategoryToken token = categoryToken(id);
    const QVector<CategoryToken> builtin = categoryTokens();
    const bool isBuiltin = std::any_of(builtin.cbegin(), builtin.cend(),
                                       [&id](const CategoryToken& item) {
                                           return item.id == id;
                                       });
    const QString key = categoryLabelOverrideKey(id);
    if (isBuiltin && token.label == label) {
        Transaction transaction(_database);
        if (!transaction.isActive()) {
            return Status::failure(QStringLiteral("无法开启写事务"));
        }
        const Status removed = removeSetting(_database, key);
        if (!removed.ok()) {
            transaction.rollback();
            return removed;
        }
        return transaction.commit();
    }

    Transaction transaction(_database);
    if (!transaction.isActive()) {
        return Status::failure(QStringLiteral("无法开启写事务"));
    }
    const Status saved = upsertSetting(_database, key, label);
    if (!saved.ok()) {
        transaction.rollback();
        return saved;
    }
    return transaction.commit();
}

Status ClassificationRepository::setCategoryColor(const QString& categoryId,
                                                  const QString& colorHexInput)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    const QString id = categoryId.trimmed();
    if (id.isEmpty()) {
        return Status::failure(QStringLiteral("分类 id 为空"));
    }
    if (id == QStringLiteral("other")) {
        // OTHER_CATEGORY_FIXED_COLOR：兜底分类固定色，不提供覆盖
        //（界面右键菜单对 other 不出配色项）。
        return Status::failure(QStringLiteral("「其他」是兜底分类，颜色固定不可修改"));
    }
    // 内置墓碑分类不再可配色（界面上已经不可见）。
    const Result<QVector<CategoryEntry>> registry = loadCategoryRegistry(_database);
    if (!registry.ok()) {
        return Status::failure(registry.error);
    }
    const bool visible = std::any_of(registry.value.cbegin(), registry.value.cend(),
                                     [&id](const CategoryEntry& entry) {
                                         return entry.id == id;
                                     });
    if (!visible) {
        return Status::failure(QStringLiteral("分类不可用或不存在：%1").arg(id));
    }

    const QString key = categoryColorOverrideKey(id);
    const QString trimmed = colorHexInput.trimmed();
    if (trimmed.isEmpty()) {
        // 空 = 重置：清键回默认色。
        Transaction transaction(_database);
        if (!transaction.isActive()) {
            return Status::failure(QStringLiteral("无法开启写事务"));
        }
        const Status removed = removeSetting(_database, key);
        if (!removed.ok()) {
            transaction.rollback();
            return removed;
        }
        return transaction.commit();
    }

    QString normalized;
    if (!normalizeColorHex(trimmed, &normalized)) {
        return Status::failure(QStringLiteral("颜色格式非法：%1").arg(trimmed));
    }

    Transaction transaction(_database);
    if (!transaction.isActive()) {
        return Status::failure(QStringLiteral("无法开启写事务"));
    }
    const Status saved = upsertSetting(_database, key, normalized);
    if (!saved.ok()) {
        transaction.rollback();
        return saved;
    }
    return transaction.commit();
}

Status ClassificationRepository::setCustomCategoryOrder(const QStringList& orderedIds)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    // 只保留合法的自定义 id，重复项去重（顺序键是完整覆盖写）。
    QStringList ids;
    for (const QString& raw : orderedIds) {
        const QString id = raw.trimmed();
        if (!id.isEmpty() && !ids.contains(id)
            && QRegularExpression(QStringLiteral("^custom:category_[0-9a-f]{16,}$"))
                   .match(id)
                   .hasMatch()) {
            ids.append(id);
        }
    }

    Transaction transaction(_database);
    if (!transaction.isActive()) {
        return Status::failure(QStringLiteral("无法开启写事务"));
    }
    const Status saved =
        upsertSetting(_database, categoryOrderKey(), ids.join(QLatin1Char(',')));
    if (!saved.ok()) {
        transaction.rollback();
        return saved;
    }
    return transaction.commit();
}

Status ClassificationRepository::resetAllColors()
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    Transaction transaction(_database);
    if (!transaction.isActive()) {
        return Status::failure(QStringLiteral("无法开启写事务"));
    }

    // 分类配色覆盖键：LIKE 前缀匹配整批删。
    {
        QSqlQuery query(_database.connection());
        query.prepare(QStringLiteral(
            "DELETE FROM settings WHERE key LIKE '__category_color_override::%'"));
        if (!query.exec()) {
            transaction.rollback();
            return Status::failure(describeError(query));
        }
    }
    // 应用自定义色：清 color_hex 即回「跟随分类色」（空串语义）。
    {
        QSqlQuery query(_database.connection());
        query.prepare(QStringLiteral("UPDATE apps SET color_hex = ''"));
        if (!query.exec()) {
            transaction.rollback();
            return Status::failure(describeError(query));
        }
    }

    return transaction.commit();
}

} // namespace patina::data
