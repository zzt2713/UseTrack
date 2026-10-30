#ifndef PATINA_ENGINE_WATCHDOG_H
#define PATINA_ENGINE_WATCHDOG_H

#include <QAtomicInteger>
#include <QDateTime>
#include <QString>

#include <atomic>
#include <functional>
#include <optional>

namespace patina::engine
{

// 独立看门狗。
//
// 主采样在 GUI 事件循环里，循环卡死就没人封口。看门狗在独立线程 1 秒轮询：
// 超过 8 秒没有成功采样 → 把未闭合段按「最后成功采样」时刻封掉。
//
// 三个健康时间戳，供诊断面板读取：
//   heartbeat —— 每次采样循环进入时记录（卡死时停止推进）；
//   successful_sample —— 采样成功且数据落库后记录；
//   watchdog_seal —— 看门狗封口成功后记录，防双封。
class RuntimeHealthState
{
public:
    void noteHeartbeat(qint64 timestampMs);
    void noteSuccessfulSample(qint64 timestampMs);
    void noteWatchdogSeal(qint64 timestampMs);

    std::optional<qint64> lastHeartbeatMs() const;
    std::optional<qint64> lastSuccessfulSampleMs() const;
    std::optional<qint64> lastWatchdogSealMs() const;

private:
    static std::optional<qint64> load(const QAtomicInteger<qint64>& field);

    // 原子字段：主线程写，看门狗线程读。0 表示尚未记录。
    QAtomicInteger<qint64> _lastHeartbeatMs{0};
    QAtomicInteger<qint64> _lastSuccessfulSampleMs{0};
    QAtomicInteger<qint64> _lastWatchdogSealMs{0};
};

// 封口判定：无成功采样 → false；已封过同一时刻 → false（防双封）；
// 否则 now - last > 8 秒。
bool shouldWatchdogSeal(const RuntimeHealthState& health, qint64 nowMs);

// 看门狗线程入口。seal 回调由 TrackingService 注入，内部持锁保证与
// 主采样互斥（同一把 mutex）；回调内先复核 last_successful_sample_ms
// 是否仍等于封口目标，不等就放弃——排队的封口不能盖掉新采样。
void runWatchdogLoop(const std::atomic_bool& running,
                     RuntimeHealthState& health,
                     const std::function<void(qint64)>& sealStaleSession);

const char* describeWatchdogSealReason();

} // namespace patina::engine

#endif // PATINA_ENGINE_WATCHDOG_H
