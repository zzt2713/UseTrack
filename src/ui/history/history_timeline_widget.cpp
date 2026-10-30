#include "ui/history/history_timeline_widget.h"

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

// —— Lanes（缩放对话框里的分轨明细）——
constexpr qreal kLaneHeight = 24.0;
constexpr qreal kLaneGap = 8.0;
// 左列放色点 + 应用名，右列放该 lane 全天累计时长：「标识—轨道—时长」
// 三段式，轨道不再顶到右边界。
constexpr qreal kTrackLeft = 140.0;
constexpr qreal kTrackRight = 52.0;
constexpr qreal kLabelFontPx = 12;
constexpr qreal kTickFontPx = 10;
constexpr qreal kAxisHeight = 24.0;
constexpr qreal kDotRadius = 4.0;
// 最小可见宽度：24h 视口里几百毫秒的段本来就只有零点几像素，
// 压到 2px 时圆角吃掉了整个色块，看不出是彩色的。
constexpr qreal kMinSegmentWidth = 3.0;

// —— Ribbon（主视图单轨色带）——
// 轨道 48px、色块 34px、圆角 6px、刻度在轨道下方。
constexpr qreal kRibbonTrackHeight = 48.0;
constexpr qreal kRibbonSegmentHeight = 34.0;
constexpr qreal kRibbonAxisHeight = 20.0;
constexpr qreal kRibbonPadding = 1.0;
constexpr qreal kRibbonSegmentRadius = 6.0;
// 色带里的段首尾相接，最小宽度只要保证「看得见」。这里给 1px 而不是 2px：
// 24h 视口下一像素约等于两分钟，撑到 2px 就等于把一次一分钟的切换画成四分钟，
// 又回到「时间对不上」的老问题上了。真正碎的时间由合并阈值处理。
constexpr qreal kRibbonMinSegmentWidth = 1.0;

// lane 累计时长的短格式（右列）：超过一小时给「1h 23m」，否则「23m」。
QString formatLaneDuration(qint64 durationMs)
{
    const qint64 totalMinutes = std::max<qint64>(0, durationMs) / 60000;
    if (totalMinutes >= 60) {
        return QStringLiteral("%1h %2m").arg(totalMinutes / 60).arg(totalMinutes % 60);
    }
    if (totalMinutes > 0) {
        return QStringLiteral("%1m").arg(totalMinutes);
    }
    return durationMs > 0 ? QStringLiteral("<1m") : QStringLiteral("0m");
}

struct AxisTick
{
    qreal ratio = 0.0;
    QString label;
};

// 刻度间隔取「第一个 >= 视口时长 / 4」的档位。
// 24h → 6h（0/6/12/18/24 五个刻度），4h 视口 → 1h。
qint64 axisStepMs(qint64 spanMs)
{
    static const qint64 kIntervals[] = {
        5 * 60 * 1000LL,     10 * 60 * 1000LL,    15 * 60 * 1000LL,
        30 * 60 * 1000LL,    60 * 60 * 1000LL,    2 * 60 * 60 * 1000LL,
        3 * 60 * 60 * 1000LL, 6 * 60 * 60 * 1000LL,
    };
    const qint64 quarter = std::max<qint64>(1, spanMs / 4);
    for (const qint64 candidate : kIntervals) {
        if (candidate >= quarter) {
            return candidate;
        }
    }
    return kIntervals[sizeof(kIntervals) / sizeof(kIntervals[0]) - 1];
}

QString tickLabel(qint64 timeMs, qint64 viewportEndMs)
{
    const QDateTime moment = QDateTime::fromMSecsSinceEpoch(timeMs);
    // 视口正好结束在午夜时，标成 00:00 会被读成「这一天的开始」，这里写 24:00。
    if (timeMs == viewportEndMs && moment.time().hour() == 0 && moment.time().minute() == 0) {
        return QStringLiteral("24:00");
    }
    return moment.toString(QStringLiteral("HH:mm"));
}

// 首尾各一个刻度（贴边不溢出），中间按步长铺，离两端太近的跳过——
// 这样首尾刻度不会被裁掉。
QVector<AxisTick> buildAxisTicks(qint64 startMs, qint64 endMs)
{
    QVector<AxisTick> ticks;
    const qint64 span = std::max<qint64>(1, endMs - startMs);
    ticks.append(AxisTick{0.0, tickLabel(startMs, endMs)});
    const qint64 step = axisStepMs(span);
    for (qint64 timeMs = startMs + step; timeMs < endMs; timeMs += step) {
        const qreal ratio = static_cast<qreal>(timeMs - startMs) / static_cast<qreal>(span);
        if (ratio < 0.05 || 1.0 - ratio < 0.05) {
            continue;
        }
        ticks.append(AxisTick{ratio, tickLabel(timeMs, endMs)});
    }
    ticks.append(AxisTick{1.0, tickLabel(endMs, endMs)});
    return ticks;
}

} // namespace

HistoryTimelineWidget::HistoryTimelineWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    // 画布透明：色块直接画在卡片底色上。
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);
}

void HistoryTimelineWidget::setSegments(qint64 viewportStartMs, qint64 viewportEndMs,
                                        const QVector<TimelineLaneSegment>& segments)
{
    _mode = Mode::Ribbon;
    _viewportStartMs = viewportStartMs;
    _viewportEndMs = viewportEndMs;
    _segments = segments;
    _lanes.clear();
    _hoverLane = -1;
    _hoverSegment = -1;
    releaseCache();
    updateGeometry();
    update();
}

void HistoryTimelineWidget::setLanes(qint64 viewportStartMs, qint64 viewportEndMs,
                                     const QVector<TimelineLane>& lanes)
{
    _mode = Mode::Lanes;
    _viewportStartMs = viewportStartMs;
    _viewportEndMs = viewportEndMs;
    _lanes = lanes;
    _segments.clear();
    _hoverLane = -1;
    _hoverSegment = -1;
    releaseCache();
    updateGeometry();
    update();
}

void HistoryTimelineWidget::clear()
{
    _lanes.clear();
    _segments.clear();
    _hoverLane = -1;
    _hoverSegment = -1;
    releaseCache();
    updateGeometry();
    update();
}

qint64 HistoryTimelineWidget::cachedBytes() const
{
    // QPixmap 没有 sizeInBytes：按设备像素 × 4（ARGB32）估算。
    return _cache.isNull() ? 0 : static_cast<qint64>(_cache.width()) * _cache.height() * 4;
}

QSize HistoryTimelineWidget::sizeHint() const
{
    if (_mode == Mode::Ribbon) {
        const qreal height = kRibbonTrackHeight + kRibbonAxisHeight + 2 * kRibbonPadding + 6.0;
        return QSize(600, static_cast<int>(height));
    }
    const qreal height = kAxisHeight + _lanes.size() * (kLaneHeight + kLaneGap) + 8.0;
    return QSize(600, static_cast<int>(height));
}

QSize HistoryTimelineWidget::minimumSizeHint() const
{
    if (_mode == Mode::Ribbon) {
        const qreal height = kRibbonTrackHeight + kRibbonAxisHeight + 2 * kRibbonPadding + 6.0;
        return QSize(280, static_cast<int>(height));
    }
    // 下限跟着 lane 数走：原来固定按 2 条算，5 条时下方的活动流会把时间轴
    // 压到只露两条半——用户看到的就是「时间轴只显示两个应用」。
    const int lanes = std::max(1, static_cast<int>(_lanes.size()));
    const qreal height = kAxisHeight + lanes * (kLaneHeight + kLaneGap) + 8.0;
    return QSize(360, static_cast<int>(height));
}

qreal HistoryTimelineWidget::trackWidthFor() const
{
    return std::max<qreal>(1.0, width() - kTrackLeft - kTrackRight);
}

QRectF HistoryTimelineWidget::trackRect() const
{
    if (_mode == Mode::Ribbon) {
        // 主视图没有左右标签列，色带占满整宽（留 1px 给描边）。
        return QRectF(kRibbonPadding, kRibbonPadding,
                      std::max<qreal>(1.0, width() - 2 * kRibbonPadding), kRibbonTrackHeight);
    }
    // Lanes 模式不画整条色轨，这里只给「双击换算时间」提供一个水平映射范围。
    return QRectF(kTrackLeft, 0, trackWidthFor(), std::max<qreal>(1.0, height()));
}

QRectF HistoryTimelineWidget::laneTrackRect(int lane) const
{
    const qreal y = kAxisHeight + lane * (kLaneHeight + kLaneGap);
    return QRectF(kTrackLeft, y, trackWidthFor(), kLaneHeight);
}

HistoryTimelineWidget::Hit HistoryTimelineWidget::hitAt(const QPointF& position) const
{
    Hit hit;
    const qreal span = static_cast<qreal>(std::max<qint64>(1, _viewportEndMs - _viewportStartMs));

    if (_mode == Mode::Ribbon) {
        const QRectF track = trackRect();
        if (!track.contains(position)) {
            return hit;
        }
        hit.lane = 0;
        const qreal ratio =
            std::clamp((position.x() - track.left()) / track.width(), 0.0, 1.0);
        const qint64 timeMs = _viewportStartMs + static_cast<qint64>(ratio * span);
        for (int i = 0; i < _segments.size(); ++i) {
            const TimelineLaneSegment& item = _segments.at(i);
            if (timeMs >= item.startMs && timeMs < item.endMs) {
                hit.segment = i;
                break;
            }
        }
        return hit;
    }

    for (int lane = 0; lane < _lanes.size(); ++lane) {
        const QRectF track = laneTrackRect(lane);
        if (!track.contains(position)) {
            continue;
        }
        hit.lane = lane;
        const qreal ratio = (position.x() - track.left()) / track.width();
        const qint64 timeMs = _viewportStartMs + static_cast<qint64>(ratio * span);
        for (int segment = 0; segment < _lanes.at(lane).segments.size(); ++segment) {
            const TimelineLaneSegment& item = _lanes.at(lane).segments.at(segment);
            if (timeMs >= item.startMs && timeMs <= item.endMs) {
                hit.segment = segment;
                break;
            }
        }
        break;
    }
    return hit;
}

QRectF HistoryTimelineWidget::segmentRect(const QRectF& track,
                                          const TimelineLaneSegment& segment, qreal span) const
{
    // 先与视口求交：缩放对话框的视口只覆盖几小时，全天段直接按原始坐标换算
    // 会算出负的 left，被最小宽度挤成标签列里的 2px 竖条。
    const qint64 visibleStartMs = std::max(segment.startMs, _viewportStartMs);
    const qint64 visibleEndMs = std::min(segment.endMs, _viewportEndMs);
    if (visibleEndMs <= visibleStartMs) {
        return QRectF();
    }
    const qreal left = track.left()
        + track.width() * (visibleStartMs - _viewportStartMs) / span;
    const qreal right = track.left()
        + track.width() * (visibleEndMs - _viewportStartMs) / span;
    const bool ribbon = _mode == Mode::Ribbon;
    const qreal height = ribbon ? kRibbonSegmentHeight : kLaneHeight - 4.0;
    const qreal minWidth = ribbon ? kRibbonMinSegmentWidth : kMinSegmentWidth;
    return QRectF(left, track.center().y() - height / 2.0,
                  std::max<qreal>(minWidth, right - left), height);
}

void HistoryTimelineWidget::renderCache()
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

    if (_mode == Mode::Ribbon) {
        renderRibbon(painter);
    } else {
        renderLanes(painter);
    }
}

void HistoryTimelineWidget::renderRibbon(QPainter& painter)
{
    const ElaThemeType::ThemeMode mode = eTheme->getThemeMode();
    const QColor borderColor = ElaThemeColor(mode, BasicBorder);
    const QColor tickColor = ElaThemeColor(mode, BasicDetailsText);

    const QRectF track = trackRect();
    const qreal span = static_cast<qreal>(std::max<qint64>(1, _viewportEndMs - _viewportStartMs));

    // 轨道底色 + 描边：一整天里没有记录的时段也得看得见边界，
    // 否则色带断开的地方会被当成「数据丢了」。
    QColor trackFill = borderColor;
    trackFill.setAlpha(mode == ElaThemeType::ThemeMode::Dark ? 30 : 22);
    painter.setPen(Qt::NoPen);
    painter.setBrush(trackFill);
    painter.drawRoundedRect(track, kRibbonSegmentRadius, kRibbonSegmentRadius);

    QColor borderStroke = borderColor;
    borderStroke.setAlpha(mode == ElaThemeType::ThemeMode::Dark ? 90 : 70);
    painter.setPen(QPen(borderStroke, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(track.adjusted(0.5, 0.5, -0.5, -0.5),
                            kRibbonSegmentRadius, kRibbonSegmentRadius);

    // 色块：每个时刻只有一个颜色，所以这里不会有重叠。
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (const TimelineLaneSegment& segment : _segments) {
        const QRectF rect = segmentRect(track, segment, span);
        if (rect.isEmpty()) {
            continue;
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(segment.color);
        // 圆角不能超过块宽的一半：1px 的窄块配 6px 圆角会画成一根两头尖的
        // 竖线，一天下来色带看着像虚线。窄块直接用直角拼起来才连成一条。
        const qreal radius = std::min(kRibbonSegmentRadius, rect.width() / 2.0);
        painter.drawRoundedRect(rect, radius, radius);
        if (segment.isLive) {
            QPen livePen(segment.color);
            livePen.setStyle(Qt::DashLine);
            livePen.setWidthF(1.5);
            painter.setPen(livePen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(rect.adjusted(-1, -1, 1, 1),
                                    radius + 1, radius + 1);
        }
    }

    // 刻度：画在轨道下方，首刻度贴左、末刻度贴右，中间居中。
    QFont tickFont = painter.font();
    tickFont.setPixelSize(static_cast<int>(kTickFontPx));
    painter.setFont(tickFont);
    painter.setPen(tickColor);
    const QFontMetrics metrics(tickFont);
    const qreal baseline = track.bottom() + kRibbonAxisHeight - 5.0;
    for (const AxisTick& tick : buildAxisTicks(_viewportStartMs, _viewportEndMs)) {
        const qreal x = track.left() + track.width() * tick.ratio;
        const qreal labelWidth = metrics.horizontalAdvance(tick.label);
        qreal textX = x - labelWidth / 2.0;
        if (tick.ratio <= 0.0) {
            textX = x;
        } else if (tick.ratio >= 1.0) {
            textX = x - labelWidth;
        }
        textX = std::clamp<qreal>(textX, 0.0,
                                  std::max<qreal>(0.0, width() - labelWidth));
        painter.drawText(QPointF(textX, baseline), tick.label);
    }
}

void HistoryTimelineWidget::renderLanes(QPainter& painter)
{
    const ElaThemeType::ThemeMode mode = eTheme->getThemeMode();
    const QColor labelColor = ElaThemeColor(mode, BasicText);
    const QColor tickColor = ElaThemeColor(mode, BasicDetailsText);
    const QColor gridColor = ElaThemeColor(mode, BasicBorder);

    QFont laneFont = painter.font();
    laneFont.setPixelSize(static_cast<int>(kLabelFontPx));
    const QFontMetrics laneMetrics(laneFont);

    const qreal span = static_cast<qreal>(std::max<qint64>(1, _viewportEndMs - _viewportStartMs));
    const qreal trackWidth = trackWidthFor();

    // 背景：每 lane 只画一条极淡的中线 + 基线，不铺色块（暗色下满宽灰底很突兀）。
    QPen gridPen(gridColor);
    gridPen.setStyle(Qt::DotLine);
    painter.setPen(gridPen);
    painter.setBrush(Qt::NoBrush);
    const qreal gridTop = kAxisHeight + kLaneHeight / 2.0;
    const qreal gridBottom = kAxisHeight
        + std::max<qsizetype>(1, _lanes.size()) * (kLaneHeight + kLaneGap) - kLaneGap
        - kLaneHeight / 2.0;
    if (_lanes.size() > 1) {
        painter.drawLine(QPointF(kTrackLeft, gridTop), QPointF(kTrackLeft + trackWidth, gridTop));
        painter.drawLine(QPointF(kTrackLeft, gridBottom),
                         QPointF(kTrackLeft + trackWidth, gridBottom));
    }

    for (int lane = 0; lane < _lanes.size(); ++lane) {
        const TimelineLane& laneData = _lanes.at(lane);
        const QRectF track = laneTrackRect(lane);

        // 左列：色点 + 应用名（色点让「哪条 lane 是什么颜色」
        // 一眼对上，右侧色块再密也不用去猜）。
        const qreal dotX = 8.0 + kDotRadius;
        painter.setPen(Qt::NoPen);
        painter.setBrush(laneData.color);
        painter.drawEllipse(QPointF(dotX, track.center().y()), kDotRadius, kDotRadius);

        painter.setFont(laneFont);
        painter.setPen(labelColor);
        const qreal labelLeft = dotX + kDotRadius + 8.0;
        const QString elided = laneMetrics.elidedText(
            laneData.label, Qt::ElideRight,
            static_cast<int>(kTrackLeft - labelLeft - 8.0));
        painter.drawText(QPointF(labelLeft, track.center().y() + laneMetrics.ascent() / 2.0),
                         elided);

        for (const TimelineLaneSegment& segment : laneData.segments) {
            const QRectF rect = segmentRect(track, segment, span);
            if (rect.isEmpty()) {
                continue;
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(segment.color);
            painter.drawRoundedRect(rect, 4, 4);
            if (segment.isLive) {
                QPen livePen(segment.color);
                livePen.setStyle(Qt::DashLine);
                livePen.setWidthF(1.5);
                painter.setPen(livePen);
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(rect.adjusted(-1, -1, 1, 1), 5, 5);
            }
        }

        // 右列：该 lane 全天累计时长，
        // 让「谁占比大」不用去数色块。
        painter.setFont(laneFont);
        painter.setPen(tickColor);
        const QString durationText = formatLaneDuration(laneData.totalDurationMs);
        const qreal durationWidth = laneMetrics.horizontalAdvance(durationText);
        painter.drawText(QPointF(width() - 10.0 - durationWidth,
                                 track.center().y() + laneMetrics.ascent() / 2.0),
                         durationText);
    }

    // 时间刻度：按视口时长选步长（24h → 4h，≤6h → 1h）。
    QFont tickFont = painter.font();
    tickFont.setPixelSize(static_cast<int>(kTickFontPx));
    painter.setFont(tickFont);
    painter.setPen(tickColor);
    const qint64 viewportHours = static_cast<qint64>(span) / 3600000;
    const int stepHours = viewportHours > 12 ? 4 : viewportHours > 3 ? 2 : 1;
    const QDateTime startMoment = QDateTime::fromMSecsSinceEpoch(_viewportStartMs);
    for (int hour = 0; hour <= viewportHours; hour += stepHours) {
        const qint64 tickMs = _viewportStartMs + hour * 3600000LL;
        const qreal x = kTrackLeft + trackWidth * (tickMs - _viewportStartMs) / span;
        const QString label = startMoment.addSecs(hour * 3600).toString(QStringLiteral("HH:mm"));
        const QFontMetrics metrics(tickFont);
        const qreal labelWidth = metrics.horizontalAdvance(label);
        // 先夹到色轨两端；轨道窄于文字时右端会小于左端，再兜一次免得 clamp 收到反序区间。
        const qreal maxTextX = kTrackLeft + trackWidth - labelWidth;
        const qreal textX = maxTextX <= kTrackLeft
            ? kTrackLeft
            : std::clamp<qreal>(x - labelWidth / 2.0, kTrackLeft, maxTextX);
        painter.drawText(QPointF(textX, kAxisHeight - 8.0), label);
    }
}

void HistoryTimelineWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    renderCache();
    QPainter painter(this);
    painter.drawPixmap(0, 0, _cache);

    // hover 描边（浮层，直接画在缓存上）。
    if (_hoverSegment < 0) {
        return;
    }
    const TimelineLaneSegment* segment = nullptr;
    QRectF track;
    if (_mode == Mode::Ribbon) {
        if (_hoverSegment < _segments.size()) {
            segment = &_segments.at(_hoverSegment);
            track = trackRect();
        }
    } else if (_hoverLane >= 0 && _hoverLane < _lanes.size()) {
        const TimelineLane& lane = _lanes.at(_hoverLane);
        if (_hoverSegment < lane.segments.size()) {
            segment = &lane.segments.at(_hoverSegment);
            track = laneTrackRect(_hoverLane);
        }
    }
    if (segment == nullptr) {
        return;
    }
    const qreal span = static_cast<qreal>(std::max<qint64>(1, _viewportEndMs - _viewportStartMs));
    const QRectF rect = segmentRect(track, *segment, span);
    // 视口外的段不描边：否则高亮框会落在左侧标签列上。
    if (rect.isEmpty()) {
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen hoverPen(ElaThemeColor(eTheme->getThemeMode(), PrimaryNormal));
    hoverPen.setWidthF(2.0);
    painter.setPen(hoverPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect.adjusted(-1, -1, 1, 1), 5, 5);
}

void HistoryTimelineWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    releaseCache();
}

void HistoryTimelineWidget::mouseMoveEvent(QMouseEvent* event)
{
    const Hit hit = hitAt(event->position());
    if (hit.lane != _hoverLane || hit.segment != _hoverSegment) {
        _hoverLane = hit.lane;
        _hoverSegment = hit.segment;
        update();
    }
}

void HistoryTimelineWidget::mousePressEvent(QMouseEvent* event)
{
    const Hit hit = hitAt(event->position());
    if (hit.segment >= 0) {
        if (_mode == Mode::Ribbon) {
            if (hit.segment < _segments.size()) {
                emit segmentClicked(_segments.at(hit.segment));
                return;
            }
        } else if (hit.lane >= 0 && hit.lane < _lanes.size()) {
            const TimelineLane& lane = _lanes.at(hit.lane);
            if (hit.segment < lane.segments.size()) {
                emit segmentClicked(lane.segments.at(hit.segment));
                return;
            }
        }
    }
    QWidget::mousePressEvent(event);
}

void HistoryTimelineWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    const qreal span = static_cast<qreal>(std::max<qint64>(1, _viewportEndMs - _viewportStartMs));
    const QRectF track = trackRect();
    qint64 focusMs = _viewportStartMs;
    if (track.width() > 0) {
        focusMs += static_cast<qint64>(
            std::clamp((event->position().x() - track.left()) / track.width(), 0.0, 1.0) * span);
    }
    emit zoomRequested(focusMs);
}

void HistoryTimelineWidget::leaveEvent(QEvent* event)
{
    QWidget::leaveEvent(event);
    if (_hoverLane != -1 || _hoverSegment != -1) {
        _hoverLane = -1;
        _hoverSegment = -1;
        update();
    }
}

void HistoryTimelineWidget::releaseCache()
{
    _cache = QPixmap();
    _cacheSize = QSize();
    _cacheMode = -1;
}

} // namespace patina::ui
