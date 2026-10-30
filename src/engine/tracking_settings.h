#ifndef PATINA_ENGINE_TRACKING_SETTINGS_H
#define PATINA_ENGINE_TRACKING_SETTINGS_H

#include "engine/session_store.h"

#include <QString>

#include <optional>

namespace patina::engine
{

// settings 键名。
inline QString trackingPausedKey()
{
    return QStringLiteral("tracking_paused");
}
inline QString titleRecordingEnabledKey()
{
    return QStringLiteral("title_recording_enabled");
}
inline QString timelineMergeGapKey()
{
    return QStringLiteral("timeline_merge_gap_secs");
}
inline QString idleTimeoutKey()
{
    return QStringLiteral("idle_timeout_secs");
}
inline QString appOverrideKeyPrefix()
{
    return QStringLiteral("__app_override::");
}
// 读侧最短可见时长（秒）。引擎不按它裁剪，只是历史页 / 时间轴的显示过滤。
// 默认 30：300 秒会把正常的短时使用一起藏掉。
inline QString minSessionSecsKey()
{
    return QStringLiteral("min_session_secs");
}

// 按应用覆盖的 JSON 值：{"track":bool,"captureTitle":bool}，两者都可缺省。
struct AppOverride
{
    std::optional<bool> track;
    std::optional<bool> captureTitle;
};

// 解析 `__app_override::<exe>` 的 value。解析失败 / 键不存在 → nullopt。
std::optional<AppOverride> parseAppOverride(const QString& rawValue);

// 引擎侧设置读取缓存。
//
// 三个 TTL 独立 5 秒；读失败沿用上次缓存，再不行才用默认值。
// 设置页写库后，下一帧 TTL 过期自动生效，本类无需主动失效。
class TrackingSettingsCache
{
public:
    explicit TrackingSettingsCache(engine::SessionStore& store);

    // continuity 窗口（来自 timeline_merge_gap_secs，默认 180s）。
    qint64 continuityWindowMs(qint64 nowMs);
    // sustained 窗口（来自 idle_timeout_secs，默认 900s）。
    qint64 sustainedWindowMs(qint64 nowMs);
    // AFK 阈值（与 continuity 窗口共用 timeline_merge_gap_secs 这个键）。
    qint64 afkThresholdMs(qint64 nowMs);

    // 追踪暂停。读失败沿用缓存；无缓存默认「不暂停」，
    // 但启动时的首帧读失败会把暂停当 true（安全暂停，见 TrackingService）。
    bool trackingPaused(qint64 nowMs);
    void setTrackingPausedHint(bool paused); // setPaused 接口直接写缓存

    // 标题录制总开关。默认 true。
    bool titleRecordingEnabled(qint64 nowMs);
    void setTitleRecordingHint(bool enabled); // 同 setTrackingPausedHint：写侧直接生效

    // 按应用的 captureTitle 覆盖。默认 true（允许记录）。
    bool captureTitleForApp(const QString& exeName, qint64 nowMs);

    // 按应用的匿名开关。默认 false（正常记录）。
    bool anonymousForApp(const QString& exeName, qint64 nowMs);

private:
    struct CachedBool
    {
        bool value = false;
        qint64 loadedAtMs = 0;
        bool valid = false;
    };
    struct CachedSeconds
    {
        qint64 value = 0;
        qint64 loadedAtMs = 0;
        bool valid = false;
    };

    // 内部读取：缓存 TTL 内直返；过期读库；读失败沿用缓存；再不行用默认。
    bool readBoolWithCache(CachedBool& cache,
                           const QString& key,
                           bool defaultValue,
                           qint64 nowMs,
                           bool failClosed);
    qint64 readSecondsWithCache(CachedSeconds& cache,
                                const QString& key,
                                qint64 defaultSeconds,
                                qint64 nowMs);
    std::optional<AppOverride> readAppOverride(const QString& exeName, qint64 nowMs);

    engine::SessionStore& _store;

    CachedSeconds _continuity;
    CachedSeconds _sustained;
    CachedBool _trackingPaused;
    CachedBool _titleRecording;
    CachedBool _trackingPausedHint;
    CachedBool _titleRecordingHint;
    bool _pausedHintValid = false;
    bool _titleHintValid = false;
};

} // namespace patina::engine

#endif // PATINA_ENGINE_TRACKING_SETTINGS_H
