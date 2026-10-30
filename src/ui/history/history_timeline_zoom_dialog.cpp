#include "ui/history/history_timeline_zoom_dialog.h"

#include <QDate>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

#include "ElaIconButton.h"
#include "ElaPushButton.h"
#include "ElaScrollPageArea.h"
#include "ElaText.h"
#include "ElaTheme.h"

#include "shared/theme_styles.h"

namespace patina::ui
{
namespace
{

constexpr qint64 kHourMs = 3600 * 1000LL;
constexpr qint64 kMinViewportMs = 1 * kHourMs;
constexpr qint64 kMaxViewportMs = 24 * kHourMs;
constexpr qint64 kDefaultViewportMs = 4 * kHourMs;

ElaText* makeLabel(const QString& text, int pixelSize, QWidget* parent, bool strong = false)
{
    auto* label = new ElaText(text, pixelSize, parent);
    label->setWordWrap(false);
    if (strong) {
        label->setTextStyle(ElaTextType::BodyStrong);
        label->setTextPixelSize(pixelSize);
    }
    return label;
}

} // namespace

HistoryTimelineZoomDialog::HistoryTimelineZoomDialog(QWidget* parent)
    : ElaDialog(parent)
{
    buildUi();
    setModal(false);
    resize(860, 420);

    // 弹窗是顶层窗口，自己样式表里的颜色得跟着主题走。
    connect(eTheme, &ElaTheme::themeModeChanged, this, [this](ElaThemeType::ThemeMode mode) {
        refreshThemeStyles(mode == ElaThemeType::ThemeMode::Dark);
        _timeline->update();
    });
}

void HistoryTimelineZoomDialog::refreshThemeStyles(bool dark)
{
    const QString secondary = shared::secondaryTextStyle(dark);
    if (_viewportLabel != nullptr) {
        _viewportLabel->setStyleSheet(secondary);
    }
    if (_hintLabel != nullptr) {
        _hintLabel->setStyleSheet(secondary);
    }
}

void HistoryTimelineZoomDialog::buildUi()
{
    this->setWindowTitle("");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    // —— 日期控制条 ——
    auto* controls = new QWidget(this);
    auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(8);

    _prevButton = new ElaIconButton(ElaIconType::CaretLeft, 11, 26, 26, controls);
    _prevButton->setBorderRadius(8);
    _prevButton->setToolTip(tr("前一天"));
    connect(_prevButton, &ElaIconButton::clicked, this, [this]() { shiftDate(-1); });
    controlsLayout->addWidget(_prevButton);

    _todayButton = new ElaPushButton(tr("今天"), controls);
    _todayButton->setFixedHeight(26);
    _todayButton->setMinimumWidth(56);
    _todayButton->setBorderRadius(8);
    connect(_todayButton, &ElaPushButton::clicked, this,
            [this]() { _date = QDate::currentDate(); emit dateChanged(_date); });
    controlsLayout->addWidget(_todayButton);

    _nextButton = new ElaIconButton(ElaIconType::CaretRight, 11, 26, 26, controls);
    _nextButton->setBorderRadius(8);
    _nextButton->setToolTip(tr("后一天"));
    connect(_nextButton, &ElaIconButton::clicked, this, [this]() { shiftDate(1); });
    controlsLayout->addWidget(_nextButton);

    _dateLabel = makeLabel(QString(), 13, controls, true);
    controlsLayout->addWidget(_dateLabel);
    controlsLayout->addStretch();

    auto* zoomOut = new ElaIconButton(ElaIconType::Minus, 11, 26, 26, controls);
    zoomOut->setBorderRadius(8);
    zoomOut->setToolTip(tr("缩小视口范围"));
    connect(zoomOut, &ElaIconButton::clicked, this, [this]() { zoomBy(-2); });
    controlsLayout->addWidget(zoomOut);

    auto* zoomIn = new ElaIconButton(ElaIconType::Plus, 11, 26, 26, controls);
    zoomIn->setBorderRadius(8);
    zoomIn->setToolTip(tr("放大视口（看更细的段落）"));
    connect(zoomIn, &ElaIconButton::clicked, this, [this]() { zoomBy(2); });
    controlsLayout->addWidget(zoomIn);

    _viewportLabel = makeLabel(QString(), 11, controls);
    controlsLayout->addWidget(_viewportLabel);

    auto* closeButton = new ElaIconButton(ElaIconType::Xmark, 11, 26, 26, controls);
    closeButton->setBorderRadius(8);
    closeButton->setToolTip(tr("关闭"));
    connect(closeButton, &ElaIconButton::clicked, this, &QDialog::close);
    controlsLayout->addWidget(closeButton);

    layout->addWidget(controls);

    // —— 时间轴主区 ——
    auto* card = new ElaScrollPageArea(this);
    card->setMinimumHeight(0);
    card->setMaximumHeight(QWIDGETSIZE_MAX);
    card->setBorderRadius(12);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(8, 4, 8, 4);
    _timeline = new HistoryTimelineWidget(card);
    connect(_timeline, &HistoryTimelineWidget::segmentClicked, this, [this](
        const TimelineLaneSegment& segment) {
        // 气泡在对话框里复用主视图的行为：这里临时借用 tooltip 语义太弱，
        // 直接弹 QToolTip 风格的静态标签就够了——明细信息点击段落时看详情弹窗。
        Q_UNUSED(segment)
    });
    cardLayout->addWidget(_timeline);
    layout->addWidget(card, 1);

    _hintLabel = makeLabel(tr("双击时间轴可重新聚焦；± 调整视口小时数（1–24h）。"), 11, this);
    layout->addWidget(_hintLabel);

    refreshThemeStyles(eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark);
}

void HistoryTimelineZoomDialog::openFor(const QDate& date, qint64 focusMs,
                                        const QVector<TimelineLane>& lanes,
                                        qint64 dayStartMs, qint64 dayEndMs)
{
    _date = date;
    _lanes = lanes;
    _dayStartMs = dayStartMs;
    _dayEndMs = dayEndMs;
    _hasDayBounds = dayEndMs > dayStartMs;
    _viewportDurationMs = kDefaultViewportMs;
    _viewportStartMs = dayStartMs;
    normalizeViewportAround(focusMs);
    refreshView();
    show();
    raise();
    activateWindow();
}

void HistoryTimelineZoomDialog::updateLanes(const QVector<TimelineLane>& lanes,
                                            qint64 dayStartMs, qint64 dayEndMs)
{
    _lanes = lanes;
    _dayStartMs = dayStartMs;
    _dayEndMs = dayEndMs;
    _hasDayBounds = dayEndMs > dayStartMs;
    if (_viewportStartMs < dayStartMs || _viewportStartMs >= dayEndMs) {
        _viewportStartMs = dayStartMs;
    }
    refreshView();
}

void HistoryTimelineZoomDialog::normalizeViewportAround(qint64 focusMs)
{
    if (!_hasDayBounds) {
        return;
    }
    const qint64 daySpan = _dayEndMs - _dayStartMs;
    _viewportDurationMs = std::clamp<qint64>(_viewportDurationMs, kMinViewportMs,
                                             std::min<qint64>(kMaxViewportMs, daySpan));
    const qint64 offset = focusMs - _dayStartMs;
    const qint64 snapped = (offset / (30 * 60 * 1000LL)) * (30 * 60 * 1000LL);
    const qint64 focus = _dayStartMs + snapped;
    qint64 start = focus - _viewportDurationMs / 2;
    start = std::clamp<qint64>(start, _dayStartMs, _dayEndMs - _viewportDurationMs);
    _viewportStartMs = start;
}

void HistoryTimelineZoomDialog::refreshView()
{
    if (_timeline != nullptr) {
        _timeline->setLanes(_viewportStartMs, _viewportStartMs + _viewportDurationMs, _lanes);
    }
    _dateLabel->setText(_date.toString(QStringLiteral("yyyy 年 M 月 d 日"))
                        + (_date == QDate::currentDate() ? tr("（今天）") : QString()));
    _viewportLabel->setText(
        tr("视口 %1 小时 · %2 – %3")
            .arg(_viewportDurationMs / kHourMs)
            .arg(QDateTime::fromMSecsSinceEpoch(_viewportStartMs).toString(QStringLiteral("HH:mm")),
                 QDateTime::fromMSecsSinceEpoch(_viewportStartMs + _viewportDurationMs)
                     .toString(QStringLiteral("HH:mm"))));
    _nextButton->setEnabled(_date < QDate::currentDate());
    _todayButton->setEnabled(_date != QDate::currentDate());
}

void HistoryTimelineZoomDialog::shiftDate(int days)
{
    const QDate next = _date.addDays(days);
    if (next > QDate::currentDate() || !next.isValid()) {
        return;
    }
    _date = next;
    emit dateChanged(_date);
}

void HistoryTimelineZoomDialog::zoomBy(int deltaHours)
{
    const qint64 next = std::clamp<qint64>(_viewportDurationMs + deltaHours * kHourMs,
                                           kMinViewportMs, kMaxViewportMs);
    if (next == _viewportDurationMs) {
        return;
    }
    const qint64 center = _viewportStartMs + _viewportDurationMs / 2;
    _viewportDurationMs = next;
    normalizeViewportAround(center);
    refreshView();
}

void HistoryTimelineZoomDialog::hideEvent(QHideEvent* event)
{
    QDialog::hideEvent(event);
    // 关掉就释放位图与数据。
    if (_timeline != nullptr) {
        _timeline->clear();
    }
}

} // namespace patina::ui
