#include "data/app_repository.h"

#include "data/database.h"

#include <QChar>
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

// 显示名净化（读出侧兜底）：版本资源字符串带的尾部 NUL 会被 SQLite 按
// C 字符串语义吞进存储，SQL 侧（substr/replace/unicode）全都看不见它，
// 只有 C++ 读回 QString 时能看到。在这里统一截断控制字符、剔除零宽字符。
// 各读模型（dashboard 等）直接查同名列，同样吃到这里的净化语义——
// 它们若另行复制本函数，改动时要同步。
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

App readApp(const QSqlQuery& query)
{
    App app;
    app.id = query.value(0).toLongLong();
    app.exeName = query.value(1).toString();
    app.exePath = query.value(2).toString();
    app.displayName = sanitizeDisplayName(query.value(3).toString());
    app.categoryId = query.value(4).toString();
    app.colorHex = query.value(5).toString();
    app.trackEnabled = query.value(6).toInt() != 0;
    app.hidden = query.value(7).toInt() != 0;
    app.firstSeenMs = query.value(8).toLongLong();
    app.lastSeenMs = query.value(9).toLongLong();
    return app;
}

const char* const kSelectColumns =
    "SELECT id, exe_name, exe_path, display_name, category_id, color_hex, track_enabled, "
    "hidden, first_seen_ms, last_seen_ms FROM apps";

} // namespace

Result<App> AppRepository::findById(qint64 id) const
{
    if (!_database.isOpen()) {
        return fail<App>(QStringLiteral("数据库未打开"));
    }
    if (id <= 0) {
        return fail<App>(QStringLiteral("app id 非法"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QString::fromLatin1(kSelectColumns) + QStringLiteral(" WHERE id = ?"));
    query.addBindValue(id);
    if (!query.exec()) {
        return fail<App>(describeError(query));
    }
    if (!query.next()) {
        return fail<App>(QStringLiteral("应用 %1 不存在").arg(id));
    }
    return succeed(readApp(query));
}

Result<std::optional<App>> AppRepository::findByExeName(const QString& exeName) const
{
    if (!_database.isOpen()) {
        return fail<std::optional<App>>(QStringLiteral("数据库未打开"));
    }

    const QString trimmed = exeName.trimmed();
    if (trimmed.isEmpty()) {
        return fail<std::optional<App>>(QStringLiteral("exe_name 为空"));
    }

    QSqlQuery query(_database.connection());
    // 列是 COLLATE NOCASE，这里用 = 就能大小写不敏感地命中。
    query.prepare(QString::fromLatin1(kSelectColumns) + QStringLiteral(" WHERE exe_name = ?"));
    query.addBindValue(trimmed);
    if (!query.exec()) {
        return fail<std::optional<App>>(describeError(query));
    }
    if (!query.next()) {
        return succeed(std::optional<App>{});
    }
    return succeed(std::optional<App>{readApp(query)});
}

Result<App> AppRepository::touch(const QString& exeName, const QString& exePath, qint64 nowMs)
{
    if (!_database.isOpen()) {
        return fail<App>(QStringLiteral("数据库未打开"));
    }

    const QString trimmedName = exeName.trimmed();
    if (trimmedName.isEmpty()) {
        return fail<App>(QStringLiteral("exe_name 为空"));
    }

    QSqlQuery query(_database.connection());
    // 一条语句完成 upsert：exe_name 有 UNIQUE 约束，冲突时只刷新最近见到的信息。
    // first_seen_ms 用 excluded 之外的原值，插入时才是 nowMs。
    query.prepare(QStringLiteral(
        "INSERT INTO apps (exe_name, exe_path, first_seen_ms, last_seen_ms) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT(exe_name) DO UPDATE SET "
        "    last_seen_ms = excluded.last_seen_ms,"
        "    exe_path = CASE WHEN excluded.exe_path <> '' THEN excluded.exe_path ELSE apps.exe_path END"));
    query.addBindValue(trimmedName);
    // 探不到进程映像路径时 exePath 是 null QString，trimmed() 仍是 null，
    // 绑成 SQL NULL 会撞上 exe_path NOT NULL 约束，连带整条开段事务回滚。
    const QString trimmedPath = exePath.trimmed();
    query.addBindValue(trimmedPath.isNull() ? QStringLiteral("") : trimmedPath);
    query.addBindValue(nowMs);
    query.addBindValue(nowMs);
    if (!query.exec()) {
        return fail<App>(describeError(query));
    }

    const Result<std::optional<App>> found = findByExeName(trimmedName);
    if (!found.ok()) {
        return fail<App>(found.error);
    }
    if (!found.value.has_value()) {
        return fail<App>(QStringLiteral("写入应用后读回失败"));
    }
    return succeed(*found.value);
}

Status AppRepository::ensureDisplayName(const QString& exeName, const QString& displayName)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    const QString trimmedName = exeName.trimmed();
    const QString trimmedDisplay = displayName.trimmed();
    if (trimmedName.isEmpty()) {
        return Status::failure(QStringLiteral("exe_name 为空"));
    }
    if (trimmedDisplay.isEmpty()) {
        return Status::success();
    }

    // 只在库里那一格还是空的时候写：用户自己改过的名字不能被覆盖。
    // 一个语句完成「条件更新」，并发下也不会把已有名字刷掉。
    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "UPDATE apps SET display_name = ? "
        "WHERE exe_name = ? AND (display_name IS NULL OR display_name = '')"));
    query.addBindValue(trimmedDisplay);
    query.addBindValue(trimmedName);
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

} // namespace patina::data
