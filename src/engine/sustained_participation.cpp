#include "engine/sustained_participation.h"

namespace patina::engine
{
namespace
{

enum class SignalResult
{
    Unavailable,     // 信号源不可用
    Matched,         // 正在播放且属于当前窗口
    IdentityMismatch, // 在播放，但不是当前窗口
    Inactive         // 源可用，当前没在播
};

SignalResult evaluateSignal(const SustainedSignal& signal,
                            const QString& exeName,
                            const QString& exePath)
{
    if (!signal.available) {
        return SignalResult::Unavailable;
    }
    if (!signal.active) {
        return SignalResult::Inactive;
    }
    return signalOriginMatchesWindow(signal, exeName, exePath) ? SignalResult::Matched
                                                               : SignalResult::IdentityMismatch;
}

SustainedReason resolveCandidateReason(const SustainedSignal& media,
                                       const SustainedSignal& audio,
                                       const QString& exeName,
                                       const QString& exePath)
{
    // 媒体优先：它的「已暂停」语义最明确。
    // 局部变量不能叫 signals —— Qt 把 signals 定义成 public 的宏。
    const SustainedSignal sources[2] = {media, audio};
    for (const SustainedSignal& signal : sources) {
        switch (evaluateSignal(signal, exeName, exePath)) {
        case SignalResult::Matched:
            return SustainedReason::SignalMatched;
        case SignalResult::IdentityMismatch:
            return SustainedReason::IdentityMismatch;
        case SignalResult::Inactive:
            return SustainedReason::SignalInactive;
        case SignalResult::Unavailable:
            break;
        }
    }
    return SustainedReason::NoSignal;
}

SustainedReason resolveInactiveReason(const SustainedSignal& media,
                                      const SustainedSignal& audio,
                                      const QString& exeName,
                                      const QString& exePath)
{
    const SustainedSignal sources[2] = {media, audio};
    for (const SustainedSignal& signal : sources) {
        if (signal.available
            && !signalOriginMatchesWindow(signal, exeName, exePath)) {
            return SustainedReason::IdentityMismatch;
        }
    }
    return SustainedReason::NoSignal;
}

SustainedRuntimeState defaultRuntimeState()
{
    return SustainedRuntimeState{};
}

} // namespace

std::pair<SustainedStatus, SustainedRuntimeState>
resolveSustainedParticipation(const SustainedInput& input,
                              const SustainedRuntimeState& previous)
{
    SustainedStatus status;
    const bool withinSustainedWindow = input.idleMs <= input.sustainedWindowMs;
    const bool continuityActive =
        !input.isAfk && input.idleMs <= input.continuityWindowMs;

    const QString identityKey = resolveSustainedIdentityKey(input.exeName, input.exePath);
    const bool sameIdentity = !identityKey.isEmpty() && previous.identityKey == identityKey;

    // 1. 追踪暂停：一切归零。
    if (input.trackingPaused) {
        status.isTrackingActive = continuityActive;
        status.state = SustainedState::Inactive;
        status.reason = SustainedReason::TrackingPaused;
        return {status, defaultRuntimeState()};
    }

    // 2. 空窗口。
    if (input.exeName.trimmed().isEmpty()) {
        status.isTrackingActive = continuityActive;
        status.state = SustainedState::Inactive;
        status.reason = SustainedReason::EmptyWindow;
        return {status, defaultRuntimeState()};
    }

    // 3. 有匹配信号：媒体优先，音频兜底。
    SustainedSignal matched;
    bool hasMatched = false;
    if (signalMatchesWindow(input.systemMedia, input.exeName, input.exePath)) {
        matched = input.systemMedia;
        hasMatched = true;
    } else if (signalMatchesWindow(input.audioSession, input.exeName, input.exePath)) {
        matched = input.audioSession;
        hasMatched = true;
    }

    if (hasMatched) {
        SustainedRuntimeState next = previous;
        next.identityKey = identityKey;
        next.lastMatchAtMs = input.nowMs;
        next.graceDeadlineMs = std::nullopt;
        next.lastSignalSource = matched.source;
        next.lastKind = SustainedKind::Audio;
        next.transientMissCount = 0;

        status.eligible = true;
        if (withinSustainedWindow) {
            status.active = true;
            status.state = SustainedState::Active;
            status.kind = SustainedKind::Audio;
            status.source = matched.source;
            status.reason = SustainedReason::SignalMatched;
        } else {
            status.state = SustainedState::Expired;
            status.reason = SustainedReason::SustainedWindowExpired;
        }
        status.isTrackingActive = continuityActive || status.active;
        return {status, next};
    }

    // 4. 曾经匹配、现在超窗：彻底过期。
    if (sameIdentity && previous.lastMatchAtMs.has_value() && !withinSustainedWindow) {
        SustainedRuntimeState next = previous;
        next.graceDeadlineMs = std::nullopt;
        status.state = SustainedState::Expired;
        status.reason = SustainedReason::SustainedWindowExpired;
        status.isTrackingActive = continuityActive;
        return {status, next};
    }

    // 5. 曾经匹配、信号明确停止（用户按了暂停）：跳过宽限。
    if (sameIdentity && previous.lastMatchAtMs.has_value()
        && signalExplicitlyStopped(input.systemMedia, input.exeName, input.exePath)) {
        SustainedRuntimeState next = previous;
        next.graceDeadlineMs = std::nullopt;
        next.transientMissCount = 0;
        status.state = SustainedState::Candidate;
        status.reason = SustainedReason::SignalInactive;
        status.isTrackingActive = continuityActive;
        return {status, next};
    }

    // 6. 曾经匹配、信号丢失（没有明确停止）：走宽限。
    if (sameIdentity && previous.lastMatchAtMs.has_value()) {
        const int nextMiss = previous.transientMissCount + 1;
        SustainedRuntimeState next = previous;
        next.transientMissCount = nextMiss;

        if (nextMiss >= kSustainedTransientMissThreshold && !next.graceDeadlineMs.has_value()) {
            next.graceDeadlineMs = input.nowMs + kSustainedGraceWindowMs;
        }
        const bool stillInGrace =
            next.graceDeadlineMs.has_value() && *next.graceDeadlineMs >= input.nowMs;

        status.eligible = true;
        if (nextMiss < kSustainedTransientMissThreshold || stillInGrace) {
            // 短暂丢失不算结束：保持参与，AFK 封口继续被压住。
            status.active = true;
            status.state = SustainedState::Grace;
            status.kind = previous.lastKind;
            status.source = previous.lastSignalSource;
            status.reason = SustainedReason::GraceWindow;
        } else {
            next.graceDeadlineMs = std::nullopt;
            status.state = SustainedState::Candidate;
            status.reason = nextMiss >= kSustainedTransientMissThreshold
                                ? SustainedReason::GraceExpired
                                : resolveCandidateReason(input.systemMedia, input.audioSession,
                                                         input.exeName, input.exePath);
        }
        status.isTrackingActive = continuityActive || status.active;
        return {status, next};
    }

    // 7. 从未匹配，但至少有一路信号源可用：候选（可能马上匹配上）。
    if (evaluateSignal(input.systemMedia, input.exeName, input.exePath) != SignalResult::Unavailable
        || evaluateSignal(input.audioSession, input.exeName, input.exePath) != SignalResult::Unavailable) {
        status.state = SustainedState::Candidate;
        status.reason =
            resolveCandidateReason(input.systemMedia, input.audioSession, input.exeName, input.exePath);
        status.isTrackingActive = continuityActive;
        return {status, defaultRuntimeState()};
    }

    // 8. 无任何信号源。
    status.state = SustainedState::Inactive;
    status.reason = resolveInactiveReason(input.systemMedia, input.audioSession,
                                          input.exeName, input.exePath);
    status.isTrackingActive = continuityActive;
    return {status, defaultRuntimeState()};
}

void applySustainedToWindow(WindowSample& window, const SustainedStatus& status)
{
    if (status.active) {
        window.sustainedParticipation = true;
        window.afk = false;
    }
}

const char* describeSustainedState(SustainedState state)
{
    switch (state) {
    case SustainedState::Inactive:
        return "inactive";
    case SustainedState::Active:
        return "active";
    case SustainedState::Grace:
        return "grace";
    case SustainedState::Candidate:
        return "candidate";
    case SustainedState::Expired:
        return "expired";
    }
    return "unknown";
}

const char* describeSustainedReason(SustainedReason reason)
{
    switch (reason) {
    case SustainedReason::None:
        return "none";
    case SustainedReason::SignalMatched:
        return "signal-matched";
    case SustainedReason::GraceWindow:
        return "grace-window";
    case SustainedReason::GraceExpired:
        return "grace-expired";
    case SustainedReason::SignalInactive:
        return "signal-inactive";
    case SustainedReason::IdentityMismatch:
        return "identity-mismatch";
    case SustainedReason::SustainedWindowExpired:
        return "sustained-window-expired";
    case SustainedReason::TrackingPaused:
        return "tracking-paused";
    case SustainedReason::EmptyWindow:
        return "empty-window";
    case SustainedReason::NoSignal:
        return "no-signal";
    }
    return "unknown";
}

} // namespace patina::engine