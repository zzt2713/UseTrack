#include "engine/tracking_settings.h"

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>

namespace patina::engine
{
namespace
{

constexpr qint64 kSettingsCacheTtlMs = 5'000;
constexpr qint64 kDefaultContinuityWindowSecs = 180;
constexpr qint64 kDefaultSustainedWindowSecs = 900;
constexpr int kAppOverrideCacheMaxEntries = 256;

std::optional<AppOverride> parseOverrideJson(const QString& raw)
{
    if (raw.trimmed().isEmpty()) {
        return std::nullopt;
    }
    const QJsonDocument document = QJsonDocument::fromJson(raw.toUtf8());
    if (!document.isObject()) {
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    AppOverride override;
    if (object.contains(QStringLiteral("track")) && object.value(QStringLiteral("track")).isBool()) {
        override.track = object.value(QStringLiteral("track")).toBool();
    }
    if (object.contains(QStringLiteral("captureTitle"))
        && object.value(QStringLiteral("captureTitle")).isBool()) {
        override.captureTitle = object.value(QStringLiteral("captureTitle")).toBool();
    }
    return override;
}

// 按应用覆盖的缓存：exe → (override, loadedAt)。
struct OverrideCacheEntry
{
    std::optional<AppOverride> overrideValue;
    qint64 loadedAtMs = 0;
};

QHash<QString, OverrideCacheEntry>& overrideCache()
{
    static QHash<QString, OverrideCacheEntry> cache;
    return cache;
}

QMutex& overrideCacheMutex()
{
    static QMutex mutex;
    return mutex;
}

void pruneOverrideCache(qint64 nowMs)
{
    auto& cache = overrideCache();
    // Qt6 的 QHash 没有 removeIf，手写一遍：先收集要删的 key 再逐个删。
    QList<QString> expiredKeys;
    for (auto it = cache.begin(); it != cache.end(); ++it) {
        if (nowMs - it->loadedAtMs >= kSettingsCacheTtlMs) {
            expiredKeys.append(it.key());
        }
    }
    for (const QString& key : expiredKeys) {
        cache.remove(key);
    }
    while (cache.size() > kAppOverrideCacheMaxEntries) {
        // LRU 简化为「清空一半」：实际使用中应用数远小于 256，不追求精确。
        auto it = cache.begin();
        for (int removed = 0; removed < cache.size() / 2 && it != cache.end();) {
            it = cache.erase(it);
            ++removed;
        }
        break;
    }
}

} // namespace

std::optional<AppOverride> parseAppOverride(const QString& rawValue)
{
    return parseOverrideJson(rawValue);
}

TrackingSettingsCache::TrackingSettingsCache(engine::SessionStore& store) : _store(store) {}

bool TrackingSettingsCache::readBoolWithCache(CachedBool& cache,
                                              const QString& key,
                                              bool defaultValue,
                                              qint64 nowMs,
                                              bool failClosed)
{
    if (cache.valid && nowMs - cache.loadedAtMs < kSettingsCacheTtlMs) {
        return cache.value;
    }

    const data::Result<std::optional<QString>> raw = _store.loadSettingValue(key);
    if (!raw.ok()) {
        // 读失败沿用缓存；无缓存用 failClosed 决定安全侧。
        if (cache.valid) {
            return cache.value;
        }
        return failClosed;
    }
    if (!raw.value.has_value()) {
        // 键不存在：用默认值，但不算缓存命中（键写进来后下一帧生效）。
        cache.value = defaultValue;
        cache.loadedAtMs = nowMs;
        cache.valid = true;
        return defaultValue;
    }

    const QString value = raw.value->trimmed().toLower();
    const bool parsed = value == QLatin1String("true") || value == QLatin1String("1")
                        || value == QLatin1String("yes");
    cache.value = parsed;
    cache.loadedAtMs = nowMs;
    cache.valid = true;
    return parsed;
}

qint64 TrackingSettingsCache::readSecondsWithCache(CachedSeconds& cache,
                                                   const QString& key,
                                                   qint64 defaultSeconds,
                                                   qint64 nowMs)
{
    if (cache.valid && nowMs - cache.loadedAtMs < kSettingsCacheTtlMs) {
        return cache.value;
    }

    const data::Result<std::optional<QString>> raw = _store.loadSettingValue(key);
    if (raw.ok() && raw.value.has_value()) {
        bool converted = false;
        const qint64 parsed = raw.value->toLongLong(&converted);
        if (converted && parsed > 0) {
            cache.value = parsed;
            cache.loadedAtMs = nowMs;
            cache.valid = true;
            return parsed;
        }
    }
    // 读失败 / 值坏：沿用缓存；无缓存用默认。
    if (cache.valid) {
        return cache.value;
    }
    cache.value = defaultSeconds;
    cache.loadedAtMs = nowMs;
    cache.valid = true;
    return defaultSeconds;
}

qint64 TrackingSettingsCache::continuityWindowMs(qint64 nowMs)
{
    return readSecondsWithCache(_continuity, timelineMergeGapKey(),
                                kDefaultContinuityWindowSecs, nowMs)
           * 1000;
}

qint64 TrackingSettingsCache::sustainedWindowMs(qint64 nowMs)
{
    return readSecondsWithCache(_sustained, idleTimeoutKey(),
                                kDefaultSustainedWindowSecs, nowMs)
           * 1000;
}

qint64 TrackingSettingsCache::afkThresholdMs(qint64 nowMs)
{
    return continuityWindowMs(nowMs);
}

bool TrackingSettingsCache::trackingPaused(qint64 nowMs)
{
    if (_pausedHintValid) {
        // setPaused 的直接写入优先于读库：调用方刚改完，不该被 TTL 旧值盖回去。
        return _trackingPausedHint.value;
    }
    // 读失败 + 无缓存 → 安全暂停（fail closed）。
    return readBoolWithCache(_trackingPaused, trackingPausedKey(), false, nowMs, /*failClosed*/ true);
}

void TrackingSettingsCache::setTrackingPausedHint(bool paused)
{
    _trackingPausedHint.value = paused;
    _trackingPausedHint.valid = true;
    _pausedHintValid = true;
    // 同步清掉读库缓存，下一次 TTL 过期重读时拿到的是新值。
    _trackingPaused.valid = false;
}

bool TrackingSettingsCache::titleRecordingEnabled(qint64 nowMs)
{
    if (_titleHintValid) {
        // 与暂停键同款：托盘刚拨过开关，不能被 TTL 内的旧值盖回去。
        return _titleRecordingHint.value;
    }
    // 读失败 + 无缓存 → 允许记录（fail open）。
    return readBoolWithCache(_titleRecording, titleRecordingEnabledKey(), true, nowMs,
                             /*failClosed*/ false);
}

void TrackingSettingsCache::setTitleRecordingHint(bool enabled)
{
    _titleRecordingHint.value = enabled;
    _titleRecordingHint.valid = true;
    _titleHintValid = true;
    _titleRecording.valid = false;
}

std::optional<AppOverride> TrackingSettingsCache::readAppOverride(const QString& exeName,
                                                                  qint64 nowMs)
{
    const QString key = exeName.trimmed().toLower();
    if (key.isEmpty()) {
        return std::nullopt;
    }

    {
        QMutexLocker locker(&overrideCacheMutex());
        pruneOverrideCache(nowMs);
        const auto it = overrideCache().constFind(key);
        if (it != overrideCache().constEnd()) {
            return it->overrideValue;
        }
    }

    const data::Result<std::optional<QString>> raw =
        _store.loadSettingValue(appOverrideKeyPrefix() + key);
    std::optional<AppOverride> override;
    if (raw.ok() && raw.value.has_value()) {
        override = parseAppOverride(*raw.value);
    } else if (!raw.ok()) {
        // 读失败：不缓存，让下一帧重试。
        return std::nullopt;
    }

    QMutexLocker locker(&overrideCacheMutex());
    pruneOverrideCache(nowMs);
    OverrideCacheEntry entry;
    entry.overrideValue = override;
    entry.loadedAtMs = nowMs;
    overrideCache().insert(key, entry);
    return override;
}

bool TrackingSettingsCache::captureTitleForApp(const QString& exeName, qint64 nowMs)
{
    const std::optional<AppOverride> override = readAppOverride(exeName, nowMs);
    if (!override.has_value() || !override->captureTitle.has_value()) {
        return true;
    }
    return *override->captureTitle;
}

bool TrackingSettingsCache::anonymousForApp(const QString& exeName, qint64 nowMs)
{
    const std::optional<AppOverride> override = readAppOverride(exeName, nowMs);
    if (!override.has_value() || !override->track.has_value()) {
        return false;
    }
    // track=false 表示「这个应用的时长匿名化」：不落 exe/标题，只记时长。
    return !*override->track;
}

} // namespace patina::engine
