#include "ui/data/trend_line_chart_widget.h"

#include "ElaTheme.h"

#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>

#include <algorithm>
#include <cmath>

namespace patina::ui
{

namespace
{

constexpr qreal kMarginTop = 10.0;
constexpr qreal kMarginRight = 16.0;
constexpr qreal kMarginBottom = 26.0;
constexpr qreal kMarginLeft = 46.0;
constexpr qreal kTickMinWidth = 64.0;
constexpr qreal kDotRadius = 3.0;
constexpr qreal kAreaOpacity = 0.12;
constexpr qreal kStrokeWidth = 2.0;
constexpr int kIntervalCount = 3;
constexpr int kTickFontPixelSize = 10;
constexpr int kTooltipFontPixelSize = 11;
constexpr qreal kTooltipPointGap = 12.0;
constexpr qreal kTooltipEdgeInset = 8.0;
constexpr qreal kTooltipPaddingX = 9.0;
constexpr qreal kTooltipPaddingY = 7.0;
constexpr qreal kTickLabelGap = 8.0;
constexpr qreal kTickLabelTop = 18.0;

struct AxisScale
{
    qreal domainMax = 1.0;
    QVector<qreal> ticks;
};

// 「整齐刻度」算法：先把最大值切成 3 段，再吸附到 1/2/3/4/6/8/10 × 10^n。
AxisScale buildAxis(qreal maxValue, qreal minimumDomain)
{
    AxisScale scale;
    const qreal safeMax = std::max(maxValue, 0.0);
    const qreal raw = std::max(1e-6, safeMax) / kIntervalCount;
    const qreal magnitude = std::pow(10.0, std::floor(std::log10(std::max(raw, 1e-6))));
    const qreal normalized = raw / magnitude;
    const qreal multiples[] = {1.0, 2.0, 3.0, 4.0, 6.0, 8.0, 10.0};
    qreal step = 10.0 * magnitude;
    for (const qreal multiple : multiples)
    {
        if (normalized <= multiple)
        {
            step = multiple * magnitude;
            break;
        }
    }
    scale.domainMax = std::max(step * kIntervalCount, std::ceil(safeMax / step) * step);
    if (minimumDomain > 0.0 && scale.domainMax < minimumDomain)
    {
        scale.domainMax = minimumDomain;
    }
    if (scale.domainMax <= 0.0)
    {
        scale.domainMax = 1.0;
    }
    for (int i = 0; i <= kIntervalCount; ++i)
    {
        scale.ticks.append(scale.domainMax * i / kIntervalCount);
    }
    return scale;
}

// 横轴刻度稀疏化：至少留 64px 一个标签，首尾必画。
QVector<int> tickIndices(int count, qreal plotWidth)
{
    QVector<int> indices;
    if (count <= 0)
    {
        return indices;
    }
    if (count == 1)
    {
        indices.append(0);
        return indices;
    }
    const int visible = std::max(2, std::min(count, static_cast<int>(std::floor(plotWidth / kTickMinWidth)) + 1));
    indices.append(0);
    for (int i = 1; i <= visible - 2; ++i)
    {
        const int index = static_cast<int>(std::lround(static_cast<qreal>(i) * (count - 1) / (visible - 1)));
        if (index > 0 && index < count - 1)
        {
            indices.append(index);
        }
    }
    indices.append(count - 1);
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    return indices;
}

// 相邻点之间用三次贝塞尔过渡，控制点取两点 x 的中点，得到平滑但不越界的曲线。
QPainterPath buildSmoothPath(const QVector<QPointF>& points)
{
    QPainterPath path;
    if (points.isEmpty())
    {
        return path;
    }
    path.moveTo(points.first());
    for (int i = 1; i < points.size(); ++i)
    {
        const QPointF previous = points.at(i - 1);
        const QPointF current = points.at(i);
        const qreal midX = (previous.x() + current.x()) / 2.0;
        path.cubicTo(QPointF(midX, previous.y()), QPointF(midX, current.y()), current);
    }
    return path;
}

} // namespace

TrendLineChartWidget::TrendLineChartWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(280, 140);
    // 画布透明：卡片底色直接透过来，深浅色主题都不出现「图里一块异色底」。
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);
}

void TrendLineChartWidget::setLabels(const QStringList& labels)
{
    if (_labels == labels)
    {
        return;
    }
    _labels = labels;
    releaseCache();
    update();
}

void TrendLineChartWidget::setSeries(const QVector<Series>& series)
{
    _series = series;
    _hoverIndex = -1;
    releaseCache();
    update();
}

void TrendLineChartWidget::setUnitSuffix(const QString& suffix)
{
    if (_unitSuffix == suffix)
    {
        return;
    }
    _unitSuffix = suffix;
    releaseCache();
    update();
}

void TrendLineChartWidget::setValueFormatter(std::function<QString(qreal)> formatter)
{
    _valueFormatter = std::move(formatter);
    update();
}

void TrendLineChartWidget::setMinimumDomain(qreal minimumDomain)
{
    if (qFuzzyCompare(_minimumDomain + 1.0, minimumDomain + 1.0))
    {
        return;
    }
    _minimumDomain = minimumDomain;
    releaseCache();
    update();
}

void TrendLineChartWidget::clear()
{
    _labels.clear();
    _series.clear();
    _hoverIndex = -1;
    releaseCache();
    update();
}

qint64 TrendLineChartWidget::cachedBytes() const
{
    if (_cache.isNull())
    {
        return 0;
    }
    return static_cast<qint64>(_cache.width()) * _cache.height() * _cache.depth() / 8;
}

QSize TrendLineChartWidget::sizeHint() const
{
    return {640, 240};
}

QSize TrendLineChartWidget::minimumSizeHint() const
{
    return {280, 140};
}

int TrendLineChartWidget::pointCount() const
{
    // QVector::size() 是 qsizetype，直接和 int 一起喂 std::max 会推导不出模板参数。
    int count = static_cast<int>(_labels.size());
    for (const Series& series : _series)
    {
        count = std::max(count, static_cast<int>(series.values.size()));
    }
    return count;
}

qreal TrendLineChartWidget::maxValue() const
{
    qreal max = 0.0;
    for (const Series& series : _series)
    {
        for (const qreal value : series.values)
        {
            max = std::max(max, value);
        }
    }
    return max;
}

qreal TrendLineChartWidget::plotLeft() const
{
    return kMarginLeft;
}

qreal TrendLineChartWidget::plotWidth() const
{
    return std::max(1.0, static_cast<qreal>(width()) - kMarginLeft - kMarginRight);
}

qreal TrendLineChartWidget::plotTop() const
{
    return kMarginTop;
}

qreal TrendLineChartWidget::plotHeight() const
{
    return std::max(1.0, static_cast<qreal>(height()) - kMarginTop - kMarginBottom);
}

qreal TrendLineChartWidget::valueAt(int seriesIndex, int pointIndex) const
{
    if (seriesIndex < 0 || seriesIndex >= _series.size())
    {
        return 0.0;
    }
    const QVector<qreal>& values = _series.at(seriesIndex).values;
    if (pointIndex < 0 || pointIndex >= values.size())
    {
        return 0.0;
    }
    return values.at(pointIndex);
}

int TrendLineChartWidget::indexAt(qreal localX) const
{
    const int count = pointCount();
    if (count <= 0)
    {
        return -1;
    }
    if (count == 1)
    {
        return 0;
    }
    const qreal clamped = std::clamp(localX, plotLeft(), plotLeft() + plotWidth());
    const qreal ratio = (clamped - plotLeft()) / plotWidth();
    return std::clamp(static_cast<int>(std::lround(ratio * (count - 1))), 0, count - 1);
}

QString TrendLineChartWidget::formatTick(qreal value) const
{
    if (_unitSuffix == QLatin1String("h"))
    {
        if (value >= 10.0)
        {
            return QString::number(std::round(value), 'f', 0) + _unitSuffix;
        }
        return QString::number(std::round(value * 10.0) / 10.0, 'f', 1) + _unitSuffix;
    }
    return QString::number(std::round(value), 'f', 0) + _unitSuffix;
}

void TrendLineChartWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    if (pointCount() <= 0)
    {
        releaseCache();
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const ElaThemeType::ThemeMode mode = eTheme->getThemeMode();

    if (_cache.isNull() || _cacheSize != size() || _cacheMode != static_cast<int>(mode))
    {
        renderCache();
    }
    if (!_cache.isNull())
    {
        painter.drawPixmap(0, 0, _cache);
    }

    if (_hoverIndex < 0 || _hoverIndex >= pointCount())
    {
        return;
    }

    const int count = pointCount();
    const qreal xAt = count <= 1
                          ? plotLeft() + plotWidth() / 2.0
                          : plotLeft() + plotWidth() * _hoverIndex / (count - 1);

    QPen cursorPen(ElaThemeColor(mode, BasicBorderDeep), 1.0);
    cursorPen.setDashPattern({4.0, 4.0});
    painter.setPen(cursorPen);
    painter.drawLine(QPointF(xAt, plotTop()), QPointF(xAt, plotTop() + plotHeight()));

    QFont tooltipFont = font();
    tooltipFont.setPixelSize(kTooltipFontPixelSize);
    const QFontMetricsF metrics(tooltipFont);
    const qreal lineHeight = metrics.height();

    QStringList lines;
    lines.append(_labels.value(_hoverIndex));
    for (int i = 0; i < _series.size(); ++i)
    {
        const qreal value = valueAt(i, _hoverIndex);
        const QString text = _valueFormatter ? _valueFormatter(value) : formatTick(value);
        const QString name = _series.at(i).name;
        lines.append(name.isEmpty() ? text : name + QStringLiteral(" ") + text);
    }

    qreal boxWidth = 0.0;
    for (const QString& line : lines)
    {
        boxWidth = std::max(boxWidth, metrics.horizontalAdvance(line));
    }
    boxWidth += kTooltipPaddingX * 2.0;
    const qreal boxHeight = lineHeight * lines.size() + kTooltipPaddingY * 2.0;

    const bool onRight = xAt <= static_cast<qreal>(width()) / 2.0;
    const qreal maxLeft = std::max(kTooltipEdgeInset, static_cast<qreal>(width()) - kTooltipEdgeInset - boxWidth);
    qreal boxLeft = onRight ? xAt + kTooltipPointGap : xAt - kTooltipPointGap - boxWidth;
    boxLeft = std::clamp(boxLeft, kTooltipEdgeInset, maxLeft);
    const QRectF box(boxLeft, kTooltipEdgeInset, boxWidth, boxHeight);

    painter.setPen(QPen(ElaThemeColor(mode, PopupBorder), 1.0));
    painter.setBrush(ElaThemeColor(mode, PopupBase));
    painter.drawRoundedRect(box, 8.0, 8.0);

    painter.setFont(tooltipFont);
    qreal textY = box.top() + kTooltipPaddingY + metrics.ascent();
    for (int i = 0; i < lines.size(); ++i)
    {
        painter.setPen(i == 0 ? ElaThemeColor(mode, BasicDetailsText) : ElaThemeColor(mode, BasicText));
        painter.drawText(QPointF(box.left() + kTooltipPaddingX, textY), lines.at(i));
        textY += lineHeight;
    }
}

void TrendLineChartWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    releaseCache();
}

void TrendLineChartWidget::mouseMoveEvent(QMouseEvent* event)
{
    const int index = indexAt(event->position().x());
    if (index != _hoverIndex)
    {
        _hoverIndex = index;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void TrendLineChartWidget::leaveEvent(QEvent* event)
{
    if (_hoverIndex >= 0)
    {
        _hoverIndex = -1;
        update();
    }
    QWidget::leaveEvent(event);
}

void TrendLineChartWidget::renderCache()
{
    const qreal dpr = devicePixelRatioF();
    _cache = QPixmap(size() * dpr);
    _cache.setDevicePixelRatio(dpr);
    _cache.fill(Qt::transparent);
    _cacheSize = size();
    _cacheMode = static_cast<int>(eTheme->getThemeMode());

    QPainter painter(&_cache);
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawStaticLayer(painter);
}

void TrendLineChartWidget::drawStaticLayer(QPainter& painter) const
{
    const int count = pointCount();
    if (count <= 0)
    {
        return;
    }
    const ElaThemeType::ThemeMode mode = eTheme->getThemeMode();
    const AxisScale axis = buildAxis(maxValue(), _minimumDomain);

    const qreal left = plotLeft();
    const qreal top = plotTop();
    const qreal width = plotWidth();
    const qreal height = plotHeight();
    const qreal bottom = top + height;

    QFont tickFont = font();
    tickFont.setPixelSize(kTickFontPixelSize);
    painter.setFont(tickFont);
    const QFontMetricsF metrics(tickFont);

    // y 轴：虚线网格 + 右侧贴轴的数字。
    for (const qreal tick : axis.ticks)
    {
        const qreal y = bottom - tick / axis.domainMax * height;
        QPen gridPen(ElaThemeColor(mode, BasicBorder), 1.0);
        gridPen.setDashPattern({3.0, 3.0});
        painter.setPen(gridPen);
        painter.drawLine(QPointF(left, y), QPointF(left + width, y));

        const QString text = formatTick(tick);
        painter.setPen(ElaThemeColor(mode, BasicDetailsText));
        painter.drawText(QPointF(left - kTickLabelGap - metrics.horizontalAdvance(text), y + metrics.ascent() / 2.0 - 1.0),
                         text);
    }

    // x 轴：只画稀疏化后留下的标签。
    const QVector<int> indices = tickIndices(count, width);
    painter.setPen(ElaThemeColor(mode, BasicDetailsText));
    for (const int index : indices)
    {
        const QString text = _labels.value(index);
        if (text.isEmpty())
        {
            continue;
        }
        const qreal x = count <= 1 ? left + width / 2.0 : left + width * index / (count - 1);
        const qreal textWidth = metrics.horizontalAdvance(text);
        qreal textX = x - textWidth / 2.0;
        if (index == indices.first())
        {
            textX = x;
        }
        else if (index == indices.last())
        {
            textX = x - textWidth;
        }
        painter.drawText(QPointF(textX, bottom + kTickLabelTop), text);
    }

    // 序列：先面积，再折线，最后点，保证点不被线盖住。
    for (const Series& series : _series)
    {
        if (series.values.isEmpty())
        {
            continue;
        }
        const int seriesCount = static_cast<int>(series.values.size());
        QVector<QPointF> points;
        points.reserve(seriesCount);
        for (int i = 0; i < seriesCount; ++i)
        {
            const qreal x = seriesCount <= 1 ? left + width / 2.0 : left + width * i / (seriesCount - 1);
            const qreal value = std::clamp(series.values.at(i), 0.0, axis.domainMax);
            const qreal y = bottom - value / axis.domainMax * height;
            points.append(QPointF(x, y));
        }
        const QPainterPath linePath = buildSmoothPath(points);
        // 调用方没给颜色时用主题主色，避免画出默认的黑色线条。
        const QColor seriesColor = series.color.isValid() ? series.color : ElaThemeColor(mode, PrimaryNormal);

        if (series.fillArea)
        {
            QPainterPath areaPath = linePath;
            areaPath.lineTo(points.last().x(), bottom);
            areaPath.lineTo(points.first().x(), bottom);
            areaPath.closeSubpath();
            QColor areaColor = seriesColor;
            areaColor.setAlphaF(kAreaOpacity);
            painter.setPen(Qt::NoPen);
            painter.setBrush(areaColor);
            painter.drawPath(areaPath);
        }

        QPen linePen(seriesColor, kStrokeWidth);
        linePen.setJoinStyle(Qt::RoundJoin);
        linePen.setCapStyle(Qt::RoundCap);
        painter.setPen(linePen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(linePath);

        if (series.showDots)
        {
            painter.setPen(Qt::NoPen);
            painter.setBrush(seriesColor);
            for (const QPointF& point : points)
            {
                painter.drawEllipse(point, kDotRadius, kDotRadius);
            }
        }
    }
}

void TrendLineChartWidget::releaseCache()
{
    _cache = QPixmap();
    _cacheSize = QSize();
    _cacheMode = -1;
}

} // namespace patina::ui
