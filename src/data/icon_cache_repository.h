#ifndef PATINA_DATA_ICON_CACHE_REPOSITORY_H
#define PATINA_DATA_ICON_CACHE_REPOSITORY_H

#include "data/types.h"

#include <QImage>
#include <QString>

namespace patina::data
{

class Database;

// icon_cache 表（schema v8）的读写：exe 名 → PNG 位图。
//
// 键是 exe 名（大小写不敏感，表列带 COLLATE NOCASE），值是 PNG 编码的图标；
// 不做 base64 包装，直接存 BLOB。生命周期简化为「首次提取后写库」。
//
// 上限：写入由调用方（platform 的图标提取层）把守——本表只服务应用图标，
// 条目数天然 ≤ 内存红线的 256 项 LRU 能容纳的 exe 个数。
class IconCacheRepository
{
public:
    explicit IconCacheRepository(Database& database) : _database(database) {}

    // 读图标。命中返回解码后的 QImage；未命中或解码失败返回空图（不算错误）。
    Result<QImage> loadIcon(const QString& exeName) const;

    // 写图标（PNG 编码落库，ON CONFLICT 覆盖）。image 为空时是负缓存：存一条
    // 零字节 BLOB，下次启动免得再解一遍注定失败的 exe。
    Status saveIcon(const QString& exeName, const QImage& image, qint64 nowMs);

private:
    Database& _database;
};

} // namespace patina::data

#endif // PATINA_DATA_ICON_CACHE_REPOSITORY_H
