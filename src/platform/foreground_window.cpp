#include "platform/foreground_window.h"

#include "platform/packaged_app.h"
#include "platform/win_handle.h"

#include <QString>

#include <tlhelp32.h>
#include <cwchar>

namespace patina::platform
{
namespace
{

constexpr int kTitleBufferLength = 512;
constexpr int kClassBufferLength = 256;
constexpr int kPathBufferLength = 1024;

QString fromWide(const wchar_t* text, int length)
{
    if (text == nullptr || length <= 0) {
        return QString();
    }
    return QString::fromWCharArray(text, length);
}

QString windowTitle(HWND hwnd)
{
    wchar_t buffer[kTitleBufferLength] = {};
    const int length = ::GetWindowTextW(hwnd, buffer, kTitleBufferLength);
    return fromWide(buffer, length);
}

QString windowClass(HWND hwnd)
{
    wchar_t buffer[kClassBufferLength] = {};
    const int length = ::GetClassNameW(hwnd, buffer, kClassBufferLength);
    return fromWide(buffer, length);
}

// 进程映像路径：OpenProcess + QueryFullProcessImageNameW 是首选，
// 拿不到（权限不足的提权进程等）时再退回 Toolhelp 快照取 exe 名。
QString processImagePath(DWORD processId)
{
    if (processId == 0) {
        return QString();
    }

    HANDLE raw = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (raw == nullptr) {
        return QString();
    }

    detail::UniqueHandle process(raw);
    wchar_t buffer[kPathBufferLength] = {};
    DWORD size = kPathBufferLength;
    if (!::QueryFullProcessImageNameW(process.get(), 0, buffer, &size)) {
        return QString();
    }
    return fromWide(buffer, static_cast<int>(size));
}

QString exeNameFromSnapshot(DWORD processId)
{
    detail::UniqueHandle snapshot(
        ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        return QString();
    }

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(PROCESSENTRY32W);

    if (!::Process32FirstW(snapshot.get(), &entry)) {
        return QString();
    }

    do {
        if (entry.th32ProcessID == processId) {
            return fromWide(entry.szExeFile, static_cast<int>(std::wcslen(entry.szExeFile)));
        }
    } while (::Process32NextW(snapshot.get(), &entry));

    return QString();
}

QString fileNameOf(const QString& path)
{
    const int separator = path.lastIndexOf(QLatin1Char('\\'));
    if (separator < 0) {
        return path;
    }
    return path.mid(separator + 1);
}

bool isInactiveWindow(HWND hwnd)
{
    return hwnd == nullptr
        || !::IsWindow(hwnd)
        || !::IsWindowVisible(hwnd)
        || ::IsIconic(hwnd);
}

HWND rootOwnerWindow(HWND hwnd)
{
    HWND rootOwner = ::GetAncestor(hwnd, GA_ROOTOWNER);
    return rootOwner != nullptr ? rootOwner : hwnd;
}

// explorer.exe 的 Progman / WorkerW / 任务栏是外壳表面，不算用户活动；
// 但资源管理器窗口本身（CabinetWClass / ExploreWClass）要算，
// 且必须是无 owner、非工具窗口的顶层窗口，否则是弹层。
bool isShellSurface(HWND rootOwnerHwnd, const QString& exeName, const QString& windowClassName)
{
    if (exeName.compare(QLatin1String("explorer.exe"), Qt::CaseInsensitive) != 0) {
        return false;
    }

    const QString classKey = windowClassName.toLower();
    if (classKey != QLatin1String("cabinetwclass") && classKey != QLatin1String("explorewclass")) {
        return true;
    }

    if (isInactiveWindow(rootOwnerHwnd)) {
        return true;
    }
    if (::GetWindow(rootOwnerHwnd, GW_OWNER) != nullptr) {
        return true;
    }

    const LONG_PTR extendedStyle = ::GetWindowLongPtrW(rootOwnerHwnd, GWL_EXSTYLE);
    return (extendedStyle & WS_EX_TOOLWINDOW) != 0;
}

} // namespace

bool ForegroundWindow::hasSameIdentity(const ForegroundWindow& other) const
{
    return hwnd == other.hwnd
        && rootOwnerHwnd == other.rootOwnerHwnd
        && processId == other.processId
        && windowClass == other.windowClass
        && title == other.title
        && exeName == other.exeName
        && exePath == other.exePath
        && appUserModelId == other.appUserModelId;
}

ForegroundWindow sampleForegroundWindow()
{
    ForegroundWindow result;

    const HWND hwnd = ::GetForegroundWindow();
    if (isInactiveWindow(hwnd)) {
        return result;
    }

    const HWND rootOwnerHwnd = rootOwnerWindow(hwnd);
    if (isInactiveWindow(rootOwnerHwnd)) {
        return result;
    }

    DWORD rootProcessId = 0;
    ::GetWindowThreadProcessId(rootOwnerHwnd, &rootProcessId);

    QString rootExePath = processImagePath(rootProcessId);
    QString rootExeName = fileNameOf(rootExePath);
    if (rootExeName.isEmpty()) {
        rootExeName = exeNameFromSnapshot(rootProcessId);
    }

    // 进程信息取不到就没有可记录的对象，按“无前台应用”处理。
    if (rootProcessId == 0 || rootExeName.trimmed().isEmpty()) {
        return result;
    }

    // 壳窗口投影。root 是 ApplicationFrameHost 时把窗口归到真实应用，
    // 投影失败（Unavailable/Ambiguous）保持 root 值，交给 process_filter 排除。
    quint32 processId = rootProcessId;
    QString exePath = rootExePath;
    QString exeName = rootExeName;
    QString appUserModelId;
    const CachedProjection projection =
        resolveHostedAppCached(reinterpret_cast<quintptr>(rootOwnerHwnd),
                               rootProcessId, rootExeName);
    if (projection.resolution == HostedAppResolution::Resolved) {
        processId = projection.hosted.processId;
        exePath = projection.hosted.exePath;
        exeName = projection.hosted.exeName;
        appUserModelId = projection.hosted.appUserModelId;
    }

    const QString title = windowTitle(hwnd);
    const QString className = windowClass(hwnd);
    if (isShellSurface(rootOwnerHwnd, exeName, className)) {
        return result;
    }

    result.hwnd = reinterpret_cast<quintptr>(hwnd);
    result.rootOwnerHwnd = reinterpret_cast<quintptr>(rootOwnerHwnd);
    result.processId = static_cast<quint32>(processId);
    result.windowClass = className;
    result.title = title;
    result.exeName = exeName;
    result.exePath = exePath;
    result.appUserModelId = appUserModelId;
    return result;
}

} // namespace patina::platform
