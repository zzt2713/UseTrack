#include "data/settings_repository.h"

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

} // namespace

Result<std::optional<QString>> SettingsRepository::value(const QString& key) const
{
    if (!_database.isOpen()) {
        return fail<std::optional<QString>>(QStringLiteral("数据库未打开"));
    }

    const QString trimmed = key.trimmed();
    if (trimmed.isEmpty()) {
        return fail<std::optional<QString>>(QStringLiteral("key 为空"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ? LIMIT 1"));
    query.addBindValue(trimmed);
    if (!query.exec()) {
        return fail<std::optional<QString>>(describeError(query));
    }
    if (!query.next()) {
        return succeed(std::optional<QString>{});
    }
    return succeed(std::optional<QString>{query.value(0).toString()});
}

Status SettingsRepository::setValue(const QString& key, const QString& value)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    const QString trimmed = key.trimmed();
    if (trimmed.isEmpty()) {
        return Status::failure(QStringLiteral("key 为空"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "INSERT INTO settings (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    query.addBindValue(trimmed);
    query.addBindValue(value);
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

Result<QVector<SettingRow>> SettingsRepository::listAll() const
{
    if (!_database.isOpen()) {
        return fail<QVector<SettingRow>>(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    if (!query.exec(QStringLiteral("SELECT key, value FROM settings ORDER BY key ASC"))) {
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

Status SettingsRepository::remove(const QString& key)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral("DELETE FROM settings WHERE key = ?"));
    query.addBindValue(key.trimmed());
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

} // namespace patina::data
