#include "ui/history/history_day_band_widget.h"

#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

#include "ElaTheme.h"

namespace
{

constexpr int kBandHeight = 22;
constexpr int kBandRadius = 8;
constexpr qreal kMinBlockWidth = 2.0;

} // namespace

namespace patina::ui
{

HistoryDayBandWidget::HistoryDayBandWidget(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

HistoryDayBandWidget::~HistoryDayBandWidget() = default;

void HistoryDayBandWidget::setBlocks(qint64 dayStartMs, qint64 dayEndMs,
                                     const QVector<Block>& blocks)
{
    _dayStartMs = dayStartMs;
    _dayEndMs = dayEndMs;
    _blocks = blocks;
    update();
}

void HistoryDayBandWidget::clear()
{
    if (_blocks.isEmpty()) {
        return;
    }
    _blocks.clear();
    update();
}

QSize HistoryDayBandWidget::sizeHint() const
{
    return {240, kBandHeight};
}

QSize HistoryDayBandWidget::minimumSizeHint() const
{
    return {120, kBandHeight};
}

void HistoryDayBandWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    if (width() <= 0) {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const ElaThemeType::ThemeMode mode = eTheme->getThemeMode();
    const QRectF trackRect(0, 0, width(), height());

    QPainterPath trackPath;
    trackPath.addRoundedRect(trackRect, kBandRadius, kBandRadius);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ElaThemeColor(mode, BasicChute));
    painter.drawPath(trackPath);

    const qint64 span = _dayEndMs - _dayStartMs;
    if (span <= 0 || _blocks.isEmpty()) {
        return;
    }

    // 色块可以超出圆角，裁到轨道形状里就不会画出圆角外。
    painter.setClipPath(trackPath);
    for (const Block& block : _blocks) {
        const qint64 start = std::max(block.startMs, _dayStartMs);
        const qint64 end = std::min(block.endMs, _dayEndMs);
        if (end <= start) {
            continue;
        }

        const qreal left = static_cast<qreal>(start - _dayStartMs) / static_cast<qreal>(span)
                           * trackRect.width();
        const qreal right = static_cast<qreal>(end - _dayStartMs) / static_cast<qreal>(span)
                            * trackRect.width();
        const qreal blockWidth = std::max(right - left, kMinBlockWidth);

        painter.setBrush(block.color);
        painter.drawRect(QRectF(left, trackRect.top(), blockWidth, trackRect.height()));
    }
}

} // namespace patina::ui
