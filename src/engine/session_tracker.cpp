#include "engine/session_tracker.h"

#include "engine/process_filter.h"
#include "engine/window_identity.h"

#include <algorithm>
#include <memory>
#include <optional>

namespace patina::engine
{
namespace
{

// AFK 阈值：接 settings 后由 timeline_merge_gap 驱动；没接缓存时用 policy 默认。
qint64 effectiveAfkThresholdMs(const TrackingPolicy& policy, TrackingSettingsCache* settings,
                               qint64 nowMs)
{
    return settings != nullptr ? settings->afkThresholdMs(nowMs) : policy.afkThresholdMs;
}

qint64 effectiveSustainedWindowMs(const TrackingPolicy& policy, TrackingSettingsCache* settings,
                                  qint64 nowMs)
{
    return settings != nullptr ? settings->sustainedWindowMs(nowMs)
                               : policy.sustainedParticipationMs;
}

qint64 effectiveContinuityWindowMs(const TrackingPolicy& policy, TrackingSettingsCache* settings,
                                   qint64 nowMs)
{
    return settings != nullptr ? settings->continuityWindowMs(nowMs)
                               : policy.afkThresholdMs;
}

bool trackingPausedNow(TrackingSettingsCache* settings, qint64 nowMs)
{
    return settings != nullptr && settings->trackingPaused(nowMs);
}

bool titleRecordingEnabledNow(TrackingSettingsCache* settings, qint64 nowMs)
{
    return settings != nullptr && settings->titleRecordingEnabled(nowMs);
}

bool captureTitleForAppNow(TrackingSettingsCache* settings, const QString& exeName, qint64 nowMs)
{
    if (settings == nullptr) {
        return true;
    }
    return settings->captureTitleForApp(exeName, nowMs);
}

bool anonymousForAppNow(TrackingSettingsCache* settings, const QString& exeName, qint64 nowMs)
{
    if (settings == nullptr) {
        return false;
    }
    return settings->anonymousForApp(exeName, nowMs);
}

// 应用最终该记录的标题：总开关 + 按应用覆盖都允许时才返回原样，否则空串。
QString titleForPersistence(TrackingSettingsCache* settings, const WindowSample& sample,
                            qint64 nowMs)
{
    if (!titleRecordingEnabledNow(settings, nowMs)) {
        return QString();
    }
    if (!captureTitleForAppNow(settings, sample.exeName, nowMs)) {
        return QString();
    }
    return sample.title;
}

// 心跳只证明进程活着，不证明前台在活动：时间戳必须落在段范围之内才采信，
// 否则退回段起点。
qint64 resolveInterruptedEndMs(qint64 sessionStartMs,
                               std::optional<qint64> lastSampleMs,
                               qint64 nowMs)
{
    if (lastSampleMs.has_value() && *lastSampleMs >= sessionStartMs && *lastSampleMs <= nowMs) {
        return *lastSampleMs;
    }
    return sessionStartMs;
}

// 被动参与到期后，段落的真实结束点是「最后一次输入 + 宽限窗口」。
// 最后一次输入 = nowMs - idleMs。
qint64 sustainedSealEndMs(qint64 nowMs, qint64 idleMs, qint64 sustainedWindowMs)
{
    return nowMs - idleMs + sustainedWindowMs;
}

} // namespace

const char* describeOutcome(SampleOutcome outcome)
{
    switch (outcome) {
    case SampleOutcome::Ignored:
        return "ignored";
    case SampleOutcome::SessionStarted:
        return "session-started";
    case SampleOutcome::SessionTransition:
        return "session-transition";
    case SampleOutcome::SessionEnded:
        return "session-ended";
    case SampleOutcome::TitleRefreshed:
        return "session-metadata-refreshed";
    case SampleOutcome::SealedStale:
        return "session-ended-stale-sample";
    case SampleOutcome::SealedSustained:
        return "passive-participation-sealed";
    case SampleOutcome::AnonymousObserved:
        return "anonymous-observed";
    }
    return "unknown";
}

SessionTracker::SessionTracker(SessionStore& store, TrackingPolicy policy)
    : SessionTracker(store, nullptr, policy)
{
}

SessionTracker::SessionTracker(SessionStore& store, TrackingSettingsCache* settingsCache,
                               TrackingPolicy policy)
    : _store(store)
    , _policy(policy)
    , _settings(settingsCache)
{
}

SessionTracker::~SessionTracker() = default;

void SessionTracker::setPolicy(const TrackingPolicy& policy)
{
    _policy = policy;
}

void SessionTracker::resetRuntimeState()
{
    _previousSample.reset();
    _lastSuccessfulSampleMs.reset();
    _sustainedState = SustainedRuntimeState{};
    _lastSustainedActive = false;
    _anonymousActive = false;
    _pendingContinuity.reset();
}

data::Result<StartupReport> SessionTracker::recoverAtStartup(qint64 nowMs)
{
    if (nowMs <= 0) {
        return data::fail<StartupReport>(QStringLiteral("启动时间非法"));
    }

    StartupReport report;

    // 匿名段也要收口。它和具名段是两条独立的时间线，具名段没有
    // 未闭合不代表匿名段没有——所以放在「有没有未闭合具名段」判断之前。
    const data::Status anonymousSealed = _store.sealInterruptedAnonymousActivity(nowMs);
    if (!anonymousSealed.ok()) {
        return data::fail<StartupReport>(anonymousSealed.error);
    }

    const data::Result<std::optional<OpenSession>> open = _store.loadOpenSession();
    if (!open.ok()) {
        return data::fail<StartupReport>(open.error);
    }
    if (!open.value.has_value()) {
        return data::succeed(report);
    }

    const data::Result<std::optional<qint64>> lastSample =
        _store.loadTimestamp(lastSuccessfulSampleKey());
    if (!lastSample.ok()) {
        return data::fail<StartupReport>(lastSample.error);
    }

    const qint64 startMs = open.value->startTimeMs;
    const qint64 endMs = resolveInterruptedEndMs(startMs, lastSample.value, nowMs);
    report.hadLastSample = lastSample.value.has_value();
    report.normalizedClock = lastSample.value.has_value() && endMs != *lastSample.value;
    report.sealedEndMs = endMs;

    ScopedStoreTransaction transaction(_store);
    if (!transaction.isActive()) {
        return data::fail<StartupReport>(QStringLiteral("无法开启事务"));
    }

    const data::Result<bool> sealed = _store.endOpenSession(endMs);
    if (!sealed.ok()) {
        return data::fail<StartupReport>(sealed.error);
    }
    const data::Status committed = transaction.commit();
    if (!committed.ok()) {
        return data::fail<StartupReport>(committed.error);
    }
    report.sealedOpenSession = sealed.value;

    // 运行态清空：崩溃前那个进程的采样不能当作本次的「前一样本」，
    // 否则会把上一次的窗口状态误判成刚发生的切换。
    resetRuntimeState();
    return data::succeed(report);
}

data::Status SessionTracker::handlePowerEvent(PowerEvent event, qint64 nowMs)
{
    if (nowMs <= 0) {
        return data::Status::failure(QStringLiteral("事件时间非法"));
    }

    switch (event) {
    case PowerEvent::Lock:
    case PowerEvent::Suspend: {
        // 锁屏/挂起是确定的离开边界，事件到达即封口，不等下一次采样。
        // 若等采样，中间那次采样还可能把离开时长算进活动段。
        ScopedStoreTransaction transaction(_store);
        if (!transaction.isActive()) {
            return data::Status::failure(QStringLiteral("无法开启事务"));
        }
        const data::Result<bool> sealed = sealOpenSession(nowMs);
        if (!sealed.ok()) {
            return data::Status::failure(sealed.error);
        }
        // 匿名段同样在离开边界收口，否则睡眠/锁屏那段会挂在匿名段里。
        const data::Status anonymousSealed = sealOpenAnonymous(nowMs);
        if (!anonymousSealed.ok()) {
            return anonymousSealed;
        }
        const data::Status committed = transaction.commit();
        if (!committed.ok()) {
            return committed;
        }
        resetRuntimeState();
        return data::Status::success();
    }
    case PowerEvent::Unlock:
    case PowerEvent::Resume:
        // 解锁/唤醒本身不写库：下一样本若是同一应用，自然会开新段；
        // 若还在锁屏或不可跟踪窗口，则什么都不发生。
        resetRuntimeState();
        return data::Status::success();
    }
    return data::Status::success();
}

data::Result<bool> SessionTracker::setPaused(bool paused, qint64 nowMs)
{
    bool didSeal = false;
    if (paused && !_paused) {
        // 进入暂停时立即封口当前段，封口时刻就是 now。
        ScopedStoreTransaction transaction(_store);
        if (!transaction.isActive()) {
            return data::fail<bool>(QStringLiteral("无法开启事务"));
        }
        const data::Result<bool> sealed = sealOpenSession(nowMs);
        if (!sealed.ok()) {
            return data::fail<bool>(sealed.error);
        }
        // 匿名段同样封在暂停时刻——暂停期间不该有任何时间入账。
        const data::Status anonymousSealed = sealOpenAnonymous(nowMs);
        if (!anonymousSealed.ok()) {
            return data::fail<bool>(anonymousSealed.error);
        }
        const data::Status committed = transaction.commit();
        if (!committed.ok()) {
            return data::fail<bool>(committed.error);
        }
        didSeal = sealed.value;
    }
    _paused = paused;
    if (_settings != nullptr) {
        _settings->setTrackingPausedHint(paused);
    }
    if (paused) {
        resetRuntimeState();
    }
    return data::succeed(didSeal);
}

void SessionTracker::applyPausedHint(bool paused)
{
    _paused = paused;
    if (_settings != nullptr) {
        _settings->setTrackingPausedHint(paused);
    }
}

data::Result<SampleOutcome> SessionTracker::handleSample(const WindowSample& rawSample,
                                                         const SustainedSignal& sustainedMedia,
                                                         const SustainedSignal& sustainedAudio)
{
    const qint64 nowMs = rawSample.sampledAtMs;

    // 探测降级：失败帧（probeOk=false）整帧丢弃，不改段、不刷新「最后成功
    // 采样」时间戳——失败的一帧不能延长任何活动区间。缓存帧（成功但来自降级
    // 状态）不走这条：引擎继续消费它，只是不把时间戳推进。
    if (!rawSample.isUsable()) {
        return data::fail<SampleOutcome>(QStringLiteral("采样无效：probeOk=false 或缺少采样时间"));
    }

    WindowSample sample = rawSample;

    // 暂停期间不产生任何新段，也不观察匿名活动。
    if (_paused || trackingPausedNow(_settings, nowMs)) {
        _paused = true;
        return data::succeed(SampleOutcome::Ignored);
    }

    // 兜底：错过的锁屏/挂起广播、或系统睡眠，表现为采样停顿。
    // 此时按最后一次成功采样封口，中间的空白不并入活动段。
    // 降级帧（fromCachedWindow）不推进时间戳，但也不算停顿——引擎
    // 依旧收到内容，只是不落时间戳。
    // 必须排在匿名分支之前：睡眠期间前台是匿名应用时，这一帧不能把整段
    // 睡眠写进 observed_until，否则匿名时长会把空白一起吃进去。
    if (!sample.fromCachedWindow && _lastSuccessfulSampleMs.has_value()
        && nowMs - *_lastSuccessfulSampleMs > _policy.stallSealAfterMs) {
        const qint64 sealAtMs = *_lastSuccessfulSampleMs;

        ScopedStoreTransaction transaction(_store);
        if (!transaction.isActive()) {
            return data::fail<SampleOutcome>(QStringLiteral("无法开启事务"));
        }
        const data::Result<bool> sealed = sealOpenSession(sealAtMs);
        if (!sealed.ok()) {
            return data::fail<SampleOutcome>(sealed.error);
        }
        // 睡眠/错过广播期间匿名段也一样要收口，否则它会把整段空白吃进去。
        const data::Status anonymousSealed = sealOpenAnonymous(sealAtMs);
        if (!anonymousSealed.ok()) {
            return data::fail<SampleOutcome>(anonymousSealed.error);
        }
        const data::Status committed = transaction.commit();
        if (!committed.ok()) {
            return data::fail<SampleOutcome>(committed.error);
        }

        resetRuntimeState();
        // 停顿后的第一帧不作为新段的起点：它可能是唤醒瞬间的过渡态。
        // 下一帧（1 秒后）会用干净状态重新判定并开段。
        return data::succeed(SampleOutcome::SealedStale);
    }

    // 时钟回拨：时间戳倒退不可能产生新的活动区间。
    // 丢弃这一帧，同时清空运行态——否则那个「未来的」时间戳会让后续每一帧
    // 都被判成回拨，直到墙钟追上为止，整段时间都不再记录。
    if (_lastSuccessfulSampleMs.has_value() && nowMs < *_lastSuccessfulSampleMs) {
        // 开着的匿名段也要收口：seal 取 MAX(observed_until, cutoff)，封口点落在
        // 最后一次真实观察到的时刻，不会被回拨的墙钟拉短。不收口的话它永远
        // 挂着，而匿名时长只统计已闭合段。
        if (_anonymousActive) {
            ScopedStoreTransaction transaction(_store);
            if (transaction.isActive()) {
                const data::Status sealed = sealOpenAnonymous(nowMs);
                if (!sealed.ok()) {
                    return data::fail<SampleOutcome>(sealed.error);
                }
                const data::Status committed = transaction.commit();
                if (!committed.ok()) {
                    return data::fail<SampleOutcome>(committed.error);
                }
            }
        }
        resetRuntimeState();
        return data::succeed(SampleOutcome::SealedStale);
    }

    // 匿名应用的时间不落 sessions，只落匿名表。observe 幂等，时钟回拨
    // 不写库（匿名仓储内部处理）。
    // 进入匿名态前先收掉具名段：匿名与具名互斥，同一段时间不能两边都记。
    const bool anonymousNow =
        anonymousForAppNow(_settings, sample.exeName, nowMs) && isTrackableSample(sample);
    if (anonymousNow) {
        ScopedStoreTransaction transaction(_store);
        if (!transaction.isActive()) {
            return data::fail<SampleOutcome>(QStringLiteral("无法开启事务"));
        }
        const data::Result<bool> namedSealed = sealOpenSession(nowMs);
        if (!namedSealed.ok()) {
            return data::fail<SampleOutcome>(namedSealed.error);
        }
        const data::Result<bool> observed = _store.observeAnonymousActivity(nowMs);
        if (!observed.ok()) {
            return data::fail<SampleOutcome>(observed.error);
        }
        if (!sample.fromCachedWindow) {
            const data::Status persisted = persistTimestamps(nowMs);
            if (!persisted.ok()) {
                return data::fail<SampleOutcome>(persisted.error);
            }
        }
        const data::Status committed = transaction.commit();
        if (!committed.ok()) {
            return data::fail<SampleOutcome>(committed.error);
        }

        _previousSample = sample;
        if (!sample.fromCachedWindow) {
            _lastSuccessfulSampleMs = nowMs;
        }
        // 匿名帧不参与 sustained 状态机，上一帧的参与态作废。
        _lastSustainedActive = false;
        _anonymousActive = true;
        _pendingContinuity.reset();
        return data::succeed(SampleOutcome::AnonymousObserved);
    }

    // 离开匿名态（切到具名应用、或匿名开关被关掉）。把开着的匿名段
    // 封在 nowMs——它和具名段互斥，不收口就会永远挂着，匿名时长恒为 0。
    // 只在切换的这一帧多开一个事务，之后每帧都走下面的常规路径。
    if (_anonymousActive) {
        ScopedStoreTransaction transaction(_store);
        if (!transaction.isActive()) {
            return data::fail<SampleOutcome>(QStringLiteral("无法开启事务"));
        }
        const data::Status sealed = sealOpenAnonymous(nowMs);
        if (!sealed.ok()) {
            return data::fail<SampleOutcome>(sealed.error);
        }
        const data::Status committed = transaction.commit();
        if (!committed.ok()) {
            return data::fail<SampleOutcome>(committed.error);
        }
        _anonymousActive = false;
    }

    // sustained participation 状态机在算 AFK 之前跑——参与中会直接把
    // AFK 位压掉，避免看 20 分钟视频被切得粉碎。
    SustainedInput sustainedInput;
    sustainedInput.exeName = sample.exeName;
    sustainedInput.exePath = sample.exePath;
    sustainedInput.idleMs = sample.idleMs;
    sustainedInput.isAfk = sample.idleMs > effectiveAfkThresholdMs(_policy, _settings, nowMs);
    sustainedInput.continuityWindowMs = effectiveContinuityWindowMs(_policy, _settings, nowMs);
    sustainedInput.sustainedWindowMs = effectiveSustainedWindowMs(_policy, _settings, nowMs);
    sustainedInput.trackingPaused = false; // 已在上面分支拦住
    sustainedInput.nowMs = nowMs;
    sustainedInput.systemMedia = sustainedMedia;
    sustainedInput.audioSession = sustainedAudio;
    const auto [sustainedStatus, sustainedNext] =
        resolveSustainedParticipation(sustainedInput, _sustainedState);
    _sustainedState = sustainedNext;
    applySustainedToWindow(sample, sustainedStatus);

    sample.afk = sample.idleMs > (sample.sustainedParticipation
                                      ? effectiveSustainedWindowMs(_policy, _settings, nowMs)
                                      : effectiveAfkThresholdMs(_policy, _settings, nowMs));

    // 被动参与到期。这条必须在
    // planTransition 之前：AFK 分支会把段落封在「最后一次输入」，而看视频/
    // 开会这段时间本身是被动参与的有效时长，要封在「最后一次输入 + 宽限窗口」。
    const std::optional<qint64> sustainedSealMs =
        resolveSustainedSealEndMs(sample, sustainedStatus, nowMs);
    if (sustainedSealMs.has_value()) {
        ScopedStoreTransaction transaction(_store);
        if (!transaction.isActive()) {
            return data::fail<SampleOutcome>(QStringLiteral("无法开启事务"));
        }
        const data::Result<bool> sealed = sealOpenSession(*sustainedSealMs);
        if (!sealed.ok()) {
            return data::fail<SampleOutcome>(sealed.error);
        }
        const data::Status anonymousSealed = sealOpenAnonymous(*sustainedSealMs);
        if (!anonymousSealed.ok()) {
            return data::fail<SampleOutcome>(anonymousSealed.error);
        }
        if (!sample.fromCachedWindow) {
            const data::Status persisted = persistTimestamps(nowMs);
            if (!persisted.ok()) {
                return data::fail<SampleOutcome>(persisted.error);
            }
        }
        const data::Status committed = transaction.commit();
        if (!committed.ok()) {
            return data::fail<SampleOutcome>(committed.error);
        }

        // 段收掉了，但这一帧的窗口仍是「前一样本」：下一秒还是同一个前台窗口
        // 时走「无变化」分支，不凭空开新段；真正回到交互（afk=false）时才开新段。
        // 挂起的连续性作废——旧段已经结束，短时切回不该再续到它上面。
        _pendingContinuity.reset();
        _previousSample = sample;
        if (!sample.fromCachedWindow) {
            _lastSuccessfulSampleMs = nowMs;
        }
        _lastSustainedActive = sustainedStatus.active;
        return data::succeed(SampleOutcome::SealedSustained);
    }

    _lastSustainedActive = sustainedStatus.active;

    const TransitionPlan plan = planTransition(sample, nowMs);

    // 没有任何要写的东西时不白开事务：一秒一次 BEGIN IMMEDIATE 毫无收益。
    // 但仍要更新内存态，否则「最后成功采样」会停住，进而误判成采样停顿。
    const bool needsWrite = plan.endPrevious || plan.startNext || plan.refreshMetadata
        || shouldPersistTimestamps(nowMs);
    if (!needsWrite) {
        if (!sample.fromCachedWindow) {
            _lastSuccessfulSampleMs = nowMs;
        }
        _previousSample = sample;
        return data::succeed(plan.outcome);
    }

    ScopedStoreTransaction transaction(_store);
    if (!transaction.isActive()) {
        return data::fail<SampleOutcome>(QStringLiteral("无法开启事务"));
    }

    const data::Status applied = applyPlan(plan, sample, nowMs);
    if (!applied.ok()) {
        return data::fail<SampleOutcome>(applied.error);
    }

    // 降级帧不写「最后成功采样」时间戳——缓存窗口不该被当成新的成功
    // 采样延长异常退出封口的依据。
    if (!sample.fromCachedWindow) {
        const data::Status persisted = persistTimestamps(nowMs);
        if (!persisted.ok()) {
            return data::fail<SampleOutcome>(persisted.error);
        }
    }

    // 事实（sessions）与派生状态（时间戳）在同一个事务里提交。
    const data::Status committed = transaction.commit();
    if (!committed.ok()) {
        return data::fail<SampleOutcome>(committed.error);
    }

    if (!sample.fromCachedWindow) {
        _lastSuccessfulSampleMs = nowMs;
    }
    _previousSample = sample;
    return data::succeed(plan.outcome);
}

bool SessionTracker::shouldPersistTimestamps(qint64 nowMs) const
{
    return !_lastHeartbeatPersistedMs.has_value()
        || nowMs - *_lastHeartbeatPersistedMs >= _policy.timestampPersistIntervalMs
        || !_lastSamplePersistedMs.has_value()
        || nowMs - *_lastSamplePersistedMs >= _policy.timestampPersistIntervalMs;
}

std::optional<qint64> SessionTracker::resolveSustainedSealEndMs(const WindowSample& next,
                                                               const SustainedStatus& status,
                                                               qint64 nowMs) const
{
    // 只有「上一帧参与中 → 这一帧到期」才封。
    if (status.state != SustainedState::Expired || !_lastSustainedActive) {
        return std::nullopt;
    }

    // 必须还是同一个应用：切走了就交给 planTransition 的常规切段处理，
    // 不该由被动参与到期这条路径收尾。
    if (!_previousSample.has_value()) {
        return std::nullopt;
    }
    const WindowIdentity previousIdentity = identityFromSample(*_previousSample);
    const WindowIdentity nextIdentity = identityFromSample(next);
    if (!previousIdentity.isValid() || !nextIdentity.isValid()
        || !previousIdentity.isSameApp(nextIdentity)) {
        return std::nullopt;
    }

    return sustainedSealEndMs(nowMs, next.idleMs,
                              effectiveSustainedWindowMs(_policy, _settings, nowMs));
}

SessionTracker::TransitionPlan SessionTracker::planTransition(const WindowSample& next,
                                                              qint64 nowMs)
{
    TransitionPlan plan;

    const bool previousTrackable = _previousSample.has_value() && isTrackableSample(*_previousSample);
    const bool nextTrackable = isTrackableSample(next);

    const WindowIdentity previousIdentity =
        previousTrackable ? identityFromSample(*_previousSample) : WindowIdentity{};
    const WindowIdentity nextIdentity = nextTrackable ? identityFromSample(next) : WindowIdentity{};

    const bool identitiesKnown = previousIdentity.isValid() && nextIdentity.isValid();
    const bool appChanged = identitiesKnown ? !previousIdentity.isSameApp(nextIdentity)
                                            : (previousTrackable != nextTrackable);
    const bool instanceChanged =
        identitiesKnown ? !previousIdentity.isSameInstance(nextIdentity) : false;
    const bool trackingStateChanged = previousTrackable != nextTrackable;
    const bool didChange = appChanged || trackingStateChanged;

    plan.endPrevious = previousTrackable && didChange;
    plan.startNext = nextTrackable && didChange;

    // 标题是否允许记录，决定持久化的标题内容与样本是否开新。
    const QString persistedTitle = titleForPersistence(_settings, next, nowMs);
    const QString previousPersistedTitle =
        _previousSample.has_value() ? titleForPersistence(_settings, *_previousSample, nowMs)
                                    : QString();
    const bool titleChanged = _previousSample.has_value()
                              && previousPersistedTitle != persistedTitle;
    plan.refreshMetadata = !didChange && nextTrackable && (titleChanged || instanceChanged);

    if (plan.endPrevious && !nextTrackable && next.afk) {
        // AFK 段的结束点是用户最后一次输入的时刻，不是发现 AFK 的时刻。
        // 否则人走开那几分钟会被虚记成活动。
        plan.endTimeOverride = nowMs - next.idleMs;
    }

    // 切应用时算组起点。挂起有效且回到同一应用 → 复用；否则本段自成新组。
    if (plan.startNext) {
        plan.continuityGroupStartTimeMs =
            resolveContinuityGroupStartTime(_pendingContinuity, next, nowMs);
    }

    // 切换应用时更新挂起。上一段刚闭合，它的组起点就是挂起要复用的值。
    if (plan.endPrevious && plan.startNext) {
        plan.outcome = SampleOutcome::SessionTransition;
    } else if (plan.endPrevious) {
        plan.outcome = SampleOutcome::SessionEnded;
    } else if (plan.startNext) {
        plan.outcome = SampleOutcome::SessionStarted;
    } else if (plan.refreshMetadata) {
        plan.outcome = SampleOutcome::TitleRefreshed;
    } else {
        plan.outcome = SampleOutcome::Ignored;
    }

    return plan;
}

data::Status SessionTracker::applyPlan(const TransitionPlan& plan,
                                       const WindowSample& next,
                                       qint64 nowMs)
{
    if (plan.endPrevious) {
        // 结束时间由 sealOpenSession 夹在段起点之后：AFK 起点早于开段时刻、或
        // 时钟回拨时，宁可写 0 时长，也不写出 end < start 的坏数据
        //（schema 里也有 CHECK 兜底）。
        const data::Result<bool> sealed = sealOpenSession(plan.endTimeOverride.value_or(nowMs));
        if (!sealed.ok()) {
            return data::Status::failure(sealed.error);
        }
        // 上一段刚闭合。记一条挂起，下次切回同一应用时复用组起点。
        if (_previousSample.has_value()) {
            _pendingContinuity = loadPendingContinuity(&*_previousSample, next,
                                                       effectiveContinuityWindowMs(
                                                           _policy, _settings, nowMs),
                                                       nowMs);
        } else {
            _pendingContinuity.reset();
        }
    } else if (plan.startNext) {
        // 没封上一段就不需要新的挂起（同应用内切换不产生挂起）。
        _pendingContinuity.reset();
    }

    if (plan.startNext) {
        const data::Result<bool> started =
            startSession(next, nowMs, plan.continuityGroupStartTimeMs);
        if (!started.ok()) {
            return data::Status::failure(started.error);
        }
    }

    if (plan.refreshMetadata) {
        // 写持久化标题（可能被隐私策略清空），样本同步切。
        const QString persistedTitle = titleForPersistence(_settings, next, nowMs);
        const data::Result<bool> refreshed =
            _store.refreshOpenTitle(next.exeName, persistedTitle, nowMs);
        if (!refreshed.ok()) {
            return data::Status::failure(refreshed.error);
        }
    }

    // 消费挂起。复用了 → 清掉防重复；超窗/换应用 → 清掉；没消费 → 保留。
    _pendingContinuity = resolveNextPendingContinuity(
        std::move(_pendingContinuity), std::nullopt, plan.continuityGroupStartTimeMs, next, nowMs);

    return data::Status::success();
}

data::Result<bool> SessionTracker::sealOpenSession(qint64 endTimeMs)
{
    const data::Result<std::optional<OpenSession>> open = _store.loadOpenSession();
    if (!open.ok()) {
        return data::fail<bool>(open.error);
    }
    if (!open.value.has_value()) {
        // 没有未闭合段不是错误：调用方只是想把当前段收掉。
        return data::succeed(false);
    }
    const qint64 clampedEndMs = std::max(endTimeMs, open.value->startTimeMs);
    return _store.endOpenSession(clampedEndMs);
}

data::Status SessionTracker::sealOpenAnonymous(qint64 cutoffMs)
{
    return _store.sealAnonymousActivity(cutoffMs);
}

data::Result<bool> SessionTracker::sealOpenSessionPublic(qint64 endTimeMs)
{
    return sealOpenSession(endTimeMs);
}

data::Status SessionTracker::sealAnonymousPublic(qint64 cutoffMs)
{
    return sealOpenAnonymous(cutoffMs);
}

data::Result<bool> SessionTracker::startSession(const WindowSample& sample, qint64 nowMs,
                                                qint64 continuityGroupStartTimeMs)
{
    // 库里若还留着未闭合段（上次异常退出后本进程没走过 recover，或者外部
    // 写入了脏数据），先封口再开新段，否则单活跃段唯一索引会直接拒绝写入。
    const data::Result<bool> sealed = sealOpenSession(nowMs);
    if (!sealed.ok()) {
        return data::fail<bool>(sealed.error);
    }

    SessionStart start;
    start.exeName = sample.exeName;
    start.exePath = sample.exePath;
    // 写进库的标题已经过隐私策略过滤。
    start.windowTitle = titleForPersistence(_settings, sample, nowMs);
    // display_name 由 TrackingService 从 platform 解析填进 sample；
    // 这里从 sample 透传，空值不动库里已有的名字。
    start.displayName = sample.displayName;
    start.startTimeMs = nowMs;
    start.continuityGroupStartTimeMs = continuityGroupStartTimeMs;
    const data::Result<bool> started = _store.startSession(start);
    if (!started.ok()) {
        return started;
    }
    return started;
}

data::Status SessionTracker::persistTimestamps(qint64 nowMs)
{
    const bool needHeartbeat = !_lastHeartbeatPersistedMs.has_value()
        || nowMs - *_lastHeartbeatPersistedMs >= _policy.timestampPersistIntervalMs;
    if (needHeartbeat) {
        const data::Status status = _store.saveTimestamp(lastHeartbeatKey(), nowMs);
        if (!status.ok()) {
            return status;
        }
        _lastHeartbeatPersistedMs = nowMs;
    }

    const bool needSample = !_lastSamplePersistedMs.has_value()
        || nowMs - *_lastSamplePersistedMs >= _policy.timestampPersistIntervalMs;
    if (needSample) {
        // 这个时间戳是异常退出封口的唯一依据；写失败必须上报，
        // 否则下次启动只能把悬挂段封在段起点（时长变成 0）。
        const data::Status status = _store.saveTimestamp(lastSuccessfulSampleKey(), nowMs);
        if (!status.ok()) {
            return status;
        }
        _lastSamplePersistedMs = nowMs;
    }

    return data::Status::success();
}

} // namespace patina::engine
