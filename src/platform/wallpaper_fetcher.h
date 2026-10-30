#ifndef PATINA_PLATFORM_WALLPAPER_FETCHER_H
#define PATINA_PLATFORM_WALLPAPER_FETCHER_H

#include <QByteArray>
#include <QString>

namespace patina::platform
{

// 壁纸下载结果。ok 为假时 data 无意义，error 是可直接显示的中文原因。
struct WallpaperResponse
{
    bool ok = false;
    QByteArray data;
    QString contentType;
    QString error;
};

// 用 WinHTTP 拉一张图（跟随重定向，自动走系统代理）。
//
// 为什么不用 Qt Network：Qt 只用 Core/Gui/Widgets/Sql，
// 且点名禁用 QNetworkAccessManager。WinHTTP 是 Windows 自带系统库，
// 调用点收在 platform/ 里，与「Win32 只在 platform/」一致。
//
// 单次请求，同步阻塞，只在用户点「刷新壁纸」时调用；调用方负责放到后台线程，
// 避免在 UI 线程上等网络。线程约束：可在任意非 UI 线程调用。
WallpaperResponse fetchWallpaper(const QString& url);

} // namespace patina::platform

#endif // PATINA_PLATFORM_WALLPAPER_FETCHER_H
