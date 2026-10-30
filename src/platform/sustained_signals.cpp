#include "platform/sustained_signals.h"

#include <QDateTime>
#include <QMutex>
#include <QMutexLocker>
#include <QString>

#include <windows.h>

#include <functional>
#include <memory>
#include <atomic>
#include <optional>
#include <thread>

// ---------------------------------------------------------------------------
// WinRT（SMTC）
//
// C++/WinRT 只在 platform/ 的实现文件里出现；SDK 10.0.26100.0 自带
// cppwinrt 投影头 + WindowsApp.lib，不需要额外依赖。
// ---------------------------------------------------------------------------

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

namespace winrt
{
using namespace Windows::Foundation;
using namespace Windows::Media::Control;
}

// ---------------------------------------------------------------------------
// WASAPI（音频会话）
// ---------------------------------------------------------------------------

#include <mmdeviceapi.h>
#include <audiopolicy.h>

namespace patina::platform
{
namespace
{

constexpr int kReconcileIntervalMs = 10'000;
constexpr int kQueryTimeoutMs = 2'000;
constexpr int kLogThrottleMs = 60'000;
constexpr int kAudioSessionLimit = 64;

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

// 60 秒节流，探测失败的日志才不会每 2 秒刷一条。
void logProbeError(const char* tag, const QString& message)
{
    static QMutex mutex;
    static qint64 lastLoggedAtMs[2] = {0, 0};
    const int slot = tag[0] == 'm' ? 0 : 1;

    QMutexLocker locker(&mutex);
    const qint64 now = nowMs();
    if (now - lastLoggedAtMs[slot] < kLogThrottleMs) {
        return;
    }
    lastLoggedAtMs[slot] = now;
    locker.unlock();
    qWarning("[%s] %s", tag, qUtf8Printable(message));
}

// 快照槽位：探测线程写，GUI 线程读。
struct SignalSlot
{
    QMutex mutex;
    SustainedSignalSnapshot snapshot;
};

SignalSlot& mediaSlot()
{
    static SignalSlot slot;
    return slot;
}

SignalSlot& audioSlot()
{
    static SignalSlot slot;
    return slot;
}

void writeSlot(SignalSlot& slot, const SustainedSignalSnapshot& snapshot)
{
    QMutexLocker locker(&slot.mutex);
    slot.snapshot = snapshot;
    slot.snapshot.generatedAtMs = nowMs();
}

SustainedSignalSnapshot readSlot(SignalSlot& slot)
{
    QMutexLocker locker(&slot.mutex);
    return slot.snapshot;
}

// 把探测线程的异常全部吞掉：探测失败只是信号不可用，不能把线程弄死。
template <typename Fn>
void runGuarded(const char* tag, Fn&& fn)
{
    try {
        fn();
    } catch (const std::exception& error) {
        logProbeError(tag, QString::fromUtf8(error.what()));
    } catch (...) {
        logProbeError(tag, QStringLiteral("未知异常"));
    }
}

// ---------------------------------------------------------------------------
// SMTC 探测（媒体会话）
// ---------------------------------------------------------------------------

// 探测线程能否继续跑：stopSustainedSignals 把它置 false，
// 卡在 get() 里的线程靠它跳出。挂在全局是因为 WinRT 回调持有引用。
std::atomic_bool g_mediaQueryAbort{false};

SustainedSignalSnapshot queryMediaSession()
{
    SustainedSignalSnapshot result;
    result.available = true;
    result.source = SustainedSourceKind::SystemMedia;

    // RequestAsync().get() 没有超时——系统媒体服务在进程退出时可能不响应，
    // 线程会永远停在 get() 上，stopSustainedSignals 的 join() 就跟着挂死，
    // 表现为「窗口关了但进程僵在后台」。所以不用 get()：挂回调 + 分片等，
    // 超过 2 秒取消（Close + 丢弃引用），线程总能回到循环检查 running 标志。
    auto operation = winrt::GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
    winrt::GlobalSystemMediaTransportControlsSessionManager manager = nullptr;
    bool completed = false;
    operation.Completed([&](auto&& async, auto&&) {
        // 回调线程里只做「取结果 + 置标志」，真正的超时取消在下面轮询。
        if (async.Status() == winrt::AsyncStatus::Completed && !g_mediaQueryAbort.load()) {
            manager = async.GetResults();
        }
        completed = true;
    });

    for (int waited = 0; waited < kQueryTimeoutMs && !completed; waited += 50) {
        ::Sleep(50);
    }
    if (!completed) {
        // 超时：Cancel 再 Close，丢弃引用让 WinRT 自己收尾。
        operation.Cancel();
        operation.Close();
        g_mediaQueryAbort.store(true);
        return SustainedSignalSnapshot{}; // available=false，本路信号不可用
    }
    if (g_mediaQueryAbort.load() || manager == nullptr) {
        return SustainedSignalSnapshot{};
    }

    auto sessions = manager.GetSessions();
    const quint32 count = sessions.Size();

    // 优先返回「正在播放」的第一个会话，否则退回第一个可用会话。
    winrt::GlobalSystemMediaTransportControlsSession chosen = nullptr;
    bool hasActive = false;
    bool hasAny = false;
    for (quint32 index = 0; index < count; ++index) {
        auto session = sessions.GetAt(index);
        auto info = session.GetPlaybackInfo();
        const auto status = info.PlaybackStatus();
        if (status == winrt::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
            if (!hasActive) {
                chosen = session;
                hasActive = true;
            }
        } else if (!hasAny) {
            chosen = session;
            hasAny = true;
        }
    }

    if (!hasActive && !hasAny) {
        return result; // 可用但没在播
    }

    result.sourceAppId = QString::fromWCharArray(chosen.SourceAppUserModelId().c_str());
    result.active = hasActive;
    return result;
}

void mediaReconcile()
{
    SustainedSignalSnapshot snapshot;
    try {
        snapshot = queryMediaSession();
    } catch (const winrt::hresult_error& error) {
        snapshot = SustainedSignalSnapshot{};
        logProbeError("media",
                      QStringLiteral("SMTC 探测失败（0x%1）：%2")
                          .arg(QString::number(error.code(), 16),
                               QString::fromWCharArray(error.message().c_str())));
    } catch (...) {
        snapshot = SustainedSignalSnapshot{};
        logProbeError("media", QStringLiteral("SMTC 探测未知异常"));
    }
    writeSlot(mediaSlot(), snapshot);
}

void mediaThreadLoop(std::atomic_bool& running)
{
    // WinRT 的 apartment：探测线程自己 init / uninit。
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    while (running.load(std::memory_order_relaxed) && !g_mediaQueryAbort.load()) {
        runGuarded("media", mediaReconcile);
        for (int waited = 0; waited < kReconcileIntervalMs && running.load(std::memory_order_relaxed);
             waited += 100) {
            ::Sleep(100);
        }
    }
    winrt::uninit_apartment();
}

// ---------------------------------------------------------------------------
// WASAPI 探测（音频会话）
// ---------------------------------------------------------------------------

struct ComGuard
{
    bool shouldUninitialize = false;
    bool ok = false;

    bool initialize()
    {
        // 探测线程是独立线程， apartment 模式随便选；MTD 兼容性最好。
        const HRESULT result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (result == S_OK || result == S_FALSE) {
            shouldUninitialize = true;
            ok = true;
            return true;
        }
        if (result == RPC_E_CHANGED_MODE) {
            // 已经以别的模式初始化过：不重复初始化，也不 uninit。
            ok = true;
            return true;
        }
        return false;
    }

    ~ComGuard()
    {
        if (shouldUninitialize) {
            ::CoUninitialize();
        }
    }
};

SustainedSignalSnapshot queryAudioSessions()
{
    ComGuard com;
    if (!com.initialize()) {
        return SustainedSignalSnapshot{};
    }

    SustainedSignalSnapshot result;
    result.available = true;
    result.source = SustainedSourceKind::AudioSession;

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator)))) {
        return result;
    }
    IMMDevice* device = nullptr;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device))) {
        enumerator->Release();
        return result;
    }
    IAudioSessionManager2* manager = nullptr;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&manager)))) {
        device->Release();
        enumerator->Release();
        return result;
    }
    IAudioSessionEnumerator* sessionEnumerator = nullptr;
    if (FAILED(manager->GetSessionEnumerator(&sessionEnumerator))) {
        manager->Release();
        device->Release();
        enumerator->Release();
        return result;
    }

    int sessionCount = 0;
    sessionEnumerator->GetCount(&sessionCount);
    const int limit = sessionCount < kAudioSessionLimit ? sessionCount : kAudioSessionLimit;

    for (int index = 0; index < limit; ++index) {
        IAudioSessionControl* control = nullptr;
        if (FAILED(sessionEnumerator->GetSession(index, &control))) {
            continue;
        }
        IAudioSessionControl2* control2 = nullptr;
        if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                              reinterpret_cast<void**>(&control2)))) {
            AudioSessionState state = AudioSessionStateInactive;
            if (SUCCEEDED(control2->GetState(&state)) && state == AudioSessionStateActive) {
                DWORD processId = 0;
                if (SUCCEEDED(control2->GetProcessId(&processId)) && processId != 0) {
                    // 真实进程的 exe 路径与文件名：音频信号源靠它跟前台
                    // 窗口对齐（Chrome 放视频 → audio 会话的 pid 是 chrome.exe）。
                    HANDLE processRaw =
                        ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
                    if (processRaw != nullptr) {
                        wchar_t pathBuffer[1024] = {};
                        DWORD pathSize = 1024;
                        if (::QueryFullProcessImageNameW(processRaw, 0, pathBuffer, &pathSize)) {
                            const QString path = QString::fromWCharArray(pathBuffer,
                                                                         static_cast<int>(pathSize));
                            result.exePath = path;
                            const int separator = path.lastIndexOf(QLatin1Char('\\'));
                            result.exeName =
                                separator >= 0 ? path.mid(separator + 1) : path;
                        }
                        ::CloseHandle(processRaw);
                    }
                    // 多个活跃会话时取第一个（都来自同一台机器，取谁
                    // 差别不大）。
                    result.active = true;
                    break;
                }
            }
            control2->Release();
        }
        control->Release();
    }

    sessionEnumerator->Release();
    manager->Release();
    device->Release();
    enumerator->Release();
    return result;
}

void audioReconcile()
{
    const SustainedSignalSnapshot snapshot = queryAudioSessions();
    writeSlot(audioSlot(), snapshot);
}

void audioThreadLoop(std::atomic_bool& running)
{
    while (running.load(std::memory_order_relaxed)) {
        runGuarded("audio", audioReconcile);
        for (int waited = 0; waited < kReconcileIntervalMs && running.load(std::memory_order_relaxed);
             waited += 100) {
            ::Sleep(100);
        }
    }
}

std::atomic_bool g_mediaRunning{false};
std::atomic_bool g_audioRunning{false};
std::unique_ptr<std::thread> g_mediaThread;
std::unique_ptr<std::thread> g_audioThread;

} // namespace

void startSustainedSignals()
{
    if (!g_mediaRunning.exchange(true)) {
        g_mediaThread = std::make_unique<std::thread>(mediaThreadLoop, std::ref(g_mediaRunning));
    }
    if (!g_audioRunning.exchange(true)) {
        g_audioThread = std::make_unique<std::thread>(audioThreadLoop, std::ref(g_audioRunning));
    }
}

void stopSustainedSignals()
{
    // 先通知 SMTC 查询取消：卡在 get 等价物里的线程靠这个标志跳出。
    g_mediaQueryAbort.store(true);
    g_mediaRunning.store(false, std::memory_order_relaxed);
    g_audioRunning.store(false, std::memory_order_relaxed);
    if (g_mediaThread && g_mediaThread->joinable()) {
        g_mediaThread->join();
        g_mediaThread.reset();
    }
    if (g_audioThread && g_audioThread->joinable()) {
        g_audioThread->join();
        g_audioThread.reset();
    }
    g_mediaQueryAbort.store(false);
}

SustainedSignalsSnapshot currentSustainedSignals()
{
    SustainedSignalsSnapshot snapshot;
    snapshot.systemMedia = readSlot(mediaSlot());
    snapshot.audioSession = readSlot(audioSlot());

    // 过期快照按「这一路不可用」处理：探测线程卡住/系统睡眠唤醒后，
    // 上一轮的「正在播放」不能再撑参与时长。generatedAtMs == 0 表示
    // 从未探测成功过，同样不可用。
    const qint64 now = nowMs();
    auto expireIfStale = [now](SustainedSignalSnapshot& signal) {
        if (signal.generatedAtMs <= 0 || now - signal.generatedAtMs > kSnapshotTtlMs) {
            signal = SustainedSignalSnapshot{};
        }
    };
    expireIfStale(snapshot.systemMedia);
    expireIfStale(snapshot.audioSession);
    return snapshot;
}

} // namespace patina::platform
