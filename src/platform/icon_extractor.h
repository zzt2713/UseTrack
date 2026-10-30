#ifndef PATINA_PLATFORM_ICON_EXTRACTOR_H
#define PATINA_PLATFORM_ICON_EXTRACTOR_H

#include <QImage>
#include <QString>

#include <functional>

namespace patina::platform
{

// 图标持久化的回落接口：内存 LRU 未命中时先问持久层，提取成功后写回。
//
// platform/ 不准 include data/（会形成 platform → data 的反向依赖，
// 且 data/ 是 SQLite 收口），所以用函数指针注入，由 app/ 层接上。
// 两个回调都可能在采样线程调用，实现方自行保证线程安全（QSqlDatabase
// 连接不能跨线程共享，data 层实现里用连接名串行化）。
using IconLoader = std::function<QImage(const QString& exeName)>;
using IconSaver = std::function<void(const QString& exeName, const QImage& image)>;

// 按 exe 路径取应用图标，返回指定边长的 ARGB 位图；取不到返回空 QImage。
//
// 返回 QImage 而不是 QIcon/QPixmap：图标要能被无 GUI 的采样线程和探测程序使用，
// QPixmap 依赖 QGuiApplication，platform/ 不该背这个前提。
//
// 三层顺序：内存 LRU（≤256 项，键含尺寸）→ 持久层（若已注入）→ ExtractIconExW。
// 提取结果（含失败负缓存）写回内存与持久层，进程重启后不再重复解图标。
// 线程约束：内存缓存无锁，只能在同一个线程（采样线程或 GUI 线程）里调用。
QImage appIconImage(const QString& exePath, int size = 32);

// 注入持久化回调。传空函数对象解除绑定（测试里库销毁后要能解绑）。
// exeName 传 exe 文件名（如 "chrome.exe"），持久层按它做键。
void setIconPersistence(IconLoader loader, IconSaver saver);

void clearIconCache();

// 当前内存缓存里的条目数（诊断面板读取）。线程约束同 appIconImage。
int iconCacheEntryCount();

} // namespace patina::platform

#endif // PATINA_PLATFORM_ICON_EXTRACTOR_H
