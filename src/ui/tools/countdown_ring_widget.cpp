#include "ui/tools/countdown_ring_widget.h"

#include "ElaTheme.h"

#include <QFontMetrics>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace patina::ui
{

namespace
{

constexpr qreal kStrokeWidth = 9.0;
constexpr qreal kOuterMargin = 10.0;
constexpr qreal kPausedDepth = 0.45;

} // namespace

CountdownRingWidget::CountdownRingWidget(QWidget* parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setMinimumSize(140, 140);
}

CountdownRingWidget::~CountdownRingWidget() = default;

void CountdownRingWidget::setProgress(qreal progress)
{
    const qreal clamped = progress < 0.0 ? -1.0 : std::clamp<qreal>(progress, 0.0, 1.0);
    if (qFuzzyCompare(clamped + 1.0, _progress + 1.0)) {
        return;
    }
    _progress = clamped;
    update();
}

void CountdownRingWidget::setPrimaryText(const QString& text)
{
    if (_primaryText == text) {
        return;
    }
    _primaryText = text;
    update();
}

void CountdownRingWidget::setSecondaryText(const QString& text)
{
    if (_secondaryText == text) {
        return;
    }
    _secondaryText = text;
    update();
}

void CountdownRingWidget::setPaused(bool paused)
{
    if (_paused == paused) {
        return;
    }
    _paused = paused;
    update();
}

void CountdownRingWidget::clear()
{
    _progress = -1.0;
    _primaryText.clear();
    _secondaryText.clear();
    _paused = false;
    update();
}

QSize CountdownRingWidget::sizeHint() const
{
    return {168, 168};
}

QSize CountdownRingWidget::minimumSizeHint() const
{
    return {140, 140};
}

void CountdownRingWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const ElaThemeType::ThemeMode mode = eTheme->getThemeMode();
    const QColor trackColor = ElaThemeColor(mode, BasicChute);
    QColor accentColor = ElaThemeColor(mode, PrimaryNormal);
    if (_paused) {
        accentColor.setAlphaF(kPausedDepth);
    }
    const QColor primaryColor = ElaThemeColor(mode, BasicText);
    const QColor secondaryColor = ElaThemeColor(mode, BasicDetailsText);

    const qreal side = std::min(width(), height());
    const QRectF ringRect(kOuterMargin, kOuterMargin, side - kOuterMargin * 2.0,
                          side - kOuterMargin * 2.0);

    QPen trackPen(trackColor, kStrokeWidth);
    trackPen.setCapStyle(Qt::FlatCap);
    painter.setPen(trackPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(ringRect);

    // 进度弧：从 12 点方向顺时针。
    if (_progress > 0.0) {
        QPen progressPen(accentColor, kStrokeWidth);
        progressPen.setCapStyle(Qt::RoundCap);
        painter.setPen(progressPen);
        const int spanDegrees = static_cast<int>(std::lround(-(_progress * 360.0)));
        // drawArc 的角度单位是 1/16 度，正方向是逆时针，所以从 90° 起往负方向走。
        painter.drawArc(ringRect, 90 * 16, spanDegrees * 16);
    }

    // 中间文字：主行大号等宽，副行小号。
    QFont primaryFont = font();
    primaryFont.setPixelSize(std::max(15, static_cast<int>(side * 0.135)));
    primaryFont.setWeight(QFont::DemiBold);
    painter.setFont(primaryFont);
    painter.setPen(primaryColor);

    const QRectF textArea = ringRect.adjusted(kStrokeWidth + 4, 0, -(kStrokeWidth + 4), 0);
    const QFontMetrics primaryMetrics(primaryFont);
    const int primaryHeight = primaryMetrics.height();

    if (!_secondaryText.isEmpty()) {
        QFont secondaryFont = font();
        secondaryFont.setPixelSize(std::max(10, static_cast<int>(side * 0.072)));
        const QFontMetrics secondaryMetrics(secondaryFont);
        const int secondaryHeight = secondaryMetrics.height();
        const int totalHeight = primaryHeight + secondaryHeight;
        const qreal primaryTop = textArea.center().y() - totalHeight / 2.0;

        painter.drawText(QRectF(textArea.left(), primaryTop, textArea.width(), primaryHeight),
                         Qt::AlignCenter, _primaryText);

        painter.setFont(secondaryFont);
        painter.setPen(secondaryColor);
        painter.drawText(QRectF(textArea.left(), primaryTop + primaryHeight, textArea.width(),
                                secondaryHeight),
                         Qt::AlignCenter, _secondaryText);
    } else {
        painter.drawText(textArea, Qt::AlignCenter, _primaryText);
    }
}

} // namespace patina::ui
