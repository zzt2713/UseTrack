#include "platform/process_resources.h"

#include "platform/win_handle.h"

namespace patina::platform
{
namespace
{

// PROCESS_MEMORY_COUNTERS_EX 的字段布局在 psapi.h 里，这里只用前三个计数器，
// 用固定大小的调用结构传：EX 结构比基础结构多两个字段（PrivateUsage 在末尾）。
// 直接声明本地结构而不 include <psapi.h>，是为了不给 ui/ 方向的编译单元带进更多
// Win32 细节——Windows SDK 的 psapi.h 只能出现在 platform/ 里。
struct ProcessMemoryCountersExLocal
{
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivateUsage;
};

using GetProcessMemoryInfoFn = BOOL(WINAPI*)(HANDLE, ProcessMemoryCountersExLocal*, DWORD);

GetProcessMemoryInfoFn resolveGetProcessMemoryInfo()
{
    static const GetProcessMemoryInfoFn fn = [] {
        const HMODULE module = ::GetModuleHandleW(L"kernel32.dll");
        if (module == nullptr) {
            return static_cast<GetProcessMemoryInfoFn>(nullptr);
        }
        return reinterpret_cast<GetProcessMemoryInfoFn>(
            ::GetProcAddress(module, "K32GetProcessMemoryInfo"));
    }();
    return fn;
}

} // namespace

ProcessResources currentProcessResources()
{
    ProcessResources resources;

    DWORD handleCount = 0;
    if (::GetProcessHandleCount(::GetCurrentProcess(), &handleCount)) {
        resources.handleCount = static_cast<int>(handleCount);
    }

    const GetProcessMemoryInfoFn getMemoryInfo = resolveGetProcessMemoryInfo();
    if (getMemoryInfo != nullptr) {
        ProcessMemoryCountersExLocal counters = {};
        counters.cb = sizeof(ProcessMemoryCountersExLocal);
        if (getMemoryInfo(::GetCurrentProcess(), &counters, counters.cb)) {
            resources.workingSetBytes = static_cast<qint64>(counters.WorkingSetSize);
            resources.privateBytes = static_cast<qint64>(counters.PrivateUsage);
            resources.valid = true;
        }
    }

    return resources;
}

bool trimWorkingSet()
{
    // (SIZE_T)-1 是 SetProcessWorkingSetSize 约定：把工作集里能换出的页都换出，
    // 不改变最小/最大工作集限制。需要 PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION，
    // 当前进程句柄天然具备。
    return ::SetProcessWorkingSetSize(::GetCurrentProcess(),
                                      static_cast<SIZE_T>(-1),
                                      static_cast<SIZE_T>(-1)) != FALSE;
}

} // namespace patina::platform
