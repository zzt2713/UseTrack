#include "ui/history/history_feed_widget.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>

#include "ElaTheme.h"

namespace
{

// 数值对齐 preview/2b-history.html 里方案 B 的活动流样式。
constexpr int kTimeColumnWidth = 52;
constexpr int kSpineColumnWidth = 14;
constexpr int kCardLeftMargin = 10;
constexpr int kCardPaddingX = 11;
constexpr int kCardPaddingY = 8;
constexpr int kCardRadius = 10;
constexpr int kCardMarginY = 2;
constexpr int kDotSize = 8;
constexpr int kDotTop = 13;
constexpr int kSpineX = kTimeColumnWidth + 6;
constexpr int kDotX = kTimeColumnWidth + 2;
constexpr int kCardX = kTimeColumnWidth + kSpineColumnWidth + kCardLeftMargin;
constexpr int kHourHeaderTop = 12;
constexpr int kHourHeaderBottom = 6;
constexpr int kTimeTop = 9;
constexpr int kNameTitleGap = 2;
constexpr int kTitleMetaGap = 3;
constexpr int kTrailingGap = 8;
// 卡片右缘到控件右缘的留白。活动流不再自己滚动，这里只是右侧内边距。
constexpr int kCardRightGutter = 10;

QString elidedText(const QFontMetrics& metrics, const QString& text, int width)
{
    if (width <= 0 || text.isEmpty())
    {
        return QString();
    }
    return metrics.elidedText(text, Qt::ElideRight, width);
}

QString clockText(qint64 timestampMs)
{
    return QDateTime::fromMSecsSinceEpoch(timestampMs).toString(QStringLiteral("HH:mm"));
}

// 时间轴竖线 + 圆点，空档行与事件行共用。
void paintSpine(QPainter& painter, const QRect& rowRect, bool lastRow, const QColor& lineColor,
                const QColor& fillColor, const QColor& dotColor)
{
    const int lineBottom = lastRow ? rowRect.top() + rowRect.height() / 2 : rowRect.bottom() + 1;
    painter.setPen(QPen(lineColor, 1));
    painter.drawLine(kSpineX, rowRect.top(), kSpineX, lineBottom);

    painter.setPen(QPen(dotColor, 2));
    painter.setBrush(fillColor);
    painter.drawEllipse(QRect(kDotX, rowRect.top() + kDotTop, kDotSize, kDotSize));
    painter.setBrush(Qt::NoBrush);
}

} // namespace

namespace patina::ui
{

HistoryFeedWidget::HistoryFeedWidget(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    _nameFont = font();
    _nameFont.setPixelSize(13);
    _nameFont.setWeight(QFont::DemiBold);

    _titleFont = font();
    _titleFont.setPixelSize(12);

    _metaFont = font();
    _metaFont.setPixelSize(11);

    _valueFont = font();
    _valueFont.setPixelSize(12);
    _valueFont.setWeight(QFont::DemiBold);

    _hourFont = font();
    _hourFont.setPixelSize(11);
    _hourFont.setWeight(QFont::DemiBold);

    const QFontMetrics hourMetrics(_hourFont);
    const QFontMetrics nameMetrics(_nameFont);
    const QFontMetrics titleMetrics(_titleFont);
    const QFontMetrics metaMetrics(_metaFont);

    _hourHeaderHeight = hourMetrics.height() + kHourHeaderTop + kHourHeaderBottom;
    _eventRowHeight = kCardMarginY * 2 + kCardPaddingY * 2 + nameMetrics.height() + kNameTitleGap
                     + titleMetrics.height() + kTitleMetaGap + metaMetrics.height();
    _gapRowHeight = kCardMarginY * 2 + kCardPaddingY * 2 + nameMetrics.height() + kNameTitleGap
                    + titleMetrics.height();
}

HistoryFeedWidget::~HistoryFeedWidget() = default;

void HistoryFeedWidget::setRows(const QVector<HistoryFeedRow>& rows)
{
    _rows.clear();
    _rows.reserve(rows.size());

    int top = 0;
    for (const HistoryFeedRow& row : rows) {
        Row item;
        item.data = row;
        item.top = top;
        item.height = measureRow(row);
        top += item.height;
        _rows.append(item);
    }

    _contentHeight = top;
    // 不放在滚动区里了：高度直接按内容固定，卡片跟着行数长，滚动交给整页外壳。
    setFixedHeight(_contentHeight);
    updateGeometry();
    update();
}

void HistoryFeedWidget::clear()
{
    if (_rows.isEmpty()) {
        return;
    }
    _rows.clear();
    _contentHeight = 0;
    setFixedHeight(0);
    updateGeometry();
    update();
}

bool HistoryFeedWidget::isEmpty() const
{
    return _rows.isEmpty();
}

QSize HistoryFeedWidget::sizeHint() const
{
    return {320, _contentHeight};
}

QSize HistoryFeedWidget::minimumSizeHint() const
{
    return {240, _contentHeight};
}

int HistoryFeedWidget::measureRow(const HistoryFeedRow& row) const
{
    switch (row.kind) {
    case HistoryFeedRow::Kind::HourHeader:
        return _hourHeaderHeight;
    case HistoryFeedRow::Kind::Gap:
        return _gapRowHeight;
    case HistoryFeedRow::Kind::Event:
        break;
    }
    return _eventRowHeight;
}

void HistoryFeedWidget::paintEvent(QPaintEvent* event)
{
    if (_rows.isEmpty()) {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QRect clipRect = event->rect();
    for (int i = 0; i < _rows.size(); ++i) {
        const Row& row = _rows.at(i);
        if (row.top + row.height < clipRect.top()) {
            continue;
        }
        if (row.top > clipRect.bottom()) {
            break;
        }

        const bool lastRow = (i == _rows.size() - 1);
        switch (row.data.kind) {
        case HistoryFeedRow::Kind::HourHeader:
            paintHourHeader(painter, row);
            break;
        case HistoryFeedRow::Kind::Gap:
            paintGapRow(painter, row, lastRow);
            break;
        case HistoryFeedRow::Kind::Event:
            paintEventRow(painter, row, lastRow);
            break;
        }
    }
}

void HistoryFeedWidget::paintHourHeader(QPainter& painter, const Row& row) const
{
    const ElaThemeType::ThemeMode mode = eTheme->getThemeMode();
    const QColor lineColor = ElaThemeColor(mode, BasicBorder);
    const QColor textColor = ElaThemeColor(mode, BasicDetailsText);

    const QFontMetrics metrics(_hourFont);
    const QString text = row.data.hourText;
    const int textWidth = metrics.horizontalAdvance(text);
    const int textHeight = metrics.height();
    const int textTop = row.top + kHourHeaderTop;

    painter.setFont(_hourFont);
    painter.setPen(textColor);
    painter.drawText(QRect(0, textTop, textWidth + 2, textHeight), Qt::AlignLeft | Qt::AlignVCenter, text);

    const int lineX = textWidth + 10;
    if (width() > lineX) {
        const int lineY = textTop + textHeight / 2;
        painter.setPen(QPen(lineColor, 1));
        painter.drawLine(lineX, lineY, width(), lineY);
    }
}

void HistoryFeedWidget::paintGapRow(QPainter& painter, const Row& row, bool lastRow) const
{
    const ElaThemeType::ThemeMode mode = eTheme->getThemeMode();
    const QColor borderColor = ElaThemeColor(mode, BasicBorder);
    const QColor baseColor = ElaThemeColor(mode, BasicBase);
    const QColor textColor = ElaThemeColor(mode, BasicDetailsText);

    const QRect rowRect(0, row.top, width(), row.height);
    paintSpine(painter, rowRect, lastRow, borderColor, baseColor, borderColor);

    const QFontMetrics timeMetrics(_titleFont);
    painter.setFont(_titleFont);
    painter.setPen(textColor);
    painter.drawText(QRect(0, row.top + kTimeTop, kTimeColumnWidth, timeMetrics.height()),
                     Qt::AlignLeft | Qt::AlignTop, clockText(row.data.startTimeMs));

    const QRect cardRect(kCardX, row.top + kCardMarginY, width() - kCardX - kCardRightGutter,
                         row.height - kCardMarginY * 2);
    if (cardRect.width() <= 0 || cardRect.height() <= 0) {
        return;
    }

    // 空档用虚线描边、不填充，视觉上比事件行轻一档。
    painter.setPen(QPen(borderColor, 1, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(cardRect).adjusted(0.5, 0.5, -0.5, -0.5), kCardRadius, kCardRadius);

    const QRect contentRect = cardRect.adjusted(kCardPaddingX, kCardPaddingY, -kCardPaddingX, -kCardPaddingY);
    const QFontMetrics nameMetrics(_nameFont);
    const QFontMetrics titleMetrics(_titleFont);

    painter.setFont(_nameFont);
    painter.setPen(textColor);
    painter.drawText(QRect(contentRect.x(), contentRect.y(), contentRect.width(), nameMetrics.height()),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     elidedText(nameMetrics, row.data.primary, contentRect.width()));

    const int titleTop = contentRect.y() + nameMetrics.height() + kNameTitleGap;
    painter.setFont(_titleFont);
    painter.setPen(textColor);
    painter.drawText(QRect(contentRect.x(), titleTop, contentRect.width(), titleMetrics.height()),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     elidedText(titleMetrics, row.data.secondary, contentRect.width()));
}

void HistoryFeedWidget::paintEventRow(QPainter& painter, const Row& row, bool lastRow) const
{
    const ElaThemeType::ThemeMode mode = eTheme->getThemeMode();
    const QColor panelColor = ElaThemeColor(mode, BasicBase);
    const QColor borderColor = ElaThemeColor(mode, BasicBorder);
    const QColor nameColor = ElaThemeColor(mode, BasicText);
    const QColor metaColor = ElaThemeColor(mode, BasicDetailsText);

    const QRect rowRect(0, row.top, width(), row.height);
    paintSpine(painter, rowRect, lastRow, borderColor, panelColor, row.data.dotColor);

    const QFontMetrics timeMetrics(_titleFont);
    painter.setFont(_titleFont);
    painter.setPen(metaColor);
    painter.drawText(QRect(0, row.top + kTimeTop, kTimeColumnWidth, timeMetrics.height()),
                     Qt::AlignLeft | Qt::AlignTop, clockText(row.data.startTimeMs));

    const QRect cardRect(kCardX, row.top + kCardMarginY, width() - kCardX - kCardRightGutter,
                         row.height - kCardMarginY * 2);
    if (cardRect.width() <= 0 || cardRect.height() <= 0) {
        return;
    }

    painter.setPen(QPen(borderColor, 1));
    painter.setBrush(panelColor);
    painter.drawRoundedRect(QRectF(cardRect).adjusted(0.5, 0.5, -0.5, -0.5), kCardRadius, kCardRadius);
    painter.setBrush(Qt::NoBrush);

    const QRect contentRect = cardRect.adjusted(kCardPaddingX, kCardPaddingY, -kCardPaddingX, -kCardPaddingY);
    const QFontMetrics nameMetrics(_nameFont);
    const QFontMetrics titleMetrics(_titleFont);
    const QFontMetrics metaMetrics(_metaFont);
    const QFontMetrics valueMetrics(_valueFont);

    const int trailingWidth = valueMetrics.horizontalAdvance(row.data.trailing);
    const int nameWidth = contentRect.width() - trailingWidth - kTrailingGap;

    painter.setFont(_nameFont);
    painter.setPen(nameColor);
    painter.drawText(QRect(contentRect.x(), contentRect.y(), nameWidth, nameMetrics.height()),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     elidedText(nameMetrics, row.data.primary, nameWidth));

    if (trailingWidth > 0) {
        painter.setFont(_valueFont);
        painter.setPen(metaColor);
        painter.drawText(QRect(contentRect.x() + contentRect.width() - trailingWidth, contentRect.y(),
                               trailingWidth, valueMetrics.height()),
                         Qt::AlignRight | Qt::AlignVCenter, row.data.trailing);
    }

    const int titleTop = contentRect.y() + nameMetrics.height() + kNameTitleGap;
    painter.setFont(_titleFont);
    painter.setPen(metaColor);
    painter.drawText(QRect(contentRect.x(), titleTop, contentRect.width(), titleMetrics.height()),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     elidedText(titleMetrics, row.data.secondary, contentRect.width()));

    const int metaTop = titleTop + titleMetrics.height() + kTitleMetaGap;
    painter.setFont(_metaFont);
    painter.setPen(metaColor);
    painter.drawText(QRect(contentRect.x(), metaTop, contentRect.width(), metaMetrics.height()),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     elidedText(metaMetrics, row.data.tertiary, contentRect.width()));
}

} // namespace patina::ui
