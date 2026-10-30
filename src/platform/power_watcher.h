#ifndef PATINA_PLATFORM_POWER_WATCHER_H
#define PATINA_PLATFORM_POWER_WATCHER_H

#include <QString>

#include <functional>

namespace patina::platform
{

// 电源/会话生命周期信号。
//
// 与 engine::PowerEvent 刻意分开：platform/ 不 include engine/，
// 两者的映射在 app/ 的接缝处完成。
enum class PowerSignal
{
    Lock,    // 会话锁定（WM_WTSSESSION_CHANGE + WTS_SESSION_LOCK）
    Unlock,  // 会话解锁
    Suspend, // 系统挂起（WM_POWERBROADCAST + PBT_APMSUSPEND）
    Resume,  // 从挂起恢复
};

struct PowerWatcherAccess;

// 锁屏/挂起事件源。
//
// 实现方式：创建一个**没有 WS_VISIBLE 的顶层窗口**接收广播。不能用
// message-only 窗口——电源广播只发给顶层窗口，不发给 message-only 窗口。
//
// 窗口建在 Qt 事件循环所在的线程上，Qt 已经替我们泵这个线程的窗口消息，
// 所以不自建线程与循环。
//
// 线程：只能在有事件循环的线程（GUI 线程）创建与销毁；回调也在该线程触发，
// 与 SessionTracker 同线程，不需要跨线程编排。
//
// 单实例：窗口类名固定，同一进程只允许一个处于运行状态的实例。
class PowerWatcher
{
public:
    using Handler = std::function<void(PowerSignal)>;

    PowerWatcher() = default;
    ~PowerWatcher();

    PowerWatcher(const PowerWatcher&) = delete;
    PowerWatcher& operator=(const PowerWatcher&) = delete;

    // 创建监听窗口并注册会话通知。失败返回 false，原因写进 errorOut。
    bool start(Handler handler, QString* errorOut = nullptr);

    // 注销通知并销毁监听窗口。未启动时是空操作。
    void stop();

    bool isRunning() const { return _hwnd != 0; }

private:
    friend struct PowerWatcherAccess;

    // 把信号交给上层。窗口过程通过 PowerWatcherAccess 调到这里。
    void emitSignal(PowerSignal signal);

    quintptr _hwnd = 0;
    Handler _handler;
};

} // namespace patina::platform

#endif // PATINA_PLATFORM_POWER_WATCHER_H
