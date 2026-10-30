#ifndef PATINA_PLATFORM_FOREGROUND_WINDOW_H
#define PATINA_PLATFORM_FOREGROUND_WINDOW_H

#include <QString>
#include <QtGlobal>

namespace patina::platform
{

// 一次前台窗口采样的快照。
// 空快照（isEmpty() 为真）表示此刻没有可采样的前台应用：锁屏界面上、
// 窗口最小化/隐藏、桌面和任务栏这类外壳表面、或者进程信息取不到。
struct ForegroundWindow
{
    // 用 quintptr 而不是 HWND，避免 windows.h 漏到 platform/ 之外。
    quintptr hwnd = 0;
    quintptr rootOwnerHwnd = 0;
    quint32 processId = 0;
    QString windowClass;
    QString title;
    QString exeName;
    QString exePath;
    // UWP 投影后的包标识：普通 Win32 应用为空。
    QString appUserModelId;

    bool isEmpty() const { return hwnd == 0; }

    // 判定两次采样是否还是同一个前台对象；用于识别“切窗口”。
    bool hasSameIdentity(const ForegroundWindow& other) const;
};

// 采集当前前台窗口。纯同步调用，不抛异常，任何一步失败都返回空快照。
// 采样线程约定：与 icon 缓存等平台状态同线程调用（不内部加锁）。
ForegroundWindow sampleForegroundWindow();

} // namespace patina::platform

#endif // PATINA_PLATFORM_FOREGROUND_WINDOW_H
