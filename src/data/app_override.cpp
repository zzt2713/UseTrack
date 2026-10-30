#include "data/app_override.h"

#include "data/database.h"
#include "data/settings_repository.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QPair>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace patina::data
{
namespace
{

// 键前缀必须与 engine/tracking_settings.h 的 appOverrideKeyPrefix() 一致。
// 不直接 include engine/ 头：data/ 不该反向依赖 engine/（目录边界）。
// 两边靠这条注释 + 单元测试（若有）对齐，改一处必须改另一处。
const QString kAppOverrideKeyPrefix = QStringLiteral("__app_override::");

constexpr int kMaxIdentityLength = 260;

QString describeError(const QSqlQuery& query)
{
    return query.lastError().text().trimmed();
}

QString overrideKeyFor(const QString& canonicalExe)
{
    return kAppOverrideKeyPrefix + canonicalExe;
}

// 从 settings 值解析。只认 captureTitle：track 是历史遗留（现在走
// apps.track_enabled 列），读到也忽略，避免两套机制打架。
std::optional<bool> parseCaptureTitle(const QString& rawValue)
{
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(rawValue.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    const QJsonValue value = object.value(QStringLiteral("captureTitle"));
    if (!value.isBool()) {
        return std::nullopt;
    }
    return value.toBool();
}

QString serializeCaptureTitle(bool captureTitle)
{
    QJsonObject object;
    object.insert(QStringLiteral("captureTitle"), captureTitle);
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

} // namespace

QString canonicalAppOverrideIdentity(const QString& exeName)
{
    const QString value = exeName.trimmed();
    if (value.isEmpty() || value.size() > kMaxIdentityLength) {
        return QString();
    }
    for (const QChar ch : value) {
        if (ch.isSpace() || ch.isNull() || ch < QChar(0x20)) {
            return QString();
        }
        if (ch == QLatin1Char('/') || ch == QLatin1Char('\\') || ch == QLatin1Char(':')) {
            return QString();
        }
    }
    return value.toLower();
}

Result<AppOverridePatch> loadAppOverride(Database& database, const QString& exeName)
{
    if (!database.isOpen()) {
        return fail<AppOverridePatch>(QStringLiteral("数据库未打开"));
    }
    const QString canonical = canonicalAppOverrideIdentity(exeName);
    if (canonical.isEmpty()) {
        return fail<AppOverridePatch>(QStringLiteral("应用标识不合法"));
    }

    SettingsRepository repository(database);
    const Result<std::optional<QString>> raw = repository.value(overrideKeyFor(canonical));
    if (!raw.ok()) {
        return fail<AppOverridePatch>(raw.error);
    }

    AppOverridePatch patch;
    patch.exeName = canonical;
    if (raw.value.has_value()) {
        patch.captureTitle = parseCaptureTitle(*raw.value);
    }
    return succeed(patch);
}

Status saveAppOverrides(Database& database, const QVector<AppOverridePatch>& patches)
{
    if (!database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }
    if (patches.isEmpty()) {
        return Status::success();
    }

    // 先整批校验再进事务：非法输入不留任何痕迹。
    QVector<QPair<QString, std::optional<bool>>> planned;
    planned.reserve(patches.size());
    for (const AppOverridePatch& patch : patches) {
        const QString canonical = canonicalAppOverrideIdentity(patch.exeName);
        if (canonical.isEmpty()) {
            return Status::failure(QStringLiteral("应用标识不合法：%1").arg(patch.exeName));
        }
        planned.append(qMakePair(canonical, patch.captureTitle));
    }

    Transaction transaction(database);
    if (!transaction.isActive()) {
        return Status::failure(QStringLiteral("无法开启覆盖项写入事务"));
    }

    SettingsRepository repository(database);
    for (const auto& item : planned) {
        const QString key = overrideKeyFor(item.first);
        if (!item.second.has_value()) {
            // 没有任何覆盖项 → 删键回默认。键不存在不算失败（幂等）。
            const Status removed = repository.remove(key);
            if (!removed.ok()) {
                return Status::failure(removed.error);
            }
            continue;
        }
        const Status written = repository.setValue(key, serializeCaptureTitle(*item.second));
        if (!written.ok()) {
            return Status::failure(written.error);
        }
    }

    const Status committed = transaction.commit();
    if (!committed.ok()) {
        return Status::failure(committed.error);
    }
    return Status::success();
}

} // namespace patina::data
