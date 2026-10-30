#include "platform/power_watcher.h"

#include "platform/win_handle.h"

// WTSRegisterSessionNotification / WM_WTSSESSION_CHANGE / WTS_SESSION_LOCK
#include <wtsapi32.h>

namespace patina::platform
{

// 桥接：把匿名命名空间里的窗口过程接到 PowerWatcher 的私有成员上。
// 头文件里只需要一个前向声明，HWND / WNDPROC 细节不外泄。
struct PowerWatcherAccess
{
    // 注意不能叫 emit：Qt 把它定义成空宏。
    static void dispatch(PowerWatcher& watcher, PowerSignal signal)
    {
        watcher.emitSignal(signal);
    }
};

namespace
{

constexpr wchar_t kPowerWatchClassName[] = L"UseTrackPowerWatcherWindow";

// 类注册成功后复用；注销一个还有窗口在用的类没意义。
bool g_windowClassRegistered = false;

// 同一进程只允许一个监听窗口，窗口过程靠它找到实例。
PowerWatcher* g_activeWatcher = nullptr;

void emitForWindow(HWND hwnd, PowerSignal signal)
{
    PowerWatcher* watcher = reinterpret_cast<PowerWatcher*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (watcher == nullptr) {
        watcher = g_activeWatcher;
    }
    if (watcher != nullptr) {
        PowerWatcherAccess::dispatch(*watcher, signal);
    }
}

LRESULT CALLBACK powerWatcherProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_LOCK) {
            emitForWindow(hwnd, PowerSignal::Lock);
            return 0;
        }
        if (wParam == WTS_SESSION_UNLOCK) {
            emitForWindow(hwnd, PowerSignal::Unlock);
            return 0;
        }
        break;

    case WM_POWERBROADCAST:
        if (wParam == PBT_APMSUSPEND) {
            emitForWindow(hwnd, PowerSignal::Suspend);
            // 返回 TRUE：已处理，不再交给 DefWindowProc。
            return TRUE;
        }
        if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            emitForWindow(hwnd, PowerSignal::Resume);
            return TRUE;
        }
        break;

    default:
        break;
    }

    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

bool ensureWindowClass(HINSTANCE instance, QString* errorOut)
{
    if (g_windowClassRegistered) {
        return true;
    }

    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = powerWatcherProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kPowerWatchClassName;

    ::SetLastError(ERROR_SUCCESS);
    if (::RegisterClassW(&windowClass) == 0) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            if (errorOut != nullptr) {
                *errorOut = QStringLiteral("注册电源监听窗口类失败（错误码 %1）")
                                .arg(static_cast<qulonglong>(error));
            }
            return false;
        }
    }

    g_windowClassRegistered = true;
    return true;
}

} // namespace

PowerWatcher::~PowerWatcher()
{
    stop();
}

bool PowerWatcher::start(Handler handler, QString* errorOut)
{
    if (_hwnd != 0) {
        stop();
    }

    if (!handler) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("未提供事件处理函数");
        }
        return false;
    }

    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    if (instance == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("取模块句柄失败");
        }
        return false;
    }

    if (!ensureWindowClass(instance, errorOut)) {
        return false;
    }

    // 关键：顶层窗口（parent = nullptr），但不设 WS_VISIBLE。
    // 电源广播只发给顶层窗口，message-only 窗口收不到。
    const HWND hwnd = ::CreateWindowExW(0,
                                        kPowerWatchClassName,
                                        L"UseTrack Power Watcher",
                                        0, // 无 WS_VISIBLE / WS_OVERLAPPED 等任何样式
                                        0, 0, 0, 0,
                                        nullptr,
                                        nullptr,
                                        instance,
                                        nullptr);
    if (hwnd == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("创建电源监听窗口失败（错误码 %1）")
                            .arg(static_cast<qulonglong>(::GetLastError()));
        }
        return false;
    }

    _handler = std::move(handler);
    // 用窗口的 user data 挂着实例指针，窗口过程据此回调。
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    g_activeWatcher = this;
    _hwnd = reinterpret_cast<quintptr>(hwnd);

    if (!::WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION)) {
        // 注册失败就没法判断锁屏，属于致命配置问题：拆掉窗口，避免留下
        // 只能收到电源广播的半残监听。
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("注册会话通知失败（错误码 %1）")
                            .arg(static_cast<qulonglong>(::GetLastError()));
        }
        stop();
        return false;
    }

    return true;
}

void PowerWatcher::stop()
{
    if (_hwnd == 0) {
        return;
    }

    HWND hwnd = reinterpret_cast<HWND>(_hwnd);
    ::WTSUnRegisterSessionNotification(hwnd);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    ::DestroyWindow(hwnd);

    _hwnd = 0;
    _handler = nullptr;
    if (g_activeWatcher == this) {
        g_activeWatcher = nullptr;
    }
}

void PowerWatcher::emitSignal(PowerSignal signal)
{
    if (_handler) {
        _handler(signal);
    }
}

} // namespace patina::platform
