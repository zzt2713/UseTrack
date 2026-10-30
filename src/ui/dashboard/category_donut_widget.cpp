#include "ui/dashboard/category_donut_widget.h"

#include "ElaTheme.h"

#include "data/repositories.h"

#include <QFont>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace patina::ui
{
namespace
{

// 段与段之间留一点缝，靠视觉分界而不是描边，避免小分片糊成一圈。
constexpr qreal kGapDegrees = 1.4;
constexpr qreal kStrokeWidth = 16.0;
// 圆环外径占控件短边的比例：留出抗锯齿与阴影余量。
constexpr qreal kOuterRatio = 0.86;

QString formatDuration(qint64 ms)
{
    const qint64 totalMinutes = std::max<qint64>(0, ms) / 60000;
    const qint64 hours = totalMinutes / 60;
    const qint64 minutes = totalMinutes % 60;
    if (hours > 0) {
        return QObject::tr("%1 小时 %2 分")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'));
    }
    return QObject::tr("%1 分").arg(minutes);
}

} // namespace

CategoryDonutWidget::CategoryDonutWidget(QWidget* parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setMinimumSize(160, 160);
    // 悬停提示需要接收移动事件（不按下）。
    setMouseTracking(true);
}

void CategoryDonutWidget::setCategories(const QVector<data::CategoryUsage>& categories)
{
    _categories = categories;
    update();
}

void CategoryDonutWidget::setCenterText(const QString& primary, const QString& secondary)
{
    _centerPrimary = primary;
    _centerSecondary = secondary;
    update();
}

void CategoryDonutWidget::clear()
{
    _categories.clear();
    _centerPrimary.clear();
    _centerSecondary.clear();
    _hoverSegment = -1;
    update();
}

QSize CategoryDonutWidget::sizeHint() const
{
    return {200, 200};
}

QSize CategoryDonutWidget::minimumSizeHint() const
{
    return {160, 160};
}

QRectF CategoryDonutWidget::ringRect() const
{
    const int side = std::min(width(), height());
    const QRectF square(
        (width() - side) / 2.0, (height() - side) / 2.0, side, side);
    const qreal radius = side * kOuterRatio / 2.0 - kStrokeWidth / 2.0;
    return square.adjusted(
        square.width() / 2.0 - radius, square.height() / 2.0 - radius,
        -(square.width() / 2.0 - radius), -(square.height() / 2.0 - radius));
}

int CategoryDonutWidget::segmentAt(const QPointF& position) const
{
    if (_categories.isEmpty()) {
        return -1;
    }
    const QRectF ring = ringRect();
    const QPointF center = ring.center();
    const QPointF delta = position - center;
    const qreal distance = std::hypot(delta.x(), delta.y());
    const qreal outer = ring.width() / 2.0 + kStrokeWidth / 2.0;
    const qreal inner = ring.width() / 2.0 - kStrokeWidth / 2.0;
    // 环带之外（环心是文字区，不算命中）。
    if (distance > outer || distance < inner) {
        return -1;
    }

    qint64 total = 0;
    for (const data::CategoryUsage& item : _categories) {
        total += std::max<qint64>(0, item.durationMs);
    }
    if (total <= 0) {
        return -1;
    }

    // 绘制约定：12 点方向起顺时针铺。Qt 屏幕坐标 y 向下，atan2 直接给出
    // 「从 +x 轴逆时针」的角度，换算成「从 12 点顺时针」的角度：
    //   clockwiseFromNorth = 90° - atan2Degrees（再规整到 [0,360)）。
    qreal angle = 90.0 - qRadiansToDegrees(std::atan2(-delta.y(), delta.x()));
    angle = std::fmod(angle, 360.0);
    if (angle < 0) {
        angle += 360.0;
    }

    qreal consumed = 0.0;
    for (int index = 0; index < _categories.size(); ++index) {
        const qint64 value = std::max<qint64>(0, _categories.at(index).durationMs);
        if (value <= 0) {
            continue;
        }
        const qreal span = 360.0 * static_cast<qreal>(value) / static_cast<qreal>(total);
        if (angle >= consumed && angle < consumed + span) {
            return index;
        }
        consumed += span;
    }
    return -1;
}

void CategoryDonutWidget::mouseMoveEvent(QMouseEvent* event)
{
    const int segment = segmentAt(event->position());
    if (segment != _hoverSegment) {
        _hoverSegment = segment;
        update();
    }
    if (segment >= 0) {
        const data::CategoryUsage& item = _categories.at(segment);
        QToolTip::showText(event->globalPosition().toPoint(),
                           tr("%1 · %2").arg(item.label, formatDuration(item.durationMs)),
                           this);
    } else {
        QToolTip::hideText();
    }
}

void CategoryDonutWidget::leaveEvent(QEvent* event)
{
    QWidget::leaveEvent(event);
    if (_hoverSegment != -1) {
        _hoverSegment = -1;
        update();
    }
    QToolTip::hideText();
}

void CategoryDonutWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const ElaThemeType::ThemeMode mode = eTheme->getThemeMode();
    const QRectF ringRect = this->ringRect();

    // 底环：没有数据时它就是唯一的内容。
    QPen trackPen(ElaThemeColor(mode, BasicChute), kStrokeWidth);
    trackPen.setCapStyle(Qt::FlatCap);
    painter.setPen(trackPen);
    painter.drawEllipse(ringRect);

    qint64 total = 0;
    for (const data::CategoryUsage& item : _categories) {
        total += std::max<qint64>(0, item.durationMs);
    }

    if (total > 0) {

        const qreal kHoverExtraWidth = 6.0;
        const qreal kHoverExpand = 4.0;

        qreal consumed = 0.0;
        for (int index = 0; index < _categories.size(); ++index) {
            const data::CategoryUsage& item = _categories.at(index);
            const qint64 value = std::max<qint64>(0, item.durationMs);
            if (value <= 0) continue;

            const qreal span = 360.0 * static_cast<qreal>(value) / static_cast<qreal>(total);
            const qreal drawn = std::max<qreal>(0.0, span - kGapDegrees);

            if (drawn > 0.0) {
                QColor segmentColor(item.colorHex);

                qreal currentPenWidth = kStrokeWidth;
                QRectF currentRect = ringRect;

                if (index == _hoverSegment) {
                    segmentColor = QColor(255, 255, 255, 30);

                    QColor originalColor(item.colorHex);
                    int r = originalColor.red() + (255 - originalColor.red()) * 0.15;
                    int g = originalColor.green() + (255 - originalColor.green()) * 0.15;
                    int b = originalColor.blue() + (255 - originalColor.blue()) * 0.15;
                    segmentColor = QColor(r, g, b);

                    currentPenWidth += kHoverExtraWidth;
                    currentRect.adjust(-kHoverExpand, -kHoverExpand, kHoverExpand, kHoverExpand);
                }

                QPen segmentPen(segmentColor, currentPenWidth);
                segmentPen.setCapStyle(Qt::FlatCap);
                painter.setPen(segmentPen);

                const int startAngle = static_cast<int>(std::lround((90.0 - consumed) * 16.0));
                const int spanAngle = static_cast<int>(std::lround(-drawn * 16.0));

                painter.drawArc(currentRect, startAngle, spanAngle);
            }
            consumed += span;
        }
    }

    // 中心文字：上行是大号总时长，下行是说明。
    if (!_centerPrimary.isEmpty()) {
        const int side = std::min(width(), height());
        const QRectF center((width() - side) / 2.0, (height() - side) / 2.0, side, side);
        QFont primaryFont = font();
        primaryFont.setPixelSize(std::max(16, static_cast<int>(side * 0.115)));
        primaryFont.setWeight(QFont::DemiBold);
        painter.setFont(primaryFont);
        painter.setPen(ElaThemeColor(mode, BasicText));
        const QRectF primaryRect(center.left(), center.top() + center.height() * 0.40,
                                 center.width(), primaryFont.pixelSize() * 1.5);
        painter.drawText(primaryRect, Qt::AlignHCenter | Qt::AlignTop, _centerPrimary);

        if (!_centerSecondary.isEmpty()) {
            QFont secondaryFont = font();
            secondaryFont.setPixelSize(std::max(10, static_cast<int>(side * 0.062)));
            painter.setFont(secondaryFont);
            painter.setPen(ElaThemeColor(mode, BasicDetailsText));
            const QRectF secondaryRect(center.left(), center.top() + center.height() * 0.56,
                                       center.width(), secondaryFont.pixelSize() * 1.6);
            painter.drawText(secondaryRect, Qt::AlignHCenter | Qt::AlignTop, _centerSecondary);
        }
    }
}

} // namespace patina::ui
