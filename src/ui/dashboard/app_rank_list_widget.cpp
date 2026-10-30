#include "ui/dashboard/app_rank_list_widget.h"

#include "ElaTheme.h"

#include "data/repositories.h"
#include "shared/app_icon_cache.h"

#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>

#include <QContextMenuEvent>

#include <algorithm>
#include <cmath>

namespace patina::ui
{
namespace
{

constexpr int kRowHeight = 58;
constexpr int kRowGap = 8;
constexpr int kIconSize = 34;
constexpr int kBarWidth = 96;
constexpr int kBarHeight = 5;
constexpr int kHorizontalPadding = 11;
constexpr int kBadgeRadius = 7;
constexpr int kBadgeHPad = 6;
constexpr int kBadgeGap = 5;
constexpr int kBadgeBottomGap = 3;

QString formatDuration(qint64 ms)
{
    const qint64 totalMinutes = std::max<qint64>(0, ms) / 60000;
    const qint64 hours = totalMinutes / 60;
    const qint64 minutes = totalMinutes % 60;
    if (hours > 0) {
        return QStringLiteral("%1h %2m")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1m").arg(minutes);
}

// 展示名：去掉 .exe 后缀，首字母大写。真正的产品名要等 platform/ 的文件版本信息，
// 那之前用进程名比留空更可读。
QString displayNameFor(const QString& exeName)
{
    QString name = exeName.trimmed();
    if (name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        name.chop(4);
    }
    if (name.isEmpty()) {
        return QStringLiteral("未知应用");
    }
    name[0] = name.at(0).toUpper();
    return name;
}

} // namespace

AppRankListWidget::AppRankListWidget(data::Database* database, QWidget* parent)
    : QWidget(parent), _database(database)
{
    // 高度由行数决定（sizeHint），宽度跟随卡片。
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // 行可点（详情入口），悬停给手型光标提示。
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
}

int AppRankListWidget::rowAt(int localY) const
{
    if (_rows.isEmpty()) {
        return -1;
    }
    const int stride = kRowHeight + kRowGap;
    const int index = localY / stride;
    if (index < 0 || index >= _rows.size()) {
        return -1;
    }
    // 行之间的间隙不算命中。
    const int offset = localY - index * stride;
    return offset <= kRowHeight ? index : -1;
}

void AppRankListWidget::mousePressEvent(QMouseEvent* event)
{
    const int row = rowAt(static_cast<int>(event->position().y()));
    if (row >= 0) {
        emit appClicked(_rows.at(row).exeName);
        return;
    }
    QWidget::mousePressEvent(event);
}

void AppRankListWidget::mouseMoveEvent(QMouseEvent* event)
{
    const int row = rowAt(static_cast<int>(event->position().y()));
    if (row != _hoverRow) {
        _hoverRow = row;
        update();
    }
}

void AppRankListWidget::leaveEvent(QEvent* event)
{
    QWidget::leaveEvent(event);
    if (_hoverRow != -1) {
        _hoverRow = -1;
        update();
    }
}

void AppRankListWidget::contextMenuEvent(QContextMenuEvent* event)
{
    const int row = rowAt(static_cast<int>(event->pos().y()));
    if (row < 0) {
        QWidget::contextMenuEvent(event);
        return;
    }
    const Row& data = _rows.at(row);
    emit appContextMenuRequested(data.exeName, data.categoryId, data.displayName,
                                 event->globalPos());
}

void AppRankListWidget::setApps(const QVector<data::AppUsage>& apps, qint64 totalDurationMs)
{
    // 注册表快照在循环外读一次：它是扫 settings 表的查询，逐 app 读会变成 N+1。
    // 首页有活跃段时 2 秒刷一轮，这个差别很实在。
    data::CategoryRegistrySnapshot registry;
    bool hasRegistry = false;
    if (_database != nullptr && _database->isOpen()) {
        const data::Result<data::CategoryRegistrySnapshot> loaded =
            data::loadCategoryRegistrySnapshot(*_database);
        if (loaded.ok() && !loaded.value.entries.isEmpty()) {
            registry = loaded.value;
            hasRegistry = true;
        }
    }

    _rows.clear();
    _rows.reserve(apps.size());
    for (const data::AppUsage& app : apps) {
        Row row;
        row.exeName = app.exeName;
        row.exePath = app.exePath;
        row.displayName = app.displayName.trimmed().isEmpty() ? displayNameFor(app.exeName)
                                                             : app.displayName.trimmed();
        // 分类管理页的覆盖项优先，否则用户改完分类这里还显示旧分类，看着像没生效。
        // 名字/颜色走注册表（可改名/配色/增删）；注册表不可用时退内置表。
        QString categoryId = data::resolveCategoryId(app.exeName, app.categoryId);
        QString categoryLabel;
        QString categoryColorHex;
        if (hasRegistry) {
            categoryId = registry.resolveId(categoryId);
            categoryLabel = registry.labelFor(categoryId);
            categoryColorHex = registry.colorFor(categoryId);
        }
        if (categoryLabel.isEmpty()) {
            const data::CategoryToken token = data::categoryToken(categoryId);
            categoryLabel = token.label;
            categoryColorHex = token.colorHex;
        }
        // 生效分类 id 单独留着：未分类徽标与
        // 右键菜单的当前项打勾都要用 id。resolveCategoryId 对空覆盖回落内置表，
        // 表里没有的才归 other —— 徽标只挂在真正的「未分类」上。
        row.categoryId = categoryId;
        row.categoryLabel = categoryLabel;
        row.categoryColor = app.colorHex.trimmed().isEmpty()
            ? QColor(categoryColorHex) : QColor(app.colorHex.trimmed());
        row.durationMs = std::max<qint64>(0, app.durationMs);
        row.percentage = totalDurationMs > 0
            ? static_cast<int>(std::lround(100.0 * static_cast<double>(row.durationMs)
                                           / static_cast<double>(totalDurationMs)))
            : 0;
        _rows.append(row);
    }

    updateGeometry();
    update();
}

void AppRankListWidget::clear()
{
    if (_rows.isEmpty()) {
        return;
    }
    _rows.clear();
    updateGeometry();
    update();
}

QSize AppRankListWidget::sizeHint() const
{
    // 行高固定，高度直接由行数算出：外层滚动区靠它决定内容总高。
    const int height = _rows.isEmpty()
        ? 0
        : _rows.size() * kRowHeight + (_rows.size() - 1) * kRowGap;
    return {320, height};
}

QSize AppRankListWidget::minimumSizeHint() const
{
    return {240, sizeHint().height()};
}

void AppRankListWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    if (_rows.isEmpty()) {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const ElaThemeType::ThemeMode mode = eTheme->getThemeMode();
    const QColor panelColor = ElaThemeColor(mode, BasicBase);
    const QColor borderColor = ElaThemeColor(mode, BasicBorder);
    const QColor primaryText = ElaThemeColor(mode, BasicText);
    const QColor secondaryText = ElaThemeColor(mode, BasicDetailsText);
    const QColor trackColor = ElaThemeColor(mode, BasicChute);

    QFont nameFont = font();
    nameFont.setPixelSize(13);
    nameFont.setWeight(QFont::Medium);

    QFont metaFont = font();
    metaFont.setPixelSize(11);

    QFont valueFont = font();
    valueFont.setPixelSize(13);
    valueFont.setWeight(QFont::Medium);

    // 未分类徽标：中性底色，画在名字后面。
    QFont badgeFont = font();
    badgeFont.setPixelSize(10);
    const QFontMetrics badgeMetrics(badgeFont);
    const QString badgeText = tr("未分类");

    const QFontMetrics nameMetrics(nameFont);
    const QFontMetrics metaMetrics(metaFont);
    const QFontMetrics valueMetrics(valueFont);

    int y = 0;
    int rowIndex = 0;
    for (const Row& row : _rows) {
        const QRectF cardRect(0, y, width(), kRowHeight);

        // 卡片底与描边：与 Ela 的 BasicBase / BasicBorder 对齐，深浅色自动跟随。
        painter.setPen(QPen(borderColor, 1));
        painter.setBrush(panelColor);
        painter.drawRoundedRect(cardRect.adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
        painter.setBrush(Qt::NoBrush);

        // 悬停描边（可点提示）。
        if (rowIndex == _hoverRow) {
            QPen hoverPen(ElaThemeColor(mode, PrimaryNormal));
            hoverPen.setWidth(2);
            painter.setPen(hoverPen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(cardRect.adjusted(1, 1, -1, -1), 10, 10);
        }

        const qreal contentLeft = kHorizontalPadding;
        const QRectF iconRect(contentLeft, y + (kRowHeight - kIconSize) / 2.0, kIconSize, kIconSize);

        // 优先画 exe 真图标；取不到（路径空、被删、权限不足）才回落到首字母方块。
        // 传 devicePixelRatioF()：2x 屏上按物理像素取图，否则 34px 的图被放大到
        // 68 物理像素，边缘直接糊成一片。
        const QPixmap icon = shared::appIcon(row.exePath, kIconSize, devicePixelRatioF());
        if (!icon.isNull())
        {
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawPixmap(iconRect, icon, QRectF(icon.rect()));
        }
        else
        {
            QColor iconBg = row.categoryColor;
            iconBg.setAlpha(28);
            painter.setPen(Qt::NoPen);
            painter.setBrush(iconBg);
            painter.drawRoundedRect(iconRect, 8, 8);

            QFont iconFont = font();
            iconFont.setPixelSize(13);
            iconFont.setWeight(QFont::DemiBold);
            painter.setFont(iconFont);
            painter.setPen(row.categoryColor);
            painter.drawText(iconRect, Qt::AlignCenter,
                             row.displayName.left(1).toUpper());
        }

        const qreal textLeft = iconRect.right() + 12;
        // 右侧固定三段：时长块 + 百分比块（贴右），中间留间距，剩余宽度给名称。
        const qreal durationBlockWidth = 96;
        const qreal percentBlockWidth = 44;
        const qreal blockGap = 8;
        const qreal percentRight = width() - kHorizontalPadding;
        const qreal durationRight = percentRight - percentBlockWidth - blockGap;
        const qreal textRight = durationRight - durationBlockWidth - 12;
        const qreal textWidth = std::max<qreal>(40, textRight - textLeft);

        painter.setFont(nameFont);
        painter.setPen(primaryText);
        const QString elidedName = nameMetrics.elidedText(
            row.displayName, Qt::ElideRight, static_cast<int>(textWidth));
        painter.drawText(
            QRectF(textLeft, y + 12, textWidth, nameMetrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            elidedName);

        // 未分类徽标：紧跟名字之后。
        // 只对 other 生效——用户主动选过「其他」时 categoryId 已经写成 other，
        // 与未分类是同一生效结果，不再重复提示。
        if (row.categoryId == QStringLiteral("other")) {
            const qreal nameAdvance = nameMetrics.horizontalAdvance(elidedName);
            const qreal badgeWidth = badgeMetrics.horizontalAdvance(badgeText) + kBadgeHPad * 2;
            if (nameAdvance + kBadgeGap + badgeWidth < textWidth) {
                // 胶囊原来写死 +2 偏移与 +4 高度，底边会压到下面「exe · 分类」那一行。
                // 改成跟名字行垂直居中，并强制与下一行留出 3px。
                const qreal badgeHeight = badgeMetrics.height() + 2;
                const qreal metaTop = y + 12 + nameMetrics.height();
                qreal badgeTop = y + 12 + (nameMetrics.height() - badgeHeight) / 2.0;
                if (badgeTop + badgeHeight > metaTop - kBadgeBottomGap) {
                    badgeTop = metaTop - kBadgeBottomGap - badgeHeight;
                }
                const QRectF badgeRect(textLeft + nameAdvance + kBadgeGap, badgeTop,
                                       badgeWidth, badgeHeight);
                painter.setFont(badgeFont);
                painter.setPen(secondaryText);
                painter.setBrush(trackColor);
                painter.drawRoundedRect(badgeRect, kBadgeRadius, kBadgeRadius);
                painter.drawText(badgeRect, Qt::AlignCenter, badgeText);
                painter.setFont(nameFont);
                painter.setPen(primaryText);
                painter.setBrush(Qt::NoBrush);
            }
        }

        painter.setFont(metaFont);
        painter.setPen(secondaryText);
        painter.drawText(
            QRectF(textLeft, y + 12 + nameMetrics.height(), textWidth, metaMetrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            metaMetrics.elidedText(
                QStringLiteral("%1 · %2").arg(row.exeName, row.categoryLabel),
                Qt::ElideRight, static_cast<int>(textWidth)));

        const QRectF valueRect(durationRight - durationBlockWidth, y + 10,
                               durationBlockWidth, valueMetrics.height());
        painter.setFont(valueFont);
        painter.setPen(primaryText);
        painter.drawText(valueRect, Qt::AlignRight | Qt::AlignVCenter,
                         formatDuration(row.durationMs));

        // 百分比单独占一块，用次级色弱化；不与名称列抢空间。
        painter.setFont(metaFont);
        painter.setPen(secondaryText);
        painter.drawText(QRectF(percentRight - percentBlockWidth, y + 10,
                                percentBlockWidth, metaMetrics.height()),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("%1%").arg(row.percentage));

        // 进度条压在时长块下面，右端与时长文字右对齐。
        const QRectF barTrack(durationRight - kBarWidth, y + kRowHeight - 18, kBarWidth, kBarHeight);
        painter.setPen(Qt::NoPen);
        painter.setBrush(trackColor);
        painter.drawRoundedRect(barTrack, kBarHeight / 2.0, kBarHeight / 2.0);

        const qreal filled = barTrack.width() * std::clamp(row.percentage, 0, 100) / 100.0;
        if (filled > 0) {
            painter.setBrush(row.categoryColor);
            painter.drawRoundedRect(
                QRectF(barTrack.left(), barTrack.top(), filled, barTrack.height()),
                kBarHeight / 2.0, kBarHeight / 2.0);
        }
        painter.setBrush(Qt::NoBrush);

        y += kRowHeight + kRowGap;
        ++rowIndex;
    }
}

} // namespace patina::ui
