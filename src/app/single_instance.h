#ifndef PATINA_APP_SINGLE_INSTANCE_H
#define PATINA_APP_SINGLE_INSTANCE_H

#include <functional>

#include <QString>

namespace patina::app
{

// 单实例锁。
//
// 用命名管道（裸 Win32，`\\.\pipe\usetrack-single-instance-<用户名>`）：
// 第二个实例连上已存在的管道写一条激活消息，第一个实例的主窗口被唤到前台，
// 然后自己退出。QSharedMemory 跨权限场景下遗留锁文件的问题更多，管道关进程
// 就干净。
class SingleInstanceLock
{
public:
    using ActivateHandler = std::function<void()>;

    // 尝试成为唯一实例。
    //   - 成功 → 返回 true，本进程持有锁；handler 在后续有第二实例到来时
    //     被调用（在监听线程上调用，由 handler 自己切回 GUI 线程）。
    //   - 已有实例 → 通知它激活并返回 false，调用方应当立即退出。
    //   - 管道建不起来 → 返回 true 并填 errorOut，降级为不做单实例保护。
    static bool acquire(const ActivateHandler& handler, QString* errorOut = nullptr);

    // 释放锁（进程退出时也会自动释放，显式调用是为了测试）。
    static void release();

private:
    SingleInstanceLock() = delete;
};

} // namespace patina::app

#endif // PATINA_APP_SINGLE_INSTANCE_H
