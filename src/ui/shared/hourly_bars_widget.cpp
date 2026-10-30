#include "ui/shared/hourly_bars_widget.h"

#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>

#include <algorithm>

#include "ElaTheme.h"

namespace patina::ui
{
namespace
{

constexpr int kAxisHeight = 18;
constexpr int kMaxSlices = 6;   // 柱内分段上限（其余并入其他）
constexpr qreal kBarRatio = 0.55;

QString formatMinutes(qint64 ms)
{
    return QObject::tr("%1 分钟").arg(std::max<qint64>(0, ms) / 60000);
}

} // namespace

HourlyBarsWidget::HourlyBarsWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumHeight(84);
    // 画布透明：柱和参考线直接画在卡片底色上。
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);
}

void HourlyBarsWidget::setTotal(const qint64* hourlyMs)
{
    std::copy(hourlyMs, hourlyMs + 24, _totalMs);
    _categoryMode = false;
    releaseCache();
    update();
}

void HourlyBarsWidget::setCategory(const QVector<CategorySlice>& slices)
{
    _slices = slices;
    std::fill(std::begin(_totalMs), std::end(_totalMs), 0);
    for (const CategorySlice& slice : _slices) {
        for (int hour = 0; hour < 24; ++hour) {
            _totalMs[hour] += slice.hourlyMs[hour];
        }
    }
    _categoryMode = true;
    releaseCache();
    update();
}

void HourlyBarsWidget::setCategoryMode(bool categoryMode)
{
    if (_categoryMode == categoryMode) {
        return;
    }
    _categoryMode = categoryMode;
    releaseCache();
    update();
}

void HourlyBarsWidget::clear()
{
    std::fill(std::begin(_totalMs), std::end(_totalMs), 0);
    _slices.clear();
    _hoverHour = -1;
    releaseCache();
    update();
}

qint64 HourlyBarsWidget::cachedBytes() const
{
    // QPixmap 没有 sizeInBytes：按设备像素 × 4（ARGB32）估算，与位图实际占用同量级。
    return _cache.isNull()
        ? 0
        : static_cast<qint64>(_cache.width()) * _cache.height() * 4;
}

QSize HourlyBarsWidget::sizeHint() const
{
    return QSize(320, 110);
}

QSize HourlyBarsWidget::minimumSizeHint() const
{
    return QSize(180, 84);
}

qreal HourlyBarsWidget::plotTop() const
{
    return 8.0;
}

qreal HourlyBarsWidget::plotHeight() const
{
    return std::max<qreal>(10.0, height() - plotTop() - kAxisHeight);
}

int HourlyBarsWidget::hourAt(qreal localX) const
{
    const qreal slot = width() / 24.0;
    if (slot <= 0) {
        return -1;
    }
    const int hour = static_cast<int>(localX / slot);
    return (hour >= 0 && hour < 24) ? hour : -1;
}

void HourlyBarsWidget::renderCache()
{
    const QSize size = this->size();
    if (size.isEmpty()) {
        return;
    }

    const ElaThemeType::ThemeMode mode = eTheme->getThemeMode();
    if (!_cache.isNull() && _cacheSize == size && _cacheMode == static_cast<int>(mode)) {
        return;
    }

    _cache = QPixmap(size * devicePixelRatioF());
    _cache.setDevicePixelRatio(devicePixelRatioF());
    _cache.fill(Qt::transparent);
    _cacheSize = size;
    _cacheMode = static_cast<int>(mode);

    QPainter painter(&_cache);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QColor axisColor = ElaThemeColor(mode, BasicDetailsText);
    const QColor gridColor = ElaThemeColor(mode, BasicBorder);
    const QColor barColor = ElaThemeColor(mode, PrimaryNormal);

    const qreal top = plotTop();
    const qreal plotBottom = top + plotHeight();
    const qreal slot = width() / 24.0;
    const qreal barWidth = std::max<qreal>(2.0, slot * kBarRatio);

    // 参考线：60 分钟顶格线（实线，比例锚点）+ 30 分钟中线（点线）。
    // 柱高 = 分钟数 / 60 × 绘图区高，5 分钟就该是 1/12 高——有了顶格线才可比。
    QPen gridPen(gridColor);
    gridPen.setStyle(Qt::DotLine);
    painter.setPen(gridPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawLine(QPointF(0, plotBottom), QPointF(width(), plotBottom));
    painter.drawLine(QPointF(0, top + plotHeight() * 0.5), QPointF(width(), top + plotHeight() * 0.5));
    // 顶格线：实线但极淡，紧贴绘图区顶端。
    QPen capPen(gridColor);
    painter.setPen(capPen);
    painter.drawLine(QPointF(0, top), QPointF(width(), top));
    QFont refFont = painter.font();
    refFont.setPixelSize(9);
    painter.setFont(refFont);
    painter.setPen(axisColor);
    painter.drawText(QPointF(width() - 24.0, top + 9.0), QStringLiteral("60m"));
    painter.drawText(QPointF(width() - 24.0, top + plotHeight() * 0.5 + 9.0),
                     QStringLiteral("30m"));

    // 柱：高度 = 分钟数 / 60 × 绘图区高，60 分钟 = 顶格。
    // _totalMs 是毫秒：先换算成分钟再乘比例（直接乘毫秒会把所有柱顶到满格）。
    const qreal minuteScale = plotHeight() / 60.0;
    for (int hour = 0; hour < 24; ++hour) {
        const qint64 totalMs = _totalMs[hour];
        if (totalMs <= 0) {
            continue;
        }
        const qreal x = hour * slot + (slot - barWidth) / 2.0;
        const qreal totalMinutes = totalMs / 60000.0;
        // 分钟数夹到 60（跨零点段不会超，但保险起见）。
        const qreal barHeight =
            std::clamp<qreal>(totalMinutes * minuteScale, 1.0, plotHeight());

        if (_categoryMode && !_slices.isEmpty()) {
            // 堆叠：从底往上按 slices 顺序（总时长降序）堆，剩余并入最后一段。
            qreal bottom = plotBottom;
            for (int sliceIndex = 0;
                 sliceIndex < _slices.size() && sliceIndex <= kMaxSlices; ++sliceIndex) {
                const CategorySlice& slice = _slices.at(sliceIndex);
                qreal sliceMs = slice.hourlyMs[hour];
                if (sliceIndex == kMaxSlices) {
                    // 「其他」：该小时总长减去已堆的各类。
                    qint64 known = 0;
                    for (int k = 0; k < kMaxSlices && k < _slices.size(); ++k) {
                        known += _slices.at(k).hourlyMs[hour];
                    }
                    sliceMs = std::max<qint64>(0, totalMs - known);
                }
                if (sliceMs <= 0) {
                    continue;
                }
                const qreal sliceHeight = std::min<qreal>(
                    bottom - top, (sliceMs / 60000.0) * minuteScale);
                painter.setPen(Qt::NoPen);
                painter.setBrush(slice.color);
                painter.drawRect(QRectF(x, bottom - sliceHeight, barWidth, sliceHeight));
                bottom -= sliceHeight;
                if (bottom <= top) {
                    break;
                }
            }
        } else {
            painter.setPen(Qt::NoPen);
            painter.setBrush(barColor);
            painter.drawRoundedRect(QRectF(x, plotBottom - barHeight, barWidth, barHeight), 2, 2);
        }
    }

    // 轴：0/6/12/18/23，间隔更合理（0/4/8/12/16/20/23 不均匀会误导比例）。
    painter.setPen(axisColor);
    QFont font = painter.font();
    font.setPixelSize(10);
    painter.setFont(font);
    for (int hour : {0, 6, 12, 18, 23}) {
        const qreal x = hour * slot + slot / 2.0;
        const QString label = QString::number(hour);
        QFontMetrics metrics(font);
        const qreal textX = std::clamp<qreal>(x - metrics.horizontalAdvance(label) / 2.0,
                                              1.0, width() - metrics.horizontalAdvance(label) - 1.0);
        painter.drawText(QPointF(textX, plotBottom + 13), label);
    }
}

void HourlyBarsWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    renderCache();
    QPainter painter(this);
    painter.drawPixmap(0, 0, _cache);

    // hover：整列高亮 + 柱顶标记 + 浮层，让「指针所在的小时」和「柱子」严格对上。
    if (_hoverHour >= 0) {
        const ElaThemeType::ThemeMode mode = eTheme->getThemeMode();
        const qreal slot = width() / 24.0;
        const qreal top = plotTop();
        const qreal plotBottom = top + plotHeight();
        const qint64 totalMs = _totalMs[_hoverHour];

        // 整列淡高亮（柱与指针所在的小时严格对应）。
        QColor columnColor = ElaThemeColor(mode, PrimaryNormal);
        columnColor.setAlphaF(0.08);
        painter.setPen(Qt::NoPen);
        painter.setBrush(columnColor);
        painter.drawRect(QRectF(_hoverHour * slot, top, slot, plotHeight()));

        QStringList lines;
        lines.append(tr("%1:00 · %2").arg(_hoverHour, 2, 10, QLatin1Char('0'))
                         .arg(formatMinutes(totalMs)));
        if (_categoryMode) {
            qint64 known = 0;
            int shown = 0;
            for (const CategorySlice& slice : _slices) {
                if (shown >= kMaxSlices) {
                    break;
                }
                if (slice.hourlyMs[_hoverHour] > 0) {
                    lines.append(QStringLiteral("%1 %2").arg(slice.label,
                                                             formatMinutes(slice.hourlyMs[_hoverHour])));
                    ++shown;
                }
                known += slice.hourlyMs[_hoverHour];
            }
            const qint64 otherMs = std::max<qint64>(0, totalMs - known);
            if (otherMs > 0 && _slices.size() > kMaxSlices) {
                lines.append(tr("其他 %1").arg(formatMinutes(otherMs)));
            }
        }

        QFont tipFont = painter.font();
        tipFont.setPixelSize(11);
        painter.setFont(tipFont);
        QFontMetrics metrics(tipFont);
        qreal tipWidth = 0;
        for (const QString& line : lines) {
            tipWidth = std::max<qreal>(tipWidth, metrics.horizontalAdvance(line));
        }
        const qreal tipHeight = lines.size() * 16.0 + 8.0;
        qreal tipX = _hoverHour * slot + slot / 2.0 - tipWidth / 2.0 - 6.0;
        tipX = std::clamp<qreal>(tipX, 4.0, width() - tipWidth - 16.0);
        qreal tipY = 2.0;

        const ElaThemeType::ThemeMode tipMode = eTheme->getThemeMode();
        const QColor tipBg = ElaThemeColor(tipMode, BasicText);
        const QColor tipFg = ElaThemeColor(tipMode, DialogBase);
        painter.setPen(Qt::NoPen);
        painter.setBrush(tipBg);
        painter.drawRoundedRect(QRectF(tipX, tipY, tipWidth + 12.0, tipHeight), 6, 6);
        painter.setPen(tipFg);
        for (const QString& line : lines) {
            painter.drawText(QPointF(tipX + 6.0, tipY + 15.0), line);
            tipY += 16.0;
        }
    }
}

void HourlyBarsWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    releaseCache();
}

void HourlyBarsWidget::mouseMoveEvent(QMouseEvent* event)
{
    const int hour = hourAt(event->position().x());
    if (hour != _hoverHour) {
        _hoverHour = hour;
        update();
    }
}

void HourlyBarsWidget::leaveEvent(QEvent* event)
{
    QWidget::leaveEvent(event);
    if (_hoverHour != -1) {
        _hoverHour = -1;
        update();
    }
}

void HourlyBarsWidget::releaseCache()
{
    _cache = QPixmap();
    _cacheSize = QSize();
    _cacheMode = -1;
}

} // namespace patina::ui
