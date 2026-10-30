#ifndef PATINA_ENGINE_SUSTAINED_PARTICIPATION_H
#define PATINA_ENGINE_SUSTAINED_PARTICIPATION_H

#include "engine/sustained_identity.h"
#include "engine/window_sample.h"

#include <QString>

#include <optional>
#include <utility>

namespace patina::engine
{

// 状态机参数：
//   连续 3 次采样丢失信号 → 进入宽限窗口；宽限 12 秒内仍算参与；
//   宽限期满仍无信号 → Candidate（暂停），再超 sustained 窗口 → Expired。
constexpr int kSustainedTransientMissThreshold = 3;
constexpr qint64 kSustainedGraceWindowMs = 12'000;

enum class SustainedState
{
    Inactive,   // 从未匹配过，或已彻底失效
    Active,     // 信号匹配，正在参与
    Grace,      // 信号短暂丢失，宽限期内仍算参与
    Candidate,  // 宽限过期，疑似暂停
    Expired     // 超出 sustained 窗口，确认停止
};

enum class SustainedKind
{
    None,
    Audio // 目前只产出 Audio 一种 kind
};

enum class SustainedReason
{
    None,
    SignalMatched,          // 有信号且匹配
    GraceWindow,            // 信号丢失但还在宽限期
    GraceExpired,           // 宽限期满
    SignalInactive,         // 信号明确停止（用户暂停）
    IdentityMismatch,       // 有信号但不属于当前窗口
    SustainedWindowExpired, // 超出 sustained 窗口
    TrackingPaused,         // 追踪暂停
    EmptyWindow,            // 窗口为空
    NoSignal                // 无可用信号源
};

struct SustainedRuntimeState
{
    QString identityKey;
    std::optional<qint64> lastMatchAtMs;
    std::optional<qint64> graceDeadlineMs;
    SustainedSignalSource lastSignalSource = SustainedSignalSource::None;
    SustainedKind lastKind = SustainedKind::None;
    int transientMissCount = 0;
};

struct SustainedStatus
{
    bool isTrackingActive = false; // continuity_active || sustained_active
    bool eligible = false;
    bool active = false;
    SustainedState state = SustainedState::Inactive;
    SustainedKind kind = SustainedKind::None;
    SustainedSignalSource source = SustainedSignalSource::None;
    SustainedReason reason = SustainedReason::None;
};

struct SustainedInput
{
    QString exeName;
    QString exePath;
    qint64 idleMs = 0;
    bool isAfk = false;
    qint64 continuityWindowMs = 180'000;
    qint64 sustainedWindowMs = 900'000;
    bool trackingPaused = false;
    qint64 nowMs = 0;
    SustainedSignal systemMedia;
    SustainedSignal audioSession;
};

// 核心状态机。返回 (状态, 新的运行态)。
// 分支顺序不能随意调换：后面的分支依赖前面分支先判掉的情况。
std::pair<SustainedStatus, SustainedRuntimeState>
resolveSustainedParticipation(const SustainedInput& input,
                              const SustainedRuntimeState& previous);

// 参与中 → 不算 AFK。这是「视频放 5 分钟不封段」的机制落点。
void applySustainedToWindow(WindowSample& window, const SustainedStatus& status);

const char* describeSustainedState(SustainedState state);
const char* describeSustainedReason(SustainedReason reason);

} // namespace patina::engine

#endif // PATINA_ENGINE_SUSTAINED_PARTICIPATION_H