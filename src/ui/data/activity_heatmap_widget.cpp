#include "ui/data/activity_heatmap_widget.h"

#include "ElaTheme.h"

#include <QDateTime>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace patina::ui
{

namespace
{

constexpr qreal kGap = 3.0;
constexpr qreal kLeftPad = 26.0;
constexpr qreal kTopPad = 15.0;
// 底部图例带：热力格画完之后，下面留一条「少 □▢▣▤ 多」的高度。
constexpr qreal kBottomLegendBand = 18.0;
constexpr qreal kMaxCellSize = 16.0;
constexpr qreal kMinCellSize = 4.0;
constexpr qreal kCellRadius = 2.5;
constexpr int kRows = 7;
constexpr int kLabelFontPixelSize = 10;
// 月份标签之间的最小水平距离：列密的时候每周都给标签会糊成一片。
constexpr qreal kMonthLabelMinGap = 30.0;
// GitHub 那种分档：不是连续透明度，而是四档色阶，扫一眼就能数出深浅层次。
// 档位边界取绝对时长而不是区间峰值：只用了五分钟的那天被拉成最深一档，
// 会让人以为自己用了一整天。四档 = <30 分钟 / 30 分钟–2 小时 / 2–5 小时 / ≥5 小时。
constexpr int kLevelCount = 4;
constexpr qreal kLevelAlpha[kLevelCount] = {0.22, 0.45, 0.72, 1.0};
constexpr qint64 kLevelThresholdMs[kLevelCount - 1] = {
    30 * 60 * 1000LL,
    2 * 3600 * 1000LL,
    5 * 3600 * 1000LL,
};

// 把时长映射成 1..kLevelCount 的档位。
// 有记录就至少是第 1 档：连续透明度在低值区几乎看不出差别，
// 分档之后「这天到底动没动」一眼就能分辨。
int levelFor(qint64 durationMs)
{
    if (durationMs <= 0)
    {
        return 0;
    }
    for (int level = 1; level < kLevelCount; ++level)
    {
        if (durationMs < kLevelThresholdMs[level - 1])
        {
            return level;
        }
    }
    return kLevelCount;
}

const QStringList& weekdayLabels()
{
    static const QStringList labels = {QStringLiteral("一"), QStringLiteral("二"), QStringLiteral("三"),
                                       QStringLiteral("四"), QStringLiteral("五"), QStringLiteral("六"),
                                       QStringLiteral("日")};
    return labels;
}

} // namespace

ActivityHeatmapWidget::ActivityHeatmapWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(240, 120);
    // 画布透明：空白格仍用 BasicChute 画出来（要能辨认「无记录」），
    // 控件自身不再垫一层底色。
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);
}

void ActivityHeatmapWidget::setDays(const QVector<data::TrendDay>& days, int maxWeeks)
{
    rebuildWeeks(days, maxWeeks);
    _hoverWeek = -1;
    _hoverRow = -1;
    releaseCache();
    update();
}

void ActivityHeatmapWidget::clear()
{
    _weeks.clear();
    _hoverWeek = -1;
    _hoverRow = -1;
    releaseCache();
    update();
}

qint64 ActivityHeatmapWidget::cachedBytes() const
{
    if (_cache.isNull())
    {
        return 0;
    }
    return static_cast<qint64>(_cache.width()) * _cache.height() * _cache.depth() / 8;
}

QSize ActivityHeatmapWidget::sizeHint() const
{
    return {640, 160};
}

QSize ActivityHeatmapWidget::minimumSizeHint() const
{
    return {240, 120};
}

void ActivityHeatmapWidget::rebuildWeeks(const QVector<data::TrendDay>& days, int maxWeeks)
{
    _weeks.clear();
    if (days.isEmpty())
    {
        return;
    }

    // 先铺成连续的天列表，再在头部补空位让第一列从周一开始。
    QVector<Cell> flat;
    flat.reserve(days.size() + kRows);
    const QDate firstDate = QDateTime::fromMSecsSinceEpoch(days.first().dayStartMs).date();
    const int leading = (firstDate.dayOfWeek() + 6) % 7;  // dayOfWeek(): 周一=1 → 0
    for (int i = 0; i < leading; ++i)
    {
        flat.append(Cell());
    }
    for (const data::TrendDay& day : days)
    {
        Cell cell;
        cell.valid = true;
        cell.hasRecord = day.durationMs > 0;
        cell.durationMs = day.durationMs;
        cell.date = QDateTime::fromMSecsSinceEpoch(day.dayStartMs).date();
        flat.append(cell);
    }
    while (flat.size() % kRows != 0)
    {
        flat.append(Cell());
    }

    // 月份标签只在该周跨到新的月份时给，避免每列都重复同一个「3月」。
    //
    // 注意必须跟踪「最近一次实际赋值的月份」：读 _weeks.last().monthLabel
    // 不行——上一列标签可能因「与上上月相同」被置空，本列拿空串一比就误判
    // 成「换了月份」，结果每个月份隔列重复出现。
    QString lastAssignedMonth;
    for (int i = 0; i < flat.size(); i += kRows)
    {
        Week week;
        week.cells = flat.mid(i, kRows);

        QString label;
        for (const Cell& cell : week.cells)
        {
            if (cell.valid)
            {
                const QString current = QString::number(cell.date.month()) + QStringLiteral("月");
                if (current != lastAssignedMonth)
                {
                    label = current;
                    lastAssignedMonth = current;
                }
                break;
            }
        }
        week.monthLabel = label;
        _weeks.append(week);
    }

    if (maxWeeks > 0 && _weeks.size() > maxWeeks)
    {
        _weeks = _weeks.mid(_weeks.size() - maxWeeks);
        // 截断后第一列原本没有月份标签，补一个，否则整段图左边会缺月份。
        if (_weeks.first().monthLabel.isEmpty())
        {
            for (const Cell& cell : _weeks.first().cells)
            {
                if (cell.valid)
                {
                    _weeks.first().monthLabel =
                        QString::number(cell.date.month()) + QStringLiteral("月");
                    break;
                }
            }
        }
    }
}

void ActivityHeatmapWidget::layoutGrid()
{
    const int columns = static_cast<int>(_weeks.size());
    if (columns <= 0)
    {
        _cellSize = 0.0;
        return;
    }
    const qreal availableWidth = static_cast<qreal>(width()) - kLeftPad - kGap * (columns - 1);
    // 高度里扣掉底部图例带：热力格独占上方，图例画在下面的带子里。
    const qreal availableHeight = static_cast<qreal>(height()) - kTopPad - kBottomLegendBand
        - kGap * (kRows - 1);
    _cellSize = std::clamp(std::min(availableWidth / columns, availableHeight / kRows), kMinCellSize, kMaxCellSize);
    _gap = kGap;
    _leftPad = kLeftPad;
    _topPad = kTopPad;
}

bool ActivityHeatmapWidget::cellAt(const QPointF& position, int* week, int* row) const
{
    if (_cellSize <= 0.0 || _weeks.isEmpty())
    {
        return false;
    }
    const qreal step = _cellSize + _gap;
    const qreal localX = position.x() - _leftPad;
    const qreal localY = position.y() - _topPad;
    if (localX < 0.0 || localY < 0.0)
    {
        return false;
    }
    const int column = static_cast<int>(std::floor(localX / step));
    const int rowIndex = static_cast<int>(std::floor(localY / step));
    if (column < 0 || column >= _weeks.size() || rowIndex < 0 || rowIndex >= kRows)
    {
        return false;
    }
    // 落在格子之间的空隙里不算命中。
    if (localX - column * step > _cellSize || localY - rowIndex * step > _cellSize)
    {
        return false;
    }
    *week = column;
    *row = rowIndex;
    return true;
}

void ActivityHeatmapWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    if (_weeks.isEmpty())
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

    if (_hoverWeek < 0 || _hoverWeek >= _weeks.size() || _hoverRow < 0 || _hoverRow >= kRows)
    {
        return;
    }
    const Cell& cell = _weeks.at(_hoverWeek).cells.at(_hoverRow);
    if (!cell.valid)
    {
        return;
    }

    const qreal step = _cellSize + _gap;
    const QRectF rect(_leftPad + _hoverWeek * step, _topPad + _hoverRow * step, _cellSize, _cellSize);

    painter.setPen(QPen(ElaThemeColor(mode, BasicText), 1.4));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect.adjusted(0.7, 0.7, -0.7, -0.7), kCellRadius, kCellRadius);

    QFont tooltipFont = font();
    tooltipFont.setPixelSize(11);
    const QFontMetricsF metrics(tooltipFont);
    // 不足一小时只写分钟，不足一分钟只写秒：
    // 「0 小时 5 分」读起来像占位符，「0 分钟」更是。
    QString durationText;
    if (cell.durationMs < 60000)
    {
        durationText = tr("%1 秒").arg((cell.durationMs + 500) / 1000);
    }
    else if (cell.durationMs < 3600000)
    {
        durationText = tr("%1 分钟").arg(cell.durationMs / 60000);
    }
    else
    {
        durationText = tr("%1 小时 %2 分")
                           .arg(cell.durationMs / 3600000)
                           .arg((cell.durationMs / 60000) % 60);
    }
    const QString text = cell.hasRecord
                             ? QStringLiteral("%1 · %2").arg(
                                   cell.date.toString(QStringLiteral("M 月 d 日")), durationText)
                             : QStringLiteral("%1 · 无记录").arg(cell.date.toString(QStringLiteral("M 月 d 日")));

    const qreal boxWidth = metrics.horizontalAdvance(text) + 18.0;
    const qreal boxHeight = metrics.height() + 14.0;
    const qreal maxLeft = std::max(8.0, static_cast<qreal>(width()) - 8.0 - boxWidth);
    const qreal boxLeft = std::clamp(rect.center().x() - boxWidth / 2.0, 8.0, maxLeft);
    const qreal boxTop = rect.top() > boxHeight + 12.0 ? rect.top() - boxHeight - 8.0 : rect.bottom() + 8.0;
    const QRectF box(boxLeft, boxTop, boxWidth, boxHeight);

    painter.setPen(QPen(ElaThemeColor(mode, PopupBorder), 1.0));
    painter.setBrush(ElaThemeColor(mode, PopupBase));
    painter.drawRoundedRect(box, 8.0, 8.0);

    painter.setFont(tooltipFont);
    painter.setPen(ElaThemeColor(mode, BasicText));
    painter.drawText(box, Qt::AlignCenter, text);
}

void ActivityHeatmapWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    releaseCache();
}

void ActivityHeatmapWidget::mouseMoveEvent(QMouseEvent* event)
{
    int week = -1;
    int row = -1;
    const bool hit = cellAt(event->position(), &week, &row);
    const int newWeek = hit ? week : -1;
    const int newRow = hit ? row : -1;
    if (newWeek != _hoverWeek || newRow != _hoverRow)
    {
        _hoverWeek = newWeek;
        _hoverRow = newRow;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void ActivityHeatmapWidget::leaveEvent(QEvent* event)
{
    if (_hoverWeek >= 0 || _hoverRow >= 0)
    {
        _hoverWeek = -1;
        _hoverRow = -1;
        update();
    }
    QWidget::leaveEvent(event);
}

void ActivityHeatmapWidget::renderCache()
{
    layoutGrid();
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

void ActivityHeatmapWidget::drawStaticLayer(QPainter& painter) const
{
    if (_weeks.isEmpty() || _cellSize <= 0.0)
    {
        return;
    }
    const ElaThemeType::ThemeMode mode = eTheme->getThemeMode();
    const QColor accent = ElaThemeColor(mode, PrimaryNormal);
    const QColor track = ElaThemeColor(mode, BasicChute);
    const qreal step = _cellSize + _gap;

    QFont labelFont = font();
    labelFont.setPixelSize(kLabelFontPixelSize);
    painter.setFont(labelFont);
    const QFontMetricsF metrics(labelFont);

    // 左侧星期标签隔行画，七行全画会挤成一团。
    painter.setPen(ElaThemeColor(mode, BasicDetailsText));
    for (int row = 0; row < kRows; row += 2)
    {
        const QString text = weekdayLabels().value(row);
        painter.drawText(QPointF(0.0, _topPad + row * step + _cellSize / 2.0 + metrics.ascent() / 2.0 - 1.0), text);
    }

    qreal lastLabelX = -1000.0;
    for (int column = 0; column < _weeks.size(); ++column)
    {
        const Week& week = _weeks.at(column);
        const qreal x = _leftPad + column * step;

        // 月份标签：除了「这周是本月的第一周」，还要和上一个标签隔开足够距离。
        // 否则 53 周挤在 600px 里时，连续几周的标签会叠在一起糊成一团。
        if (!week.monthLabel.isEmpty() && x - lastLabelX >= kMonthLabelMinGap)
        {
            painter.setPen(ElaThemeColor(mode, BasicDetailsText));
            painter.drawText(QPointF(x, _topPad - 5.0), week.monthLabel);
            lastLabelX = x;
        }

        for (int row = 0; row < kRows; ++row)
        {
            const Cell& cell = week.cells.at(row);
            if (!cell.valid)
            {
                continue;
            }
            const QRectF rect(x, _topPad + row * step, _cellSize, _cellSize);
            QColor fill = track;
            const int level = levelFor(cell.hasRecord ? cell.durationMs : 0);
            if (level > 0)
            {
                fill = accent;
                fill.setAlphaF(kLevelAlpha[level - 1]);
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawRoundedRect(rect, kCellRadius, kCellRadius);
        }
    }

    // —— 右下角「少 □▢▣▤ 多」色阶图例（格子下方的专属图例带，不与格子重叠）——
    painter.setFont(labelFont);
    painter.setPen(ElaThemeColor(mode, BasicDetailsText));
    const QString lessText = tr("少");
    const QString moreText = tr("多");
    const qreal legendCellSize = std::min<qreal>(10.0, _cellSize);
    const qreal legendGap = 3.0;
    // 带子顶端 = 最后一排格子的底 + 间距；文字与色块在带内垂直居中。
    const qreal gridBottomY = _topPad + kRows * step - _gap;
    const qreal legendTop = gridBottomY + (kBottomLegendBand - legendCellSize) / 2.0;
    const qreal textY = legendTop + legendCellSize / 2.0 + metrics.ascent() / 2.0 - 1.0;
    // 从右往左排：「多」在右端，色阶块向左展开，空白格与「少」贴在最左。
    const qreal moreWidth = metrics.horizontalAdvance(moreText);
    qreal cursorX = static_cast<qreal>(width()) - 4.0 - moreWidth;
    painter.drawText(QPointF(cursorX, textY), moreText);

    // 色阶块：level 4 → 1 从右往左。
    for (int level = kLevelCount; level >= 1; --level)
    {
        cursorX -= legendCellSize + legendGap;
        QColor fill = accent;
        fill.setAlphaF(kLevelAlpha[level - 1]);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(
            QRectF(cursorX, legendTop, legendCellSize, legendCellSize), 2.0, 2.0);
    }
    // 空白格（无记录）也画进图例最左。
    cursorX -= legendCellSize + legendGap;
    painter.setPen(Qt::NoPen);
    painter.setBrush(track);
    painter.drawRoundedRect(QRectF(cursorX, legendTop, legendCellSize, legendCellSize), 2.0, 2.0);

    cursorX -= metrics.horizontalAdvance(lessText) + 2.0;
    painter.setPen(ElaThemeColor(mode, BasicDetailsText));
    painter.drawText(QPointF(cursorX, textY), lessText);
}

void ActivityHeatmapWidget::releaseCache()
{
    _cache = QPixmap();
    _cacheSize = QSize();
    _cacheMode = -1;
}

} // namespace patina::ui
