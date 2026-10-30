#ifndef PATINA_ENGINE_WINDOW_SAMPLE_H
#define PATINA_ENGINE_WINDOW_SAMPLE_H

#include <QString>
#include <QtGlobal>

namespace patina::engine
{

// 一次采样在引擎侧的规范化形态。
//
// 与 platform::ForegroundWindow 分开：engine/ 不接触 Win32，也不该被采样实现
// 的字段变动牵着走；测试需要能构造任意样本（含 AFK、无窗口、低交互）。
struct WindowSample
{
    // 采样本身是否成功。false 表示这次读前台窗口失败（超时、Win32 异常），
    // 这时整帧丢弃：既不改段，也不刷新「最后成功采样」时间戳。
    // 与 valid 区分开——锁屏时采样是成功的，只是没有可跟踪的前台对象。
    bool probeOk = true;

    // false = 此刻没有可采样的前台对象（锁屏、最小化、外壳表面、取不到进程）。
    bool valid = false;
    quintptr hwnd = 0;
    quintptr rootOwnerHwnd = 0;
    quint32 processId = 0;
    QString windowClass;
    QString title;
    QString exeName;
    QString exePath;

    // UWP 应用的 AUMID：ApplicationFrameHost 窗口被投影成真实应用后，
    // 这里落包标识，参与实例身份判定——同一家商店应用换版本包也认得出。
    QString appUserModelId;

    // 跳过探测降级状态：probeOk=false 且缓存窗口时，采样帧携带上次
    // 成功的窗口内容，但引擎不得把它当成新的成功采样刷新时间戳。
    bool fromCachedWindow = false;

    // 版本资源里解析出的显示名。空 = 没解析到，store 不动库里已有值。
    QString displayName;

    // 距最后一次用户输入的毫秒数，来自 GetLastInputInfo（platform/ 提供）。
    qint64 idleMs = 0;

    // 采样时刻，epoch 毫秒。引擎所有时间判定都用它，不自己取时钟，
    // 这样测试可以精确控制时间。
    qint64 sampledAtMs = 0;

    // 连续性组起点：本段自己的组起点。0 表示「新组」。
    // 由 SessionTracker 在开段/复用时写入，采样方不必填。
    qint64 continuityGroupStartTimeMs = 0;

    // 低交互场景（看视频、开会）：媒体/音频信号置位期间不按 AFK 判死，
    // 改用 sustainedParticipationSecs 的宽限窗口。
    bool sustainedParticipation = false;

    // 由 SessionTracker 按 idleMs 与阈值算出，采样方不必填。
    bool afk = false;

    bool isEmpty() const { return !valid || exeName.isEmpty(); }

    // 可用于判断“用户在这一帧是否在用某个应用”。
    bool isUsable() const { return probeOk && sampledAtMs > 0; }
};

} // namespace patina::engine

#endif // PATINA_ENGINE_WINDOW_SAMPLE_H
