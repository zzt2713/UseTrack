#ifndef PATINA_PLATFORM_AUTOSTART_H
#define PATINA_PLATFORM_AUTOSTART_H

#include <QString>

namespace patina::platform
{

// 开机自启（HKCU\Software\Microsoft\Windows\CurrentVersion\Run）。
//
// 写的是当前用户的分支，不需要管理员权限，也不影响其他用户。
// 值名固定，便于卸载时清理；值是带引号的完整命令行。
//
// 为什么不用任务计划程序：它需要额外的权限与 COM 调用，
// 对「登录后启动一个小工具」这件事是过度设计。
class Autostart
{
public:
    // 用于注册表的命令：当前可执行文件的绝对路径 + 参数。
    // argv 里的第一个元素会被替换成 QCoreApplication::applicationFilePath()。
    static QString commandLine();

    // 读注册表里当前的命令；不存在或读失败都返回空串。
    static QString registeredCommand();

    // 注册表里是否有指向当前程序的自启项。
    // 只判断值是否存在、路径是否就是本程序，不比较参数。
    static bool isEnabled();

    // 写入 / 删除自启项。失败返回 false，原因可经 lastError() 取到。
    static bool setEnabled(bool enabled);

    static QString lastError();
};

} // namespace patina::platform

#endif // PATINA_PLATFORM_AUTOSTART_H
