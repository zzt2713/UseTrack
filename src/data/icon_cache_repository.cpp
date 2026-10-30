#include "data/icon_cache_repository.h"

#include "data/database.h"

#include <QBuffer>
#include <QImage>
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

Result<QImage> IconCacheRepository::loadIcon(const QString& exeName) const
{
    if (!_database.isOpen()) {
        return fail<QImage>(QStringLiteral("数据库未打开"));
    }

    const QString trimmed = exeName.trimmed();
    if (trimmed.isEmpty()) {
        return succeed(QImage());
    }

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "SELECT icon_png FROM icon_cache WHERE exe_name = ? COLLATE NOCASE"));
    query.addBindValue(trimmed);
    if (!query.exec()) {
        return fail<QImage>(describeError(query));
    }
    if (!query.next()) {
        // 未命中：正常路径，不算错误。
        return succeed(QImage());
    }

    const QByteArray png = query.value(0).toByteArray();
    if (png.isEmpty()) {
        // 负缓存条目：这个 exe 解不出图标，别再试了。
        return succeed(QImage());
    }

    QImage image;
    if (!image.loadFromData(png, "PNG")) {
        // 位图坏了等价于没有：让调用方重新提取并覆盖。
        return succeed(QImage());
    }
    return succeed(image);
}

Status IconCacheRepository::saveIcon(const QString& exeName, const QImage& image, qint64 nowMs)
{
    if (!_database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    const QString trimmed = exeName.trimmed();
    if (trimmed.isEmpty()) {
        return Status::failure(QStringLiteral("exe 名为空"));
    }

    QByteArray png;
    if (!image.isNull()) {
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        // ARGB32 的 PNG 保住透明通道；32px 的图标只有几 KB。
        image.save(&buffer, "PNG");
        buffer.close();
        if (png.isEmpty()) {
            return Status::failure(QStringLiteral("图标编码 PNG 失败"));
        }
    }
    // image 为空：png 保持空字节串，存成负缓存条目。

    QSqlQuery query(_database.connection());
    query.prepare(QStringLiteral(
        "INSERT INTO icon_cache (exe_name, icon_png, last_updated_ms) VALUES (?, ?, ?) "
        "ON CONFLICT(exe_name) DO UPDATE SET "
        "    icon_png = excluded.icon_png, last_updated_ms = excluded.last_updated_ms"));
    query.addBindValue(trimmed);
    query.addBindValue(png);
    query.addBindValue(nowMs);
    if (!query.exec()) {
        return Status::failure(describeError(query));
    }
    return Status::success();
}

} // namespace patina::data
