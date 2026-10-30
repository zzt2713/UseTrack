#ifndef PATINA_DATA_APP_OVERRIDE_H
#define PATINA_DATA_APP_OVERRIDE_H

#include "data/types.h"

#include <QString>
#include <QVector>

#include <optional>

namespace patina::data
{

class Database;

// 按应用覆盖的写侧。
//
// 读侧在 engine/tracking_settings.cpp（TrackingSettingsCache::readAppOverride，
// 5 秒 TTL 缓存）。这里只负责落库，键与格式必须与读侧严格一致：
//   settings["__app_override::<exe 小写>"] = {"track":bool,"captureTitle":bool}
//
// 为什么单独一个文件而不是塞进 ClassificationRepository：
// 「不计入统计」走的是 apps.track_enabled 列（applyPatches），而
// captureTitle 走的是 settings 键，两套机制、两张表、两种事务边界。
// 混在一个仓储里，保存分类草稿时会连带写出 settings 键，回滚语义就乱了。
//
// 写入规则：
//   - 规范化 exe 名失败 → 什么都不做；
//   - 覆盖项全为空（等于没有任何非默认设置）→ 删键，回落到默认；
//   - 否则整份 JSON 覆盖写。
struct AppOverridePatch
{
    QString exeName;
    // nullopt = 保持默认（记录标题）；true/false = 显式覆盖。
    std::optional<bool> captureTitle;
};

// 规范化 exe 标识：trim + 小写。空、含空白、含控制字符、含路径分隔符的
// 一律返回空串（调用方据此判失败）。与 app_links.h 的 canonicalIdentity 同口径，
// 但那边是关联应用用的，这边是覆盖键用的，各自独立避免互相牵制。
QString canonicalAppOverrideIdentity(const QString& exeName);

// 读当前覆盖项。键不存在 / 解析失败 → 空 patch（captureTitle 为 nullopt）。
Result<AppOverridePatch> loadAppOverride(Database& database, const QString& exeName);

// 批量写。captureTitle 为 nullopt 且整份 patch 没有任何覆盖项时删键。
// 全部写在一个事务里，任何一条失败整体回滚。
Status saveAppOverrides(Database& database, const QVector<AppOverridePatch>& patches);

} // namespace patina::data

#endif // PATINA_DATA_APP_OVERRIDE_H
