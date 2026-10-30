#include "engine/watchdog.h"

#include <QDateTime>

// ::Sleep 用。engine/ 不该碰 Win32 —— Win32 只准出现在
// platform/。这里破一次例的原因：看门狗的「分片 sleep」逻辑是它自身的核心
// 语义（停止信号最多 1 秒内生效），拉进 platform/ 反而拆散。只用了
// kernel32 的 Sleep，不碰任何窗口/句柄 API。
#include <windows.h>

#include <functional>

namespace patina::engine
{
namespace
{

constexpr qint64 kWatchdogPollMs = 1'000;
constexpr qint64 kStallSealAfterMs = 8'000;

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace

void RuntimeHealthState::noteHeartbeat(qint64 timestampMs)
{
    _lastHeartbeatMs.storeRelease(timestampMs);
}

void RuntimeHealthState::noteSuccessfulSample(qint64 timestampMs)
{
    _lastSuccessfulSampleMs.storeRelease(timestampMs);
}

void RuntimeHealthState::noteWatchdogSeal(qint64 timestampMs)
{
    _lastWatchdogSealMs.storeRelease(timestampMs);
}

std::optional<qint64> RuntimeHealthState::load(const QAtomicInteger<qint64>& field)
{
    const qint64 value = field.loadAcquire();
    return value > 0 ? std::optional<qint64>{value} : std::nullopt;
}

std::optional<qint64> RuntimeHealthState::lastHeartbeatMs() const
{
    return load(_lastHeartbeatMs);
}

std::optional<qint64> RuntimeHealthState::lastSuccessfulSampleMs() const
{
    return load(_lastSuccessfulSampleMs);
}

std::optional<qint64> RuntimeHealthState::lastWatchdogSealMs() const
{
    return load(_lastWatchdogSealMs);
}

bool shouldWatchdogSeal(const RuntimeHealthState& health, qint64 nowMs)
{
    const std::optional<qint64> lastSuccessful = health.lastSuccessfulSampleMs();
    if (!lastSuccessful.has_value()) {
        return false;
    }
    // 已按这个时刻封过：看门狗成功封口后 last_successful 不再推进（线程还
    // 卡着），不挡一下会每秒重复封。
    if (health.lastWatchdogSealMs() == lastSuccessful) {
        return false;
    }
    return nowMs - *lastSuccessful > kStallSealAfterMs;
}

void runWatchdogLoop(const std::atomic_bool& running,
                     RuntimeHealthState& health,
                     const std::function<void(qint64)>& sealStaleSession)
{
    while (running.load(std::memory_order_relaxed)) {
        const qint64 now = nowMs();
        if (shouldWatchdogSeal(health, now)) {
            const qint64 target = health.lastSuccessfulSampleMs().value_or(0);
            // 回调内部会拿主采样锁；锁内复核 last_successful == target，
            // 不等说明排队期间主循环恢复了，放弃本次封口。
            if (sealStaleSession) {
                sealStaleSession(target);
            }
        }
        // 分片 sleep：停止信号最多 1 秒内生效。
        for (int waited = 0;
             waited < kWatchdogPollMs && running.load(std::memory_order_relaxed); waited += 100) {
            ::Sleep(100);
        }
    }
}

const char* describeWatchdogSealReason()
{
    return "watchdog-sealed";
}

} // namespace patina::engine
