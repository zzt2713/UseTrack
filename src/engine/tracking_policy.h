#ifndef PATINA_ENGINE_TRACKING_POLICY_H
#define PATINA_ENGINE_TRACKING_POLICY_H

#include <QtGlobal>

namespace patina::engine
{

// 引擎的时间参数。默认值不是凭感觉定的，都是实测过的常用值。
//
//   采样间隔        1s
//   AFK 阈值        180s
//   低交互宽限      900s
//   采样停顿封口    8s
struct TrackingPolicy
{
    qint64 sampleIntervalMs = 1000;

    // 距最后一次用户输入超过这个值即判 AFK，该段时间不计入活动。
    qint64 afkThresholdMs = 180 * 1000;

    // 低交互场景（视频、会议）的宽限窗口：媒体/音频信号在时用它代替 AFK 阈值，
    // 避免看视频被误判为离开。
    qint64 sustainedParticipationMs = 900 * 1000;

    // 两次成功采样间隔超过它，说明进程被挂起/系统睡眠过。
    // 此时不能把中间这段空白串进活动段，必须封口重开。
    qint64 stallSealAfterMs = 8000;

    // 采样时间戳落库的频率。异常退出封口要靠它，但每秒写一次没必要。
    qint64 timestampPersistIntervalMs = 3000;
};

} // namespace patina::engine

#endif // PATINA_ENGINE_TRACKING_POLICY_H
