#ifndef PATINA_ENGINE_SESSION_TRACKER_H
#define PATINA_ENGINE_SESSION_TRACKER_H

#include "data/types.h"
#include "engine/continuity.h"
#include "engine/session_store.h"
#include "engine/sustained_identity.h"
#include "engine/sustained_participation.h"
#include "engine/tracking_policy.h"
#include "engine/tracking_settings.h"
#include "engine/window_sample.h"

#include <optional>

namespace patina::engine
{

// 一次采样的处理结果，用于观测与测试，不参与落库。
enum class SampleOutcome
{
    Ignored,      // 采样与前一次没有语义变化（同一应用同一标题）
    SessionStarted,
    SessionTransition, // 旧段闭合 + 新段开启
    SessionEnded,      // 只闭合，没有开新段（AFK、锁屏、切到不可跟踪窗口）
    TitleRefreshed,    // 同一应用换了窗口实例或标题，只刷新标题
    SealedStale,       // 采样停顿/时钟回拨，按最后一次成功采样封口
    SealedSustained,   // 5.1/§7.5：被动参与到期，封在「最后输入 + 宽限窗口」
    AnonymousObserved, // 5.9：匿名应用采样，时间落在匿名表
};

const char* describeOutcome(SampleOutcome outcome);

// 锁屏/睡眠事件（来自 platform/ 的 WM_WTSSESSION_CHANGE 与 WM_POWERBROADCAST）。
enum class PowerEvent
{
    Lock,
    Unlock,
    Suspend,
    Resume,
};

struct StartupReport
{
    bool sealedOpenSession = false; // 崩溃/断电留下的悬挂段被封闭了
    qint64 sealedEndMs = 0;         // 封口用的结束时间
    bool hadLastSample = false;     // 库里有上次成功采样的时间戳
    bool normalizedClock = false;   // 时间戳不合理（超出段范围）而退回段起点
};

// session 生命周期的边界语义全在这里：
//
//   AFK 不计入活动：idleMs 超过阈值即不可跟踪，段落结束在「用户最后一次输入」
//         的时刻，而不是发现 AFK 的时刻。
//   锁屏/睡眠唤醒不串段：电源事件立即封口；错过的电源事件由采样停顿
//         （stallSealAfterMs）兜底，按最后一次成功采样封口，中间空白不并入。
//   异常退出封口：recoverAtStartup() 用库里的最后采样时间戳封口悬挂段。
//   长时间离开后回前台开新段：AFK 封口后运行时状态清空，回来自动新开段，
//         不续接旧段；短时切走再切回复用组起点。
//   低交互不误判：媒体/音频信号在时用 sustainedParticipationMs 的宽限阈值
//         代替 AFK 阈值（状态机驱动）；参与到期后按「最后一次输入 +
//         sustainedParticipationMs」封口，不是按最后一次输入封口。
//   事实与派生状态同事务：一次采样的所有写入落在一个事务里。
//
// 追踪暂停：暂停时对活动段封口，暂停期间不产生新段；读失败安全暂停。
// 探测降级：probeOk=false 时引擎仍接收缓存帧（fromCachedWindow=true），
//     但不刷新「最后成功采样」，恢复后自动回到正常采样。
// 标题隐私线：总开关 + 按应用覆盖关掉时，标题写空、样本闭合。
// 匿名活动：开启匿名统计的应用不落 sessions，走 observe/seal。
//
// 线程：与 Database 一样，只能在创建它的线程使用，内部不加锁。
class SessionTracker
{
public:
    SessionTracker(SessionStore& store, TrackingPolicy policy = TrackingPolicy{});
    // 带设置缓存的构造：引擎参数与按应用覆盖都从 settings 表读。
    // cache 指针由调用方持有并保证生命周期覆盖 tracker。
    SessionTracker(SessionStore& store, TrackingSettingsCache* settingsCache,
                   TrackingPolicy policy = TrackingPolicy{});
    ~SessionTracker();
    SessionTracker(const SessionTracker&) = delete;
    SessionTracker& operator=(const SessionTracker&) = delete;

    // 启动时调用一次：封闭上次异常退出留下的悬挂段。
    // 采样时间戳落在段范围之外时退回段起点，不把段拉长到 now——心跳只证明
    // 进程活着，不证明前台在活动。
    data::Result<StartupReport> recoverAtStartup(qint64 nowMs);

    // 锁屏/挂起立即封口；解锁/恢复只清空运行状态，让下一样本新开段。
    data::Status handlePowerEvent(PowerEvent event, qint64 nowMs);

    // 每秒调用一次。sample.sampledAtMs 必须由调用方填好。
    // sustainedMedia / sustainedAudio 是 platform 探针的当前快照，由
    // 调用方从 platform 单例拷进来（engine 不 include platform）。
    data::Result<SampleOutcome> handleSample(const WindowSample& sample,
                                             const SustainedSignal& sustainedMedia = {},
                                             const SustainedSignal& sustainedAudio = {});

    // 丢弃运行时状态（不停库）。策略变更或环境不可信时用。
    void resetRuntimeState();

    const TrackingPolicy& policy() const { return _policy; }
    void setPolicy(const TrackingPolicy& policy);

    // 最近一次成功落库的采样时刻（内存态，异常退出靠库里的时间戳恢复）。
    std::optional<qint64> lastSuccessfulSampleMs() const { return _lastSuccessfulSampleMs; }

    // 暂停/恢复。暂停时封口当前段；恢复后由下一个采样新开段。
    // 返回是否有封口动作发生（供 TrackingService 通知 UI）。
    data::Result<bool> setPaused(bool paused, qint64 nowMs);
    bool isPaused() const { return _paused; }

    // 启动恢复路径。从库里读到暂停键时直接同步标记位，不封口
    // （启动时也没有段可封）。恢复路径由 setPaused(false) 处理。
    void applyPausedHint(bool paused);

    // 看门狗封口入口。与 sealOpenSession 相同，但暴露成公有——
    // 看门狗线程通过 TrackingService 的回调走这里，回调内部持锁。
    // 看门狗用：封口未闭合段 / 匿名段。调用方负责加锁与复核时间戳。
    data::Result<bool> sealOpenSessionPublic(qint64 endTimeMs);
    data::Status sealAnonymousPublic(qint64 cutoffMs);

private:
    // 过渡计划，纯函数，便于单独核对语义。
    struct TransitionPlan
    {
        bool endPrevious = false;
        bool startNext = false;
        bool refreshMetadata = false;
        std::optional<qint64> endTimeOverride;
        SampleOutcome outcome = SampleOutcome::Ignored;
        // 开新段时复用的组起点。startNext 为真时有效。
        qint64 continuityGroupStartTimeMs = 0;
    };

    TransitionPlan planTransition(const WindowSample& next, qint64 nowMs);

    // 被动参与到期封口点。参与态由 active 转 Expired 时，段落结束在
    // 「最后一次输入 + sustainedParticipationMs」，而不是最后一次输入时刻——
    // 否则一段 20 分钟的视频会被算成 0 时长。
    // 返回 std::nullopt 表示不该由这条路径封口。
    std::optional<qint64> resolveSustainedSealEndMs(const WindowSample& next,
                                                    const SustainedStatus& status,
                                                    qint64 nowMs) const;

    // 在调用方给的事务里应用计划。
    data::Status applyPlan(const TransitionPlan& plan, const WindowSample& next, qint64 nowMs);

    // 封口当前段，返回是否真的闭合了（没有未闭合段时返回 false，不算错误）。
    data::Result<bool> sealOpenSession(qint64 endTimeMs);

    // 收口未闭合的匿名段。匿名段与具名段互斥，进出匿名态、暂停、
    // 锁屏/挂起、启动恢复时都要把开着的那条闭合，否则它会永远挂着，
    // 而匿名时长只统计已闭合段。
    data::Status sealOpenAnonymous(qint64 cutoffMs);

    // 开新段前先封口残留的未闭合段，保证不会撞上单活跃段唯一索引。
    data::Result<bool> startSession(const WindowSample& sample, qint64 nowMs,
                                    qint64 continuityGroupStartTimeMs);

    // 每 timestampPersistIntervalMs 落一次心跳与最后成功采样时间。
    // 只处理有效采样——无效帧在 handleSample 入口就被丢弃了。
    data::Status persistTimestamps(qint64 nowMs);

    // 这一帧是否到了该落时间戳的时候（用来跳过纯观察帧的事务）。
    bool shouldPersistTimestamps(qint64 nowMs) const;

    SessionStore& _store;
    TrackingPolicy _policy;

    // 设置读取缓存。不拥有，由调用方持有。
    TrackingSettingsCache* _settings = nullptr;

    std::optional<WindowSample> _previousSample;
    std::optional<qint64> _lastSuccessfulSampleMs;

    // 时间戳节流：避免每秒写一次 settings 表。
    std::optional<qint64> _lastHeartbeatPersistedMs;
    std::optional<qint64> _lastSamplePersistedMs;

    // sustained participation 运行态。
    SustainedRuntimeState _sustainedState;

    // 上一帧的参与态。到期封口要比较「上一帧参与中、这一帧已到期」，
    // 而 _sustainedState 在算出新状态后就被覆盖了。
    bool _lastSustainedActive = false;

    std::optional<PendingContinuity> _pendingContinuity;

    // 当前是否处在匿名态（上一帧落的是匿名段）。离开匿名态时要把
    // 开着的那条匿名段收口，否则它会永远挂着。
    bool _anonymousActive = false;

    bool _paused = false;
};

} // namespace patina::engine

#endif // PATINA_ENGINE_SESSION_TRACKER_H
