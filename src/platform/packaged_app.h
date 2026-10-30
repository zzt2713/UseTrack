#ifndef PATINA_PLATFORM_PACKAGED_APP_H
#define PATINA_PLATFORM_PACKAGED_APP_H

#include <QString>
#include <QtGlobal>

namespace patina::platform
{

// UWP 宿主投影。
//
// 商店应用的窗口都挂在 ApplicationFrameHost.exe 这个壳进程下：壳有自己的
// pid 与 exe 路径，直接记录的话所有 UWP 应用都会记成同一个壳。
// 这里把壳窗口投影回真实应用：真实 pid、真实 exe、包标识 AUMID。
//
// 投影算法：枚举壳窗口的子窗口，找「进程不是壳本身、且能取到 AUMID」的那个；
// AUMID 相同的归为一组，组内 exe 名必须一致，否则视为歧义不投影。
struct HostedApp
{
    quint32 processId = 0;
    QString exeName;
    QString exePath;
    QString appUserModelId;

    bool isValid() const { return processId != 0 && !exeName.isEmpty(); }
};

enum class HostedAppResolution
{
    NotApplicable, // 根进程不是壳，无需投影
    Resolved,      // 投影成功，hosted 有效
    Unavailable,   // 是壳但找不到真实应用（投影失败）
    Ambiguous      // 子窗口里撞出多个候选（投影失败）
};

// root exe 名大小写不敏感地等于 ApplicationFrameHost.exe 时才尝试投影。
// hostedOut 非 null 时接收投影出的真实应用（仅 Resolved 时有效）。
HostedAppResolution resolveHostedApp(quintptr rootOwnerHwnd,
                                     quint32 rootProcessId,
                                     const QString& rootExeName,
                                     HostedApp* hostedOut = nullptr);

// resolveHostedApp 的带缓存版本：同样输入在 1 秒内直接复用上一次投影结果。
// 壳窗口的子窗口枚举 + AUMID 查询不便宜，1 秒采样间隔下必须做。
struct CachedProjection
{
    HostedAppResolution resolution = HostedAppResolution::NotApplicable;
    HostedApp hosted;
};
CachedProjection resolveHostedAppCached(quintptr rootOwnerHwnd,
                                        quint32 rootProcessId,
                                        const QString& rootExeName);

// 取进程的 AUMID（GetApplicationUserModelId）。非打包进程返回空串。
// 结果带缓存：壳窗口里同一个 pid 会被反复查询，10 秒内的正结果、1 秒内的
// 负结果直接复用（负缓存短是因为进程可能刚启动、包标识晚一点才可查）。
QString queryProcessAppUserModelId(quint32 processId);

// 进程的 exe 路径与文件名（壳子窗口的 pid 归属真实应用，需要单独查路径）。
QString processImagePathOf(quint32 processId);
QString processExeNameOf(quint32 processId);

} // namespace patina::platform

#endif // PATINA_PLATFORM_PACKAGED_APP_H
