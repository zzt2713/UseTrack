#ifndef PATINA_DATA_APP_LINKS_H
#define PATINA_DATA_APP_LINKS_H

#include "data/types.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>

namespace patina::data
{

class Database;

// 关联应用。
//
// 语义：用户可以把若干 exe 声明成「同一个应用」——一个父应用 + 若干成员。
// 统计时成员的时长并到父应用头上，界面只出现父应用一行；展示名、分类、
// 配色一律取父应用的覆盖项。历史行（sessions/apps）一个字节都不动，
// 关联只是读时归并口径，所以解除关联之后数字自然回原样。
//
// 存储是 settings 表里的一条键：
//
//     __app_link::<成员 exe> = <父 exe>
//
// 两边都是小写 exe 名（apps.exe_name 是 COLLATE NOCASE，小写是它的规范形）。
// 只存父 exe 名，不存 JSON：本项目没有前端草稿层，不需要为并发冲突检测留快照。
//
// 约束：
//   - 成员与父都必须是合法身份（小写、无空白、无控制字符、长度有上限）；
//   - 不能自指，也不能嵌套（父本身不能再是别人的成员）；
//   - 一个应用要么是父、要么是成员，不能同时是两者。
inline QString appLinkKeyPrefix() { return QStringLiteral("__app_link::"); }

QString appLinkKey(const QString& member);

// 能否作为关联的成员或父。空、含空白、含控制字符、含路径分隔符、
// 带大写的都拒绝（写库前统一 toLower，这里只做兜底校验）。
bool isValidAppLinkIdentity(const QString& exeName);

// 成员 exe（小写）→ 父 exe（小写）。
using AppLinks = QHash<QString, QString>;

Result<AppLinks> loadAppLinks(Database& database);

// parent 为空 = 解除关联。整表校验在写入前做：改完之后仍不能出现自指或嵌套。
Status setAppLink(Database& database, const QString& member, const QString& parent);

// 统计口径的归并键（小写）。未关联时就是 exeName 自己。
QString statisticalAppKey(const QString& exeName, const AppLinks& links);

// 父应用 + 它的全部成员（小写，父排第一，其余按字典序）。
// 未关联时只返回自己。详情页按它把成员的活动段一起捞出来。
QStringList linkedAppKeys(const QString& exeName, const AppLinks& links);

// 应用身份：图标、展示名、分类、配色都要用。键是小写 exe。
struct AppIdentity
{
    qint64 appId = 0;
    QString exeName;
    QString exePath;
    QString displayName;
    QString categoryId;
    QString colorHex;
    qint64 lastSeenMs = 0;
};

Result<QHash<QString, AppIdentity>> loadAppIdentities(Database& database);

// 归并之后该用哪份身份：有关联且父应用在 apps 表里时返回父的，
// 否则返回空（调用方保留自己那一行）。
std::optional<AppIdentity> linkedIdentity(const QString& exeName,
                                          const AppLinks& links,
                                          const QHash<QString, AppIdentity>& identities);

} // namespace patina::data

#endif // PATINA_DATA_APP_LINKS_H
