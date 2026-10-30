#include "engine/continuity.h"

#include "engine/process_filter.h"

namespace patina::engine
{
namespace
{

bool shouldOpenPendingContinuity(const WindowSample* previous,
                                 const WindowSample& current)
{
    if (previous == nullptr || !isTrackableSample(*previous) || current.afk) {
        return false;
    }
    const WindowIdentity previousIdentity = identityFromSample(*previous);
    if (!previousIdentity.isValid()) {
        return false;
    }
    // 当前不可跟踪也算「切走了」：壳窗口一闪而过不应切断连续性。
    if (!isTrackableSample(current)) {
        return true;
    }
    return !previousIdentity.isSameApp(identityFromSample(current));
}

} // namespace

std::optional<PendingContinuity> loadPendingContinuity(const WindowSample* previous,
                                                       const WindowSample& current,
                                                       qint64 continuityWindowMs,
                                                       qint64 nowMs)
{
    if (!shouldOpenPendingContinuity(previous, current)) {
        return std::nullopt;
    }

    const WindowIdentity identity = identityFromSample(*previous);
    PendingContinuity pending;
    pending.appKey = identity.appKey;
    // 组起点取上一段的组起点：如果上段自己就是新组，那它等于上段起点。
    // WindowSample 没有 startTimeMs，用 continuityGroupStartTimeMs（0 = 新组）
    // 或 sampledAtMs 兜底——上一帧就是这个段的最后一次采样。
    pending.continuityGroupStartTimeMs =
        previous->continuityGroupStartTimeMs != 0
            ? previous->continuityGroupStartTimeMs
            : previous->sampledAtMs;
    pending.expiresAtMs = nowMs + continuityWindowMs;
    return pending;
}

qint64 resolveContinuityGroupStartTime(const std::optional<PendingContinuity>& pending,
                                       const WindowSample& current,
                                       qint64 nowMs)
{
    if (!pending.has_value()) {
        return nowMs;
    }
    if (pending->expiresAtMs < nowMs) {
        return nowMs;
    }
    if (!isTrackableSample(current)) {
        return nowMs;
    }
    const WindowIdentity identity = identityFromSample(current);
    if (identity.appKey != pending->appKey) {
        return nowMs;
    }
    return pending->continuityGroupStartTimeMs;
}

std::optional<PendingContinuity> resolveNextPendingContinuity(
    std::optional<PendingContinuity> existing,
    std::optional<PendingContinuity> next,
    qint64 continuityGroupStartTimeMs,
    const WindowSample& current,
    qint64 nowMs)
{
    if (next.has_value()) {
        return next;
    }
    if (!existing.has_value()) {
        return std::nullopt;
    }
    if (existing->expiresAtMs < nowMs) {
        return std::nullopt;
    }
    // 当前帧已经消费掉挂起（组起点相同 + 同一个应用）→ 清掉，防重复消费。
    if (isTrackableSample(current)) {
        const WindowIdentity identity = identityFromSample(current);
        if (identity.appKey == existing->appKey
            && continuityGroupStartTimeMs == existing->continuityGroupStartTimeMs) {
            return std::nullopt;
        }
    }
    return existing;
}

} // namespace patina::engine
