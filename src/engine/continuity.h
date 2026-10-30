#ifndef PATINA_ENGINE_CONTINUITY_H
#define PATINA_ENGINE_CONTINUITY_H

#include "engine/window_identity.h"
#include "engine/window_sample.h"

#include <QString>

#include <optional>

namespace patina::engine
{

// 连续性组。
//
// 短暂切走再切回同一应用：切走时记一条「挂起的连续性」，切回来时组起点
// 复用切走前的那个值——几个碎段共享一个 group start，读模型就能把它们
// 拼成「同一段连续使用」。
//
// 窗口：continuity_window_secs（来自 timeline_merge_gap 设置，默认 180s）。
// 超窗开新组；切到不可跟踪窗口时挂起仍然保留（SearchHost 那种一闪而过的
// 壳不该把用户在原应用里的连续使用切断）。
struct PendingContinuity
{
    QString appKey;                    // 切走前那个应用的身份键
    qint64 continuityGroupStartTimeMs = 0; // 切走前那个段的组起点
    qint64 expiresAtMs = 0;            // now + continuityWindow
};

// 切换应用时该不该开一条挂起：
//   - 上一次采样是可跟踪的、当前不是 AFK；
//   - 当前是同一个应用 → 不需要（没切走）；
//   - 当前不可跟踪 → 也开（壳一闪而过不破坏连续性）。
std::optional<PendingContinuity> loadPendingContinuity(const WindowSample* previous,
                                                       const WindowSample& current,
                                                       qint64 continuityWindowMs,
                                                       qint64 nowMs);

// 开新段时该用什么组起点：挂起有效 + 应用相同 → 复用；否则 now。
qint64 resolveContinuityGroupStartTime(const std::optional<PendingContinuity>& pending,
                                       const WindowSample& current,
                                       qint64 nowMs);

// 消费挂起：已复用 / 已过期 → 清掉；否则保留给下一次切换。
// newPending 非 nullopt 表示这一帧自己开了一条新的，直接用它。
std::optional<PendingContinuity> resolveNextPendingContinuity(
    std::optional<PendingContinuity> existing,
    std::optional<PendingContinuity> next,
    qint64 continuityGroupStartTimeMs,
    const WindowSample& current,
    qint64 nowMs);

} // namespace patina::engine

#endif // PATINA_ENGINE_CONTINUITY_H
