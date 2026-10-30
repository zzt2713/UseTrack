#include "data/app_links.h"

#include "data/database.h"
#include "data/settings_repository.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>

namespace patina::data
{
namespace
{

// apps 表是「装过的软件」规模（几百），这里只是防御性上限（读查询必须有界）。
constexpr int kMaxAppIdentities = 4096;
// Windows 文件名上限 255，留一点余量；超长的一定不是正常 exe 名。
constexpr int kMaxIdentityLength = 260;

QString describeError(const QSqlQuery& query)
{
    const QSqlError error = query.lastError();
    return error.isValid() ? error.text() : QStringLiteral("SQL 执行失败");
}

// 规范形：去空白 + 小写。apps.exe_name 是 COLLATE NOCASE，小写是它的等价类代表。
QString canonicalIdentity(const QString& exeName)
{
    return exeName.trimmed().toLower();
}

// 整表一致性：不能自指，不能嵌套（父本身不能再是别人的成员）。
// 写入前对「改完之后」的整张表跑一遍，比逐条判断更好推理。
bool linksAreConsistent(const AppLinks& links, QString* error)
{
    for (auto it = links.cbegin(); it != links.cend(); ++it) {
        const QString member = it.key();
        const QString parent = it.value();
        if (member == parent) {
            if (error != nullptr) {
                *error = QStringLiteral("应用不能关联到自己");
            }
            return false;
        }
        // 父本身也是成员 → 会形成链，统计归并键就不唯一了。
        if (links.contains(parent)) {
            if (error != nullptr) {
                *error = QStringLiteral("「%1」已经关联到别的应用，不能再作为父应用").arg(parent);
            }
            return false;
        }
    }
    return true;
}

} // namespace

QString appLinkKey(const QString& member)
{
    return appLinkKeyPrefix() + canonicalIdentity(member);
}

bool isValidAppLinkIdentity(const QString& exeName)
{
    const QString value = exeName.trimmed();
    if (value.isEmpty() || value.size() > kMaxIdentityLength) {
        return false;
    }
    // 必须是规范形：小写、无空白、无控制字符、不含路径分隔符。
    // 调用方传原始 exe 名进来时先自己 toLower（写库路径内部会规范化）。
    if (value != value.toLower()) {
        return false;
    }
    for (const QChar character : value) {
        if (character.isSpace() || character.isNull() || character < QChar(0x20)) {
            return false;
        }
        if (character == QLatin1Char('/') || character == QLatin1Char('\\')
            || character == QLatin1Char(':')) {
            return false;
        }
    }
    return true;
}

Result<AppLinks> loadAppLinks(Database& database)
{
    if (!database.isOpen()) {
        return fail<AppLinks>(QStringLiteral("数据库未打开"));
    }

    SettingsRepository repository(database);
    const Result<QVector<SettingRow>> rows = repository.listAll();
    if (!rows.ok()) {
        return fail<AppLinks>(rows.error);
    }

    const QString prefix = appLinkKeyPrefix();
    AppLinks links;
    for (const SettingRow& row : rows.value) {
        if (!row.key.startsWith(prefix)) {
            continue;
        }
        const QString member = canonicalIdentity(row.key.mid(prefix.size()));
        const QString parent = canonicalIdentity(row.value);
        // 库里的脏数据（自指、父为空）直接忽略：读路径不能因为一条坏键整页失败。
        if (member.isEmpty() || parent.isEmpty() || member == parent) {
            continue;
        }
        links.insert(member, parent);
    }
    return succeed(links);
}

Status setAppLink(Database& database, const QString& member, const QString& parent)
{
    if (!database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    const QString memberKey = canonicalIdentity(member);
    if (!isValidAppLinkIdentity(memberKey)) {
        return Status::failure(QStringLiteral("应用标识不合法"));
    }

    const Result<AppLinks> current = loadAppLinks(database);
    if (!current.ok()) {
        return Status::failure(current.error);
    }

    AppLinks next = current.value;
    next.remove(memberKey);

    const QString parentKey = canonicalIdentity(parent);
    if (!parentKey.isEmpty()) {
        if (!isValidAppLinkIdentity(parentKey)) {
            return Status::failure(QStringLiteral("父应用标识不合法"));
        }
        next.insert(memberKey, parentKey);
        QString error;
        if (!linksAreConsistent(next, &error)) {
            return Status::failure(error);
        }
    } else {
        // 解除关联：如果这个应用自己就是父，它的成员要一起放开，
        // 否则成员会指向一个不再是父的父应用，界面上就出现「挂在别人名下」
        // 却查不到父应用的悬挂状态。
        for (auto it = next.begin(); it != next.end();) {
            if (it.value() == memberKey) {
                it = next.erase(it);
            } else {
                ++it;
            }
        }
    }

    SettingsRepository repository(database);
    if (parentKey.isEmpty()) {
        const Status removed = repository.remove(appLinkKey(memberKey));
        // 键本来就不存在不算失败：解除一个未关联的应用是幂等操作。
        if (!removed.ok()) {
            return removed;
        }
    } else {
        const Status written = repository.setValue(appLinkKey(memberKey), parentKey);
        if (!written.ok()) {
            return written;
        }
    }

    // 父应用自己的关联键要清掉：一个应用要么是父要么是成员。
    if (!parentKey.isEmpty()) {
        const Status cleaned = repository.remove(appLinkKey(parentKey));
        if (!cleaned.ok()) {
            return cleaned;
        }
    }
    return Status::success();
}

QString statisticalAppKey(const QString& exeName, const AppLinks& links)
{
    const QString key = canonicalIdentity(exeName);
    if (key.isEmpty()) {
        return key;
    }
    const auto it = links.constFind(key);
    return it == links.constEnd() ? key : it.value();
}

QStringList linkedAppKeys(const QString& exeName, const AppLinks& links)
{
    const QString key = canonicalIdentity(exeName);
    if (key.isEmpty()) {
        return {};
    }

    QStringList result;
    const auto it = links.constFind(key);
    if (it != links.constEnd()) {
        // 传进来的是成员：父 + 全部兄弟成员。
        result.append(it.value());
        QStringList members;
        for (auto member = links.cbegin(); member != links.cend(); ++member) {
            if (member.value() == it.value()) {
                members.append(member.key());
            }
        }
        members.sort();
        result.append(members);
        return result;
    }

    // 传进来的是父（或未关联）：自己 + 挂在它名下的成员。
    result.append(key);
    QStringList members;
    for (auto member = links.cbegin(); member != links.cend(); ++member) {
        if (member.value() == key) {
            members.append(member.key());
        }
    }
    members.sort();
    result.append(members);
    return result;
}

Result<QHash<QString, AppIdentity>> loadAppIdentities(Database& database)
{
    if (!database.isOpen()) {
        return fail<QHash<QString, AppIdentity>>(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(database.connection());
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
        "SELECT id, exe_name, exe_path, display_name, category_id, color_hex, last_seen_ms "
        "FROM apps ORDER BY last_seen_ms DESC LIMIT %1").arg(kMaxAppIdentities));
    if (!query.exec()) {
        return fail<QHash<QString, AppIdentity>>(describeError(query));
    }

    QHash<QString, AppIdentity> identities;
    while (query.next()) {
        AppIdentity identity;
        identity.appId = query.value(0).toLongLong();
        identity.exeName = query.value(1).toString();
        identity.exePath = query.value(2).toString();
        identity.displayName = query.value(3).toString();
        identity.categoryId = query.value(4).toString();
        identity.colorHex = query.value(5).toString();
        identity.lastSeenMs = query.value(6).toLongLong();
        const QString key = canonicalIdentity(identity.exeName);
        if (key.isEmpty()) {
            continue;
        }
        identities.insert(key, identity);
    }
    return succeed(identities);
}

std::optional<AppIdentity> linkedIdentity(const QString& exeName,
                                          const AppLinks& links,
                                          const QHash<QString, AppIdentity>& identities)
{
    const QString key = canonicalIdentity(exeName);
    if (key.isEmpty()) {
        return std::nullopt;
    }
    const auto link = links.constFind(key);
    if (link == links.constEnd()) {
        return std::nullopt;
    }
    const auto parent = identities.constFind(link.value());
    if (parent == identities.constEnd()) {
        return std::nullopt;
    }
    return *parent;
}

} // namespace patina::data
