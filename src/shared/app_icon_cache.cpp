#include "shared/app_icon_cache.h"

#include "platform/icon_extractor.h"

#include <QCache>
#include <QImage>

namespace patina::shared
{
namespace
{

// 与内存红线一致：图标缓存上限 256 项。
constexpr int kIconCacheMaxEntries = 256;

// 键含尺寸与 dpr：同一 exe 在不同控件里可能是 32 / 34 / 40，各存一份缩放后的位图；
// 同一逻辑尺寸在 100% / 150% / 200% 屏上物理像素不同，也得分开存。
QString cacheKey(const QString& exePath, int size, qreal dpr)
{
    return QString::number(size) + QLatin1Char('@') + QString::number(dpr, 'f', 2)
        + QLatin1Char('|') + exePath.toLower();
}

QCache<QString, QPixmap>& pixmapCache()
{
    static QCache<QString, QPixmap> cache(kIconCacheMaxEntries);
    return cache;
}

} // namespace

QPixmap appIcon(const QString& exePath, int size, qreal dpr)
{
    if (exePath.trimmed().isEmpty() || size <= 0)
    {
        return QPixmap();
    }

    // dpr 兜底：0 或负值（某些离屏控件还没上屏时 dpr 是 0）按 1.0 处理，
    // 否则物理尺寸会算成 0，取图直接空。
    const qreal effectiveDpr = dpr > 0.0 ? dpr : 1.0;
    const int physicalSize = qMax(1, qRound(size * effectiveDpr));

    const QString key = cacheKey(exePath, size, effectiveDpr);
    if (const QPixmap* cached = pixmapCache().object(key))
    {
        return *cached;
    }

    // platform/ 那层会自己做 LRU 与失败负缓存，这里拿到的可能已经是空图。
    // 传物理尺寸进去：2x 屏上要的是 68 而不是 34，差这一步就是「糊」的根源。
    const QImage image = patina::platform::appIconImage(exePath, physicalSize);
    QPixmap pixmap;
    if (!image.isNull())
    {
        pixmap = QPixmap::fromImage(image);
        // 标上 dpr，画的时候按逻辑尺寸摆，Qt 自己按 dpr 采样回物理像素。
        pixmap.setDevicePixelRatio(effectiveDpr);
    }

    // 空图也存进去：解不出图标的 exe 每帧重试一次只会白烧 CPU。
    pixmapCache().insert(key, new QPixmap(pixmap));
    return pixmap;
}

void clearAppIconCache()
{
    pixmapCache().clear();
    patina::platform::clearIconCache();
}

int appIconCacheEntryCount()
{
    return pixmapCache().size() + patina::platform::iconCacheEntryCount();
}

} // namespace patina::shared
