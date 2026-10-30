#include "platform/icon_extractor.h"

#include "platform/win_handle.h"

#include <QCache>
#include <QFileInfo>
#include <QImage>
#include <QString>

#include <climits>
#include <cstring>
#include <string>

// WIN32_LEAN_AND_MEAN 会排除 shell 相关头文件，ExtractIconExW 要显式引入。
#include <shellapi.h>

namespace patina::platform
{
namespace
{

// 图标缓存上限 256 项。
constexpr int kIconCacheMaxEntries = 256;
// 持久层统一存的高清源尺寸。exe 内嵌图标最大档通常是 256px；存 64px 在 2x
// 屏上不够用——34 逻辑像素要 68 物理像素，源比目标还小，只能放大，必糊。
// 存满 256px，任何 dpr 下的显示尺寸都是下采样。
constexpr int kPersistedSourceSize = 256;

QCache<QString, QImage>& iconCache()
{
    static QCache<QString, QImage> cache(kIconCacheMaxEntries);
    return cache;
}

// 持久化回调（app/ 层注入）。无锁共享：注入发生在启动早期、GUI 线程，
// 而采样线程随后才读；Qt 线程启动本身提供了 happens-before，这里不再加锁。
// 运行中不换回调（换也只会发生在测试解绑时）。
IconLoader& iconLoader()
{
    static IconLoader loader;
    return loader;
}

IconSaver& iconSaver()
{
    static IconSaver saver;
    return saver;
}

QString cacheKey(const QString& exePath, int size)
{
    return QString::number(size) + QLatin1Char('|') + exePath.toLower();
}

// —— HICON → 原生分辨率位图 ——
//
// 关键：GetIconInfo 拿到 hbmColor（exe 内嵌图标的原始位图，
// 通常是 256px 的 PNG 资源解码结果），GetDIBits 按 32bpp 自顶向下取出，
// 全程不做小尺寸重绘。这是「清晰」的秘诀——DrawIconEx(size=32) 会把
// 256px 源缩小重绘，DPI 缩放下锯齿明显；直接取原生位图再由 Qt 平滑
// 缩放到目标尺寸，质量高一个档次。
QImage hiconToNativeImage(HICON icon)
{
    if (icon == nullptr) {
        return QImage();
    }

    ICONINFO info = {};
    if (!::GetIconInfo(icon, &info)) {
        return QImage();
    }
    // hbmMask / hbmColor 由 GetIconInfo 分配，本函数负责释放。
    detail::UniqueBitmap maskBitmap(info.hbmMask);
    detail::UniqueBitmap colorBitmap(info.hbmColor);

    if (colorBitmap.get() == nullptr) {
        // 单色图标没有颜色位图：退回 DrawIconEx 重绘路径（调用方处理）。
        return QImage();
    }

    BITMAP bm = {};
    if (::GetObjectW(colorBitmap.get(), sizeof(BITMAP), &bm) == 0) {
        return QImage();
    }
    const int width = bm.bmWidth;
    const int height = bm.bmHeight;
    if (width <= 0 || height <= 0) {
        return QImage();
    }

    detail::ScreenDc screenDc(::GetDC(nullptr));
    if (!screenDc) {
        return QImage();
    }
    detail::MemoryDc memoryDc(::CreateCompatibleDC(screenDc.get()));
    if (!memoryDc) {
        return QImage();
    }

    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    // 负高度 = 自顶向下 DIB，与 QImage 的行序一致。
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    QImage result(width, height, QImage::Format_ARGB32);
    if (result.isNull()) {
        return QImage();
    }
    const int lines = ::GetDIBits(memoryDc.get(), colorBitmap.get(), 0,
                                  static_cast<UINT>(height), result.bits(),
                                  &bitmapInfo, DIB_RGB_COLORS);
    if (lines == 0) {
        return QImage();
    }
    return result;
}

// 兜底：从窗口句柄要图标。
// 优先级 ICON_BIG → ICON_SMALL2 → ICON_SMALL → 类图标；
// SMTO_ABORTIFHUNG + 100ms 防卡死窗口拖垮采样线程。
// 返回的 HICON 归窗口所有，调用方不得 DestroyIcon。
HICON queryWindowIconHandle(HWND hwnd)
{
    if (hwnd == nullptr) {
        return nullptr;
    }
    static const WPARAM iconTypes[] = {ICON_BIG, ICON_SMALL2, ICON_SMALL};
    for (const WPARAM iconType : iconTypes) {
        DWORD_PTR result = 0;
        if (::SendMessageTimeoutW(hwnd, WM_GETICON, iconType, 0,
                                  SMTO_BLOCK | SMTO_ABORTIFHUNG, 100,
                                  &result) != 0
            && result != 0) {
            return reinterpret_cast<HICON>(result);
        }
    }
    const LONG_PTR classIcon = ::GetClassLongPtrW(hwnd, GCLP_HICON);
    if (classIcon != 0) {
        return reinterpret_cast<HICON>(classIcon);
    }
    const LONG_PTR classSmallIcon = ::GetClassLongPtrW(hwnd, GCLP_HICONSM);
    if (classSmallIcon != 0) {
        return reinterpret_cast<HICON>(classSmallIcon);
    }
    return nullptr;
}

// 在零初值、自上而下的 32bpp DIB 上画一次图标（DrawIconEx 重绘路径，
// 仅作 hbmColor 直取失败时的退路）。
// flags 传 DI_NORMAL 取颜色，传 DI_MASK 取掩码（掩码里白色是透明区域）。
QImage drawIconPass(HICON icon, int size, UINT flags)
{
    if (icon == nullptr || size <= 0) {
        return QImage();
    }

    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = size;
    // 负高度表示自上而下，省掉一次行反转。
    bitmapInfo.bmiHeader.biHeight = -size;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP rawBitmap = ::CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (rawBitmap == nullptr || bits == nullptr) {
        if (rawBitmap != nullptr) {
            ::DeleteObject(rawBitmap);
        }
        return QImage();
    }
    detail::UniqueBitmap bitmap(rawBitmap);

    std::memset(bits, 0, static_cast<size_t>(size) * static_cast<size_t>(size) * 4u);

    detail::ScreenDc screenDc(::GetDC(nullptr));
    if (!screenDc) {
        return QImage();
    }
    detail::MemoryDc memoryDc(::CreateCompatibleDC(screenDc.get()));
    if (!memoryDc) {
        return QImage();
    }

    HGDIOBJ previous = ::SelectObject(memoryDc.get(), rawBitmap);
    const BOOL drawn = ::DrawIconEx(memoryDc.get(), 0, 0, icon, size, size, 0, nullptr, flags);
    ::SelectObject(memoryDc.get(), previous);

    if (!drawn) {
        return QImage();
    }

    // 32bpp DIB 的内存序是 BGRA，正好对应 QImage::Format_ARGB32。
    const QImage view(static_cast<const uchar*>(bits), size, size, size * 4, QImage::Format_ARGB32);
    return view.copy();
}

bool isFullyTransparent(const QImage& image)
{
    for (int y = 0; y < image.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) != 0) {
                return false;
            }
        }
    }
    return true;
}

void fillOpaque(QImage& image)
{
    for (int y = 0; y < image.height(); ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            line[x] |= 0xFF000000u;
        }
    }
}

// 用掩码补 alpha：掩码里非黑即透明，黑色为不透明。
void applyMaskAlpha(QImage& image, const QImage& mask)
{
    for (int y = 0; y < image.height(); ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        const QRgb* maskLine = reinterpret_cast<const QRgb*>(mask.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const bool transparent = (maskLine[x] & 0x00FFFFFFu) != 0;
            line[x] = transparent ? 0u : (line[x] | 0xFF000000u);
        }
    }
}

QImage renderIcon(HICON icon, int size)
{
    QImage image = drawIconPass(icon, size, DI_NORMAL);
    if (image.isNull() || !isFullyTransparent(image)) {
        // 现代 32bpp 带 alpha 的图标，DI_NORMAL 直接给出了正确的 alpha。
        return image;
    }

    // 老式掩码图标：DI_NORMAL 只写了 RGB，alpha 全留在 0。
    // 再画一遍 DI_MASK 取掩码，据此补出透明度。
    const QImage mask = drawIconPass(icon, size, DI_MASK);
    if (!mask.isNull()) {
        applyMaskAlpha(image, mask);
        return image;
    }

    // 掩码也取不到：退化成不透明方块，至少不是一张全透明的空图。
    fillOpaque(image);
    return image;
}

// 从 exe 文件提取高清图标：优先 hbmColor 原生位图（大图标通常是
// exe 内嵌的 256px PNG 资源），失败退回 DrawIconEx 重绘。
QImage extractIconFromFile(const QString& exePath, int targetSize)
{
    const QString trimmed = exePath.trimmed();
    if (trimmed.isEmpty()) {
        return QImage();
    }

    const std::wstring widePath = trimmed.toStdWString();
    HICON largeIcon = nullptr;
    HICON smallIcon = nullptr;
    const UINT extracted = ::ExtractIconExW(widePath.c_str(), 0, &largeIcon, &smallIcon, 1);

    if (extracted == 0 || extracted == UINT_MAX) {
        return QImage();
    }

    detail::UniqueIcon ownedLarge(largeIcon);
    detail::UniqueIcon ownedSmall(smallIcon);

    // 优先大图标：exe 内嵌的最大尺寸，信息量最全。
    HICON source = ownedLarge ? ownedLarge.get() : ownedSmall.get();
    if (source == nullptr) {
        return QImage();
    }

    // 路径 1（清晰的关键）：GetIconInfo → hbmColor 原生位图，
    // 256px PNG 资源无损解码，之后由 Qt 平滑缩放到目标尺寸。
    QImage native = hiconToNativeImage(source);
    if (!native.isNull()) {
        // hbmColor 尺寸就是图标原生尺寸（常见 32/48/256），保留 alpha。
        return native;
    }

    // 路径 2：原生位图拿不到（单色图标等），按目标尺寸重绘 + 掩码补 alpha。
    return renderIcon(source, targetSize);
}

} // namespace

void setIconPersistence(IconLoader loader, IconSaver saver)
{
    iconLoader() = std::move(loader);
    iconSaver() = std::move(saver);
}

QImage appIconImage(const QString& exePath, int size)
{
    if (exePath.trimmed().isEmpty() || size <= 0) {
        return QImage();
    }

    const QString key = cacheKey(exePath, size);
    if (const QImage* cached = iconCache().object(key)) {
        // 命中空图就是负缓存，直接返回。
        return *cached;
    }

    // 内存 LRU 未命中：先问持久层（键用 exe 文件名，与库里的条目对齐）。
    // 命中则回填内存缓存，不再走 ExtractIconExW——重启后图标直接从库加载。
    // 持久层统一存 kPersistedSourceSize 的高清源：显示端任意 ≤ 该值的尺寸
    // 都是下采样（清晰），换尺寸不再发糊。
    const QString exeName = QFileInfo(exePath).fileName();
    if (!iconLoader() && !iconSaver()) {
        // 未注入持久化（探测程序等场景）：行为与旧版一致。
        const QImage icon = extractIconFromFile(exePath, size);
        iconCache().insert(key, new QImage(icon));
        return icon;
    }

    if (iconLoader()) {
        const QImage persisted = iconLoader()(exeName);
        // 老库里存的是 64px 源，现在要 68+ 物理像素就不够用了。源比目标还小
        // 只能放大，正是糊的成因——这种情况当作未命中，重新解一份高清源覆盖。
        if (!persisted.isNull() && persisted.height() >= size) {
            const QImage scaled =
                persisted.height() == size
                    ? persisted
                    : persisted.scaled(size, size, Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation);
            iconCache().insert(key, new QImage(scaled));
            return scaled;
        }
    }

    // 提取：按持久层标准源尺寸解一份高清源，
    // 内存里另存目标尺寸的缩放。
    QImage source = extractIconFromFile(exePath, kPersistedSourceSize);
    // 写回持久层（含失败负缓存：空图也要记，下次启动别再白解一遍）。
    if (iconSaver()) {
        iconSaver()(exeName, source);
    }

    const QImage scaled =
        source.isNull()
            ? QImage()
            : (source.height() == size
                   ? source
                   : source.scaled(size, size, Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation));
    iconCache().insert(key, new QImage(scaled));
    return scaled;
}

void clearIconCache()
{
    iconCache().clear();
}

int iconCacheEntryCount()
{
    return iconCache().size();
}

} // namespace patina::platform
