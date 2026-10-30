#include "app/single_instance.h"

#include <QMetaObject>

#include <windows.h>

#include <atomic>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

namespace patina::app
{
namespace
{

// 命名管道名带用户名后缀：同一用户才能互通，多用户会话（快速用户切换）不互踩。
std::wstring pipeName()
{
    wchar_t userName[256] = {};
    DWORD size = static_cast<DWORD>(std::size(userName));
    if (!::GetUserNameW(userName, &size)) {
        // 取不到用户名时退回固定后缀：宁可多用户共享一个管道，也不能把名字
        // 拼成空的导致管道名撞车或非法。
        return L"\\\\.\\pipe\\usetrack-single-instance-default";
    }
    // 成功时 size 含结尾 NUL，要减掉，否则管道名里会夹一个 \0。
    const size_t length = size > 0 ? static_cast<size_t>(size - 1) : 0;
    return L"\\\\.\\pipe\\usetrack-single-instance-" + std::wstring(userName, length);
}

HANDLE g_pipe = INVALID_HANDLE_VALUE;
SingleInstanceLock::ActivateHandler g_handler;
std::atomic_bool g_listenRunning{false};
std::unique_ptr<std::thread> g_listenThread;

// 等一次重叠 I/O 完成，同时给停止信号留出响应窗口。
// running 变 false 时用 CancelIoEx 踢掉这次 I/O，并且必须等它真正落地
// （事件被置位）才返回——否则 OVERLAPPED 还挂在内核里引用栈地址，
// 下一轮循环在同一栈位置重建就是悬垂。
bool waitForIo(HANDLE pipe, HANDLE event, const std::atomic_bool& running)
{
    while (running.load(std::memory_order_relaxed)) {
        const DWORD wait = ::WaitForSingleObject(event, 200);
        if (wait == WAIT_OBJECT_0) {
            return true;
        }
        if (wait == WAIT_FAILED) {
            return false;
        }
    }
    ::CancelIoEx(pipe, nullptr);
    ::WaitForSingleObject(event, 5000);
    return false;
}

// 监听线程：循环等第二实例连接，收到就断开并回调激活。
// 管道以 FILE_FLAG_OVERLAPPED 创建，ConnectNamedPipe/ReadFile 都走重叠 I/O，
// 停止信号才能把它们从阻塞里踢出来——同步管道上的 overlapped 参数会被忽略，
// CancelIoEx 也就无从生效。
void listenLoop(HANDLE pipe, std::atomic_bool& running)
{
    const HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        return;
    }

    while (running.load(std::memory_order_relaxed)) {
        ::ResetEvent(event);
        OVERLAPPED connectOverlapped = {};
        connectOverlapped.hEvent = event;

        bool connected = false;
        if (::ConnectNamedPipe(pipe, &connectOverlapped)) {
            connected = true;
        } else {
            const DWORD error = ::GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                // 客户端在我们调 Connect 之前就接上了，也算成功。
                connected = true;
            } else if (error == ERROR_IO_PENDING) {
                connected = waitForIo(pipe, event, running);
            }
            // 其他错误（管道已关/句柄失效）：退出线程。
        }

        if (!connected || !running.load(std::memory_order_relaxed)) {
            break;
        }

        // 读一条消息（内容不关心，给对端一个完整的请求-响应周期）。
        ::ResetEvent(event);
        OVERLAPPED readOverlapped = {};
        readOverlapped.hEvent = event;
        char buffer[64] = {};
        DWORD bytesRead = 0;
        bool readDone = false;
        if (::ReadFile(pipe, buffer, sizeof(buffer) - 1, &bytesRead, &readOverlapped)) {
            readDone = true;
        } else if (::GetLastError() == ERROR_IO_PENDING) {
            if (waitForIo(pipe, event, running)) {
                readDone = ::GetOverlappedResult(pipe, &readOverlapped, &bytesRead, FALSE) != FALSE;
            }
        }
        ::DisconnectNamedPipe(pipe);

        // 只有真读到内容才激活：没连上、或被取消的等待都不该弹窗口。
        // handler 由 acquire() 填入，内部走 QMetaObject::invokeMethod 切 GUI 线程。
        if (readDone && bytesRead > 0 && g_handler) {
            g_handler();
        }
    }

    ::CloseHandle(event);
}

enum class ProbeResult
{
    NoInstance,   // 管道不存在：本进程可以成为唯一实例
    InstanceFound,// 已有实例，且激活消息已送达
    InstanceUnreachable, // 已有实例，但消息没送到（管道忙/权限）
};

ProbeResult probeExistingInstance(const std::wstring& name)
{
    // ERROR_PIPE_BUSY 说明实例在，但监听线程正在服务别的连接：等它空出来重试。
    for (int attempt = 0; attempt < 3; ++attempt) {
        HANDLE client = ::CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, 0, nullptr);
        if (client != INVALID_HANDLE_VALUE) {
            static constexpr char kMessage[] = "activate\n";
            DWORD written = 0;
            const bool sent = ::WriteFile(client, kMessage, sizeof(kMessage) - 1, &written,
                                          nullptr)
                != FALSE;
            ::FlushFileBuffers(client);
            ::CloseHandle(client);
            return sent ? ProbeResult::InstanceFound : ProbeResult::InstanceUnreachable;
        }
        if (::GetLastError() != ERROR_PIPE_BUSY) {
            return ProbeResult::NoInstance;
        }
        ::WaitNamedPipeW(name.c_str(), 500);
    }
    return ProbeResult::InstanceUnreachable;
}

} // namespace

bool SingleInstanceLock::acquire(const ActivateHandler& handler, QString* errorOut)
{
    const std::wstring name = pipeName();

    // 先试连：连得上说明已有实例，通知它激活后本进程退出。
    switch (probeExistingInstance(name)) {
    case ProbeResult::InstanceFound:
        return false;
    case ProbeResult::InstanceUnreachable:
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("已有实例在运行，但激活通知未能送达");
        }
        return false;
    case ProbeResult::NoInstance:
        break;
    }

    // 没有实例：创建服务端管道。FILE_FLAG_FIRST_PIPE_INSTANCE 保证并发启动时
    // 只有一个进程能建成功，另一个会在 CreateNamedPipeW 上失败。
    HANDLE server = ::CreateNamedPipeW(
        name.c_str(),
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,          // maxInstances：同一时间只服务一个第二实例
        64, 64,     // in/out buffer
        0,          // default timeout
        nullptr);
    if (server == INVALID_HANDLE_VALUE) {
        // 单实例是尽力而为：管道建不起来（权限、别的进程占了同名管道）不该
        // 拦住应用启动，降级成「允许并存」并留一条警告。
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("单实例管道创建失败（错误码 %1），已跳过单实例保护")
                            .arg(static_cast<qulonglong>(::GetLastError()));
        }
        return true;
    }

    g_pipe = server;
    g_handler = handler;

    // 独立监听线程，但不是 detached：release() 会置 running=false + CancelIoEx，
    // 线程从重叠 I/O 里退出来后 join 收口。
    g_listenRunning.store(true);
    g_listenThread = std::make_unique<std::thread>(listenLoop, server, std::ref(g_listenRunning));
    return true;
}

void SingleInstanceLock::release()
{
    // 先置 false，让监听线程从重叠 I/O 里退出来。
    g_listenRunning.store(false);
    if (g_pipe != INVALID_HANDLE_VALUE) {
        // CancelIoEx 把可能阻塞在 ConnectNamedPipe/ReadFile 上的 I/O 踢出来。
        ::CancelIoEx(g_pipe, nullptr);
        ::CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
    if (g_listenThread && g_listenThread->joinable()) {
        g_listenThread->join();
        g_listenThread.reset();
    }
    g_handler = nullptr;
}

} // namespace patina::app
