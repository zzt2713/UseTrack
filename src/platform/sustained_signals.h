#ifndef PATINA_PLATFORM_SUSTAINED_SIGNALS_H
#define PATINA_PLATFORM_SUSTAINED_SIGNALS_H

#include <QString>

namespace patina::platform
{

// 媒体/音频信号源的探测层。
//
// 两路信号源，各自带一个专属 std::thread，每 10 秒 reconcile 一次、
// 单次查询超时 2 秒。探测线程把快照写进
// 内部互斥锁保护的槽位；GUI 线程每秒采样时直接读最新快照，不做回调。
//
// WinRT（SMTC）在这一层调用——Win32/Win32-API 收口在
// platform/，engine/ 只消费这份中立快照。

enum class SustainedSourceKind
{
    None,
    SystemMedia,
    AudioSession
};

struct SustainedSignalSnapshot
{
    // false = 这一路探测不可用（超时/失败/未初始化），与「可用但没在播」区分。
    bool available = false;
    bool active = false;
    SustainedSourceKind source = SustainedSourceKind::None;
    // 媒体：AUMID；音频：真实进程 exe 名。
    QString sourceAppId;
    // 音频会话能拿到真实进程的 exe 路径；媒体会话拿不到。
    QString exeName;
    QString exePath;
    // 这份快照的产生时刻。快照超过 kSnapshotTtlMs 没被刷新就当作不可用——
    // 探测线程卡住时，绝不能拿一份过期的「正在播放」继续算参与时长。
    qint64 generatedAtMs = 0;
};

// 快照有效期。
// reconcile 间隔 10 秒，留 15 秒余量：单次探测超时（2 秒）不会误判过期。
constexpr qint64 kSnapshotTtlMs = 15'000;

// 初始化两路信号源并启动探测线程。重复调用是空操作。
// 探测线程在进程退出前由 stopSustainedSignals() 收口。
void startSustainedSignals();

void stopSustainedSignals();

// 读最新快照。两路独立：systemMedia.available / audioSession.available
// 分别表示各自探测是否成功。线程安全（内部有互斥锁）。
// 超过 kSnapshotTtlMs 未刷新的快照按 available=false 返回：探测线程卡住时
// 不能让一份过期的「正在播放」继续撑着参与时长。
struct SustainedSignalsSnapshot
{
    SustainedSignalSnapshot systemMedia;
    SustainedSignalSnapshot audioSession;
};
SustainedSignalsSnapshot currentSustainedSignals();

} // namespace patina::platform

#endif // PATINA_PLATFORM_SUSTAINED_SIGNALS_H
