#ifndef PATINA_PLATFORM_PROCESS_RESOURCES_H
#define PATINA_PLATFORM_PROCESS_RESOURCES_H

#include <QtGlobal>

namespace patina::platform
{

// 当前进程的资源快照（诊断面板用）。
//
// 裁掉了线程数与两处 webview 计数。
struct ProcessResources
{
    bool valid = false;        // 取数失败时为 false，其余字段无意义
    qint64 workingSetBytes = 0; // 工作集：与任务管理器「内存」列同源
    qint64 privateBytes = 0;    // 私有提交：任务管理器「提交大小」
    int handleCount = 0;        // 内核句柄数
};

// 取当前进程的工作集/私有提交/句柄数。
//
// 不使用 GetProcessMemoryInfo 的 psapi 导入库：psapi.dll 在 Win7+ 归入
// kernel32 的 K32GetProcessMemoryInfo，直接 GetProcAddress 取，省一条链接依赖。
// 线程约束：任意线程可调（只读当前进程信息）。
ProcessResources currentProcessResources();

// 把当前进程的工作集还给系统（Win32 SetProcessWorkingSetSize(h, -1, -1)）。
//
// delete 只是把页标成「可复用」，物理页仍留在进程工作集里，任务管理器和
// 本模块的读数都不会动。收托盘拆完页面树后必须显式 trim 一次，数字才降；
// 下次访问这些页时由系统按需缺页换回，代价是短暂卡顿。
// 线程约束：任意线程可调；返回是否调用成功。
bool trimWorkingSet();

} // namespace patina::platform

#endif // PATINA_PLATFORM_PROCESS_RESOURCES_H
