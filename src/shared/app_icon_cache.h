#ifndef PATINA_SHARED_APP_ICON_CACHE_H
#define PATINA_SHARED_APP_ICON_CACHE_H

#include <QPixmap>
#include <QString>

namespace patina::shared
{

// 应用图标（UI 侧取图入口）。
//
// 为什么要这一层：`ui/*` 不 include `platform/*`，
// 而图标解码是 Win32 的活（ExtractIconExW + DIB 转换），只能待在 platform/。
// 这里做一次转手：取 QImage → 转 QPixmap → 自己再做一层按尺寸的缓存。
//
// 双层缓存是有意的：platform/ 那层缓存的是 QImage（无 GUI 依赖，采样线程也能用），
// 这层缓存的是已经上过屏的 QPixmap——每帧把 QImage 转一次 QPixmap 太贵。
// 两层上限都是 256 项，且只存缩放后的位图。
//
// dpr 是设备像素比：`size` 仍是**逻辑像素**（控件上的摆放尺寸），但内部按
// `size * dpr` 的物理像素取图，返回的 pixmap 已经 setDevicePixelRatio(dpr)。
// 这样 150% / 200% 缩放下图标不会被 Qt 放大采样，和高 dpi 屏上一样锐利。
// 调用方从 QWidget 里传 `devicePixelRatioF()` 即可，画的时候照旧按逻辑坐标画。
//
// 取不到图标返回空 QPixmap，调用方自己画首字母占位；失败结果同样进缓存，
// 免得每次重绘都去解一遍 exe 资源。
// 线程约束：只能在 GUI 线程调用（QPixmap 的硬性要求）。
QPixmap appIcon(const QString& exePath, int size, qreal dpr = 1.0);

// 主题切换等需要重新取图的时机调用；一般不用手动清。
void clearAppIconCache();

// 两层缓存当前的条目数之和（诊断面板读取）。只能在 GUI 线程调用。
int appIconCacheEntryCount();

} // namespace patina::shared

#endif // PATINA_SHARED_APP_ICON_CACHE_H
