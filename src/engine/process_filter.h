#ifndef PATINA_ENGINE_PROCESS_FILTER_H
#define PATINA_ENGINE_PROCESS_FILTER_H

#include "engine/window_sample.h"

#include <QString>

namespace patina::engine
{

// 该 exe 是否值得作为活动记录。
//
// 三条排除线：
//   1. 系统外壳/输入/安全类进程的固定名单；
//   2. 名字像系统宿主进程（searchhost、runtimebroker、*host.exe 里带 runtime 等）；
//   3. 安装/升级/卸载这类生命周期工具，它们出现在前台不代表用户在用那个应用。
bool shouldTrackProcess(const QString& exeName);

// 整个采样是否可跟踪：空样本、AFK、被过滤的进程、桌面外壳窗口都不可跟踪。
bool isTrackableSample(const WindowSample& sample);

} // namespace patina::engine

#endif // PATINA_ENGINE_PROCESS_FILTER_H
