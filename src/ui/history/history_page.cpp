#include "ui/history/history_page.h"

#include <QColor>
#include <QCursor>
#include <QDateTime>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayoutItem>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStringList>
#include <QToolTip>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

#include "ElaCalendarPicker.h"
#include "ElaComboBox.h"
#include "ElaIconButton.h"
#include "ElaPivot.h"
#include "ElaPushButton.h"
#include "ElaScrollPageArea.h"
#include "ElaText.h"
#include "ElaTheme.h"

#include "data/app_links.h"
#include "data/category_registry.h"
#include "data/hourly_read_model.h"
#include "engine/tracking_settings.h"
#include "shared/theme_styles.h"
#include "ui/dashboard/app_rank_list_widget.h"
#include "ui/destination/destination_detail_dialog.h"
#include "ui/history/history_day_band_widget.h"
#include "ui/history/history_feed_widget.h"
#include "ui/history/history_timeline_widget.h"
#include "ui/history/history_timeline_zoom_dialog.h"
#include "ui/shared/hourly_bars_widget.h"

namespace
{

// 一页取多少段。取整百会让单页渲染耗时变长，60 段大约一屏半。
constexpr int kPageSize = 60;

// 单日保留上限。分页是为了不一次把全表读进来，但一直点「加载更多」仍然会无界增长；
// 这个硬顶保证「翻页时内存有界」，触发时界面上会明说。
constexpr int kMaxRetainedSegments = 2000;

// 时间轴一次读多少段。一天最多 86400 秒，正常数据量在几百段这个量级；
// 这个上限只用来兜住异常数据（内存有界），
// 与活动流的 kMaxRetainedSegments 分开——后者管的是「点加载更多别把内存吃穿」。
constexpr int kMaxTimelineSegments = 100000;

// 相邻两段之间超过这个间隔才插一条「无记录」。
constexpr qint64 kGapThresholdMs = 20 * 60 * 1000;

// 同一应用相邻两段的间隔不超过它就直接并成一段（5 秒）。
// 抖动的碎段被「最短可见时长」摘掉之后，同应用的前后两段会重新变成相邻，
// 这时按这个间隔黏回去——否则摘掉碎段反而让一次连续使用裂成好几行。
constexpr qint64 kDirectMergeGapMs = 5 * 1000;

// 「最短可见时长」下拉的可选秒数。0 = 不过滤。
constexpr qint64 kMinSessionChoices[] = {0, 1, 3, 5, 10, 30, 60, 300};

constexpr int kLegendDotSize = 9;
constexpr int kSidebarWidth = 268;

QString formatDurationLong(qint64 durationMs)
{
    const qint64 totalMs = std::max<qint64>(0, durationMs);
    // 不足一分钟直接给秒数：否则 7 秒的段也显示「0 分」，
    // 一屏几个「0 分」看着像数据坏了。
    if (totalMs < 60000) {
        return QObject::tr("%1 秒").arg((totalMs + 500) / 1000);
    }
    const qint64 totalMinutes = totalMs / 60000;
    if (totalMinutes < 60) {
        return QObject::tr("%1 分").arg(totalMinutes);
    }
    const qint64 hours = totalMinutes / 60;
    const qint64 minutes = totalMinutes % 60;
    return QObject::tr("%1 小时 %2 分").arg(hours).arg(minutes, 2, 10, QLatin1Char('0'));
}

QString formatDurationShort(qint64 durationMs)
{
    const qint64 totalMinutes = std::max<qint64>(0, durationMs) / 60000;
    if (totalMinutes < 60) {
        return QObject::tr("%1m").arg(totalMinutes);
    }
    const qint64 hours = totalMinutes / 60;
    const qint64 minutes = totalMinutes % 60;
    return QObject::tr("%1h %2m").arg(hours).arg(minutes, 2, 10, QLatin1Char('0'));
}

QString displayNameFor(const QString& exeName)
{
    QString name = exeName.trimmed();
    if (name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        name.chop(4);
    }
    if (name.isEmpty()) {
        return QObject::tr("未知应用");
    }
    name[0] = name.at(0).toUpper();
    return name;
}

ElaScrollPageArea* makeCard(QWidget* parent)
{
    auto* card = new ElaScrollPageArea(parent);
    // ElaScrollPageArea 构造里写死 setFixedHeight(75)，卡片要跟着内容长，必须解开。
    card->setMinimumHeight(0);
    card->setMaximumHeight(QWIDGETSIZE_MAX);
    card->setBorderRadius(14);
    return card;
}

ElaText* makeLabel(const QString& text, int pixelSize, QWidget* parent, bool strong = false)
{
    auto* label = new ElaText(text, pixelSize, parent);
    label->setWordWrap(false);
    if (strong) {
        label->setTextStyle(ElaTextType::BodyStrong);
        // setTextStyle 会改字号，必须在它之后设回来。
        label->setTextPixelSize(pixelSize);
    }
    return label;
}

} // namespace

namespace patina::ui
{

HistoryPage::HistoryPage(data::Database* database, QWidget* parent)
    : QWidget(parent)
    , _database(database)
    , _date(QDate::currentDate())
{
    buildUi();
    loadMinSessionSetting();
    // 分布模式持久化：与 Dashboard 的小时图模式同一套 QSettings 约定。
    _timelineByCategory =
        QSettings().value(QStringLiteral("history/timelineByCategory"), false).toBool();
    if (_legendModePivot != nullptr) {
        const QSignalBlocker blocker(_legendModePivot);
        _legendModePivot->setCurrentIndex(_timelineByCategory ? 1 : 0);
    }
    if (_legendTitle != nullptr) {
        _legendTitle->setText(_timelineByCategory ? tr("分类分布") : tr("应用分布"));
    }

    connect(eTheme, &ElaTheme::themeModeChanged, this, [this](ElaThemeType::ThemeMode mode) {
        const bool dark = mode == ElaThemeType::ThemeMode::Dark;
        _feed->update();
        _band->update();
        _timeline->update();
        _hourlyBars->update();
        refreshPopoverStyle(dark);
        refreshLegendStyle(dark);
        if (_minSessionBox != nullptr) {
            shared::applyComboBoxTheme(_minSessionBox, dark);
        }
        // lane 的兜底色是按主题取的，切主题时得重建一次；
        // 没有分类色可用的应用才会走到那条路径，重建成本很低。
        if (_loaded) {
            rebuildTimeline();
        }
    });

    updateDateUi();
    applyEmptyState(tr("正在读取历史数据…"));
}

HistoryPage::~HistoryPage() = default;

void HistoryPage::buildUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(24, 16, 24, 24);
    rootLayout->setSpacing(14);

    auto* header = new QWidget(this);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(10);
    headerLayout->addWidget(makeLabel(tr("历史"), 19, header, true));
    headerLayout->addWidget(makeLabel(tr("按天回看每一次应用切换"), 12, header));
    headerLayout->addStretch();
    // 段数统计放标题行，不放日期行：日期行已经排了六个控件，
    // 再加一段文字会把整页的最小宽度顶上去（QLabel 不换行时按全文宽度占位）。
    _chipLabel = makeLabel(QString(), 12, header);
    headerLayout->addWidget(_chipLabel);
    rootLayout->addWidget(header);

    auto* dateBar = new QWidget(this);
    auto* dateBarLayout = new QHBoxLayout(dateBar);
    dateBarLayout->setContentsMargins(0, 0, 0, 0);
    dateBarLayout->setSpacing(10);

    _prevButton = new ElaIconButton(ElaIconType::CaretLeft, 12, 30, 30, dateBar);
    _prevButton->setBorderRadius(9);
    _prevButton->setToolTip(tr("前一天"));
    connect(_prevButton, &ElaIconButton::clicked, this, [this]() { selectDate(_date.addDays(-1)); });
    dateBarLayout->addWidget(_prevButton);

    _dateLabel = makeLabel(QString(), 17, dateBar, true);
    dateBarLayout->addWidget(_dateLabel);
    _weekdayLabel = makeLabel(QString(), 13, dateBar);
    dateBarLayout->addWidget(_weekdayLabel);

    _nextButton = new ElaIconButton(ElaIconType::CaretRight, 12, 30, 30, dateBar);
    _nextButton->setBorderRadius(9);
    _nextButton->setToolTip(tr("后一天"));
    connect(_nextButton, &ElaIconButton::clicked, this, [this]() { selectDate(_date.addDays(1)); });
    dateBarLayout->addWidget(_nextButton);

    _todayButton = new ElaPushButton(tr("今天"), dateBar);
    _todayButton->setFixedHeight(30);
    _todayButton->setMinimumWidth(64);
    _todayButton->setBorderRadius(9);
    connect(_todayButton, &ElaPushButton::clicked, this, [this]() { selectDate(QDate::currentDate()); });
    dateBarLayout->addWidget(_todayButton);

    _datePicker = new ElaCalendarPicker(dateBar);
    _datePicker->setToolTip(tr("选择日期"));
    connect(_datePicker, &ElaCalendarPicker::selectedDateChanged, this, [this](QDate date) {
        if (_updatingPicker || !date.isValid()) {
            return;
        }
        selectDate(date);
    });
    dateBarLayout->addWidget(_datePicker);

    // 最短可见时长：切走一瞬间就切回来的抖动不该各占一行。
    // 只在活动流的显示层过滤，库里的段一条不少；时间轴读的是 _timelineSegments
    // （当天全量段），不受这个阈值影响——它若也跟着过滤，真实使用里成片的短切换
    // 会被抹掉，色带中间凭空出现大段空白，看起来像「那些时间没用电脑」。
    _minSessionBox = new ElaComboBox(dateBar);
    _minSessionBox->setBorderRadius(9);
    _minSessionBox->setFixedHeight(30);
    _minSessionBox->setMinimumWidth(112);
    _minSessionBox->setToolTip(tr("短于这个时长的切换不在活动流里显示（时间轴与统计不受影响）"));
    for (const qint64 secs : kMinSessionChoices) {
        if (secs <= 0) {
            _minSessionBox->addItem(tr("显示全部"), QVariant::fromValue<qint64>(0));
        } else if (secs < 60) {
            _minSessionBox->addItem(tr("≥ %1 秒").arg(secs), QVariant::fromValue<qint64>(secs));
        } else {
            _minSessionBox->addItem(tr("≥ %1 分钟").arg(secs / 60), QVariant::fromValue<qint64>(secs));
        }
    }
    shared::applyComboBoxTheme(_minSessionBox,
                               eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark);
    connect(_minSessionBox, &ElaComboBox::currentIndexChanged, this, [this](int) {
        const qint64 secs = _minSessionBox->currentData().toLongLong();
        applyMinSessionSetting(secs);
    });
    dateBarLayout->addWidget(_minSessionBox);
    // 末尾留个伸缩：不给的话多余宽度会均摊给下拉框，日期行会被撑得很松散。
    dateBarLayout->addStretch();

    rootLayout->addWidget(dateBar);

    _statusLabel = makeLabel(QString(), 13, this);
    _statusLabel->setWordWrap(true);
    _statusLabel->setVisible(false);
    rootLayout->addWidget(_statusLabel);

    auto* body = new QHBoxLayout();
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(14);

    // 左：方案 B——横向时间轴独立置顶卡 + 小时面板 + 活动流（矮卡）。
    auto* leftColumn = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(leftColumn);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(14);

    // —— 横向时间轴卡 ——
    auto* timelineCard = makeCard(leftColumn);
    auto* timelineLayout = new QVBoxLayout(timelineCard);
    timelineLayout->setContentsMargins(16, 12, 16, 8);
    timelineLayout->setSpacing(8);

    auto* timelineHead = new QWidget(timelineCard);
    auto* timelineHeadLayout = new QHBoxLayout(timelineHead);
    timelineHeadLayout->setContentsMargins(0, 0, 0, 0);
    timelineHeadLayout->setSpacing(10);
    timelineHeadLayout->addWidget(makeLabel(tr("时间轴"), 14, timelineHead, true));
    timelineHeadLayout->addWidget(makeLabel(tr("点色块看标题 · 双击放大看分轨"), 11, timelineHead));
    timelineHeadLayout->addStretch();
    _timelineModeButton = new ElaIconButton(ElaIconType::LayerGroup, 12, 26, 26, timelineHead);
    _timelineModeButton->setBorderRadius(8);
    _timelineModeButton->setToolTip(tr("切换 按应用 / 按分类"));
    connect(_timelineModeButton, &ElaIconButton::clicked, this,
            [this]() { setTimelineByCategory(!_timelineByCategory); });
    timelineHeadLayout->addWidget(_timelineModeButton);
    timelineLayout->addWidget(timelineHead);

    _timeline = new HistoryTimelineWidget(timelineCard);
    connect(_timeline, &HistoryTimelineWidget::segmentClicked,
            this, &HistoryPage::showSegmentPopover);
    connect(_timeline, &HistoryTimelineWidget::zoomRequested,
            this, &HistoryPage::openZoomDialog);
    timelineLayout->addWidget(_timeline);
    // 主视图是单轨色带，高度固定在 sizeHint（轨道 + 刻度），不会被内容拉高。
    _timeline->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    leftLayout->addWidget(timelineCard);

    // —— 小时活动面板 ——
    auto* hourlyCard = makeCard(leftColumn);
    auto* hourlyLayout = new QVBoxLayout(hourlyCard);
    hourlyLayout->setContentsMargins(16, 12, 16, 10);
    hourlyLayout->setSpacing(6);
    hourlyLayout->addWidget(makeLabel(tr("小时活动"), 13, hourlyCard, true));
    _hourlyBars = new HourlyBarsWidget(hourlyCard);
    _hourlyBars->setFixedHeight(110);
    hourlyLayout->addWidget(_hourlyBars);
    leftLayout->addWidget(hourlyCard);

    // —— 活动流（矮卡：随内容伸缩，外层整页滚动）——
    auto* feedCard = makeCard(leftColumn);
    auto* feedLayout = new QVBoxLayout(feedCard);
    feedLayout->setContentsMargins(16, 14, 16, 14);
    feedLayout->setSpacing(10);

    auto* feedHead = new QWidget(feedCard);
    auto* feedHeadLayout = new QHBoxLayout(feedHead);
    feedHeadLayout->setContentsMargins(0, 0, 0, 0);
    feedHeadLayout->setSpacing(10);
    feedHeadLayout->addWidget(makeLabel(tr("活动流"), 14, feedHead, true));
    feedHeadLayout->addStretch();
    _feedMetaLabel = makeLabel(QString(), 12, feedHead);
    feedHeadLayout->addWidget(_feedMetaLabel);
    feedLayout->addWidget(feedHead);

    // 活动流不自己滚：卡片高度跟着行数长，滚动交给整页的外壳（ElaScrollPage）。
    _feed = new HistoryFeedWidget(feedCard);
    _feed->setStyleSheet(QStringLiteral("background:transparent;"));
    feedLayout->addWidget(_feed);

    auto* moreRow = new QWidget(feedCard);
    auto* moreRowLayout = new QHBoxLayout(moreRow);
    moreRowLayout->setContentsMargins(0, 0, 0, 0);
    moreRowLayout->setSpacing(10);
    moreRowLayout->addStretch();
    _moreButton = new ElaPushButton(tr("加载更多"), moreRow);
    _moreButton->setFixedHeight(30);
    _moreButton->setMinimumWidth(96);
    _moreButton->setBorderRadius(9);
    connect(_moreButton, &ElaPushButton::clicked, this, &HistoryPage::loadMore);
    moreRowLayout->addWidget(_moreButton);
    _moreCountLabel = makeLabel(QString(), 12, moreRow);
    moreRowLayout->addWidget(_moreCountLabel);
    moreRowLayout->addStretch();
    feedLayout->addWidget(moreRow);

    leftLayout->addWidget(feedCard);
    // 活动流不再自己滚之后，左栏会被内容撑得很高。左栏与右栏必须都靠上：
    // 一旦这里留 addStretch，QHBoxLayout 会把左栏的内容在垂直方向居中，
    // 于是左栏越长、右侧「当日概览 / 应用分布」越往中间沉
    //（应用分布跑到中间、顶部留出一块空白）。让左栏自己吃掉多余高度即可。
    body->addWidget(leftColumn, 1, Qt::AlignTop);

    // 右：当日概览 + 应用分布。
    auto* side = new QWidget(this);
    side->setFixedWidth(kSidebarWidth);
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(12);

    auto* summaryCard = makeCard(side);
    auto* summaryLayout = new QVBoxLayout(summaryCard);
    summaryLayout->setContentsMargins(16, 14, 16, 14);
    summaryLayout->setSpacing(10);

    auto* summaryHead = new QWidget(summaryCard);
    auto* summaryHeadLayout = new QHBoxLayout(summaryHead);
    summaryHeadLayout->setContentsMargins(0, 0, 0, 0);
    summaryHeadLayout->setSpacing(10);
    summaryHeadLayout->addWidget(makeLabel(tr("当日概览"), 14, summaryHead, true));
    summaryHeadLayout->addStretch();
    summaryHeadLayout->addWidget(makeLabel(tr("已加载时段"), 12, summaryHead));
    summaryLayout->addWidget(summaryHead);

    auto* metrics = new QWidget(summaryCard);
    auto* metricsLayout = new QGridLayout(metrics);
    metricsLayout->setContentsMargins(0, 0, 0, 0);
    metricsLayout->setHorizontalSpacing(18);
    metricsLayout->setVerticalSpacing(10);
    const auto addMetric = [&](int row, int column, const QString& title, ElaText*& value) {
        auto* cell = new QWidget(metrics);
        auto* cellLayout = new QVBoxLayout(cell);
        cellLayout->setContentsMargins(0, 0, 0, 0);
        cellLayout->setSpacing(2);
        cellLayout->addWidget(makeLabel(title, 11, cell));
        value = makeLabel(QString(), 17, cell, true);
        cellLayout->addWidget(value);
        metricsLayout->addWidget(cell, row, column);
    };
    addMetric(0, 0, tr("活跃时长"), _metricTotal);
    addMetric(0, 1, tr("切换次数"), _metricSwitch);
    addMetric(1, 0, tr("应用数"), _metricApps);
    addMetric(1, 1, tr("分类数"), _metricCategories);
    summaryLayout->addWidget(metrics);

    _band = new HistoryDayBandWidget(summaryCard);
    summaryLayout->addWidget(_band);

    auto* ruler = new QWidget(summaryCard);
    auto* rulerLayout = new QHBoxLayout(ruler);
    rulerLayout->setContentsMargins(0, 0, 0, 0);
    rulerLayout->setSpacing(0);
    const QStringList ticks = {QStringLiteral("00"), QStringLiteral("06"), QStringLiteral("12"),
                               QStringLiteral("18"), QStringLiteral("24")};
    for (int i = 0; i < ticks.size(); ++i) {
        rulerLayout->addWidget(makeLabel(ticks.at(i), 11, ruler));
        if (i != ticks.size() - 1) {
            rulerLayout->addStretch();
        }
    }
    summaryLayout->addWidget(ruler);
    sideLayout->addWidget(summaryCard);

    auto* legendCard = makeCard(side);
    auto* legendCardLayout = new QVBoxLayout(legendCard);
    legendCardLayout->setContentsMargins(16, 14, 16, 14);
    legendCardLayout->setSpacing(10);

    // 标题与「按应用 / 按分类」分段控件同一行。
    // 分段控件放在图例卡上而不是时间轴卡上——它管的是右侧这份分布列表，
    // 时间轴那侧的按钮是同一开关的另一个入口（两处必须同步，见 setTimelineByCategory）。
    auto* legendHead = new QWidget(legendCard);
    auto* legendHeadLayout = new QHBoxLayout(legendHead);
    legendHeadLayout->setContentsMargins(0, 0, 0, 0);
    legendHeadLayout->setSpacing(8);
    _legendTitle = makeLabel(tr("应用分布"), 14, legendHead, true);
    legendHeadLayout->addWidget(_legendTitle);
    legendHeadLayout->addStretch();
    _legendModePivot = new ElaPivot(legendHead);
    _legendModePivot->setTextPixelSize(12);
    _legendModePivot->setPivotSpacing(12);
    _legendModePivot->setMarkWidth(18);
    _legendModePivot->appendPivot(tr("应用"));
    _legendModePivot->appendPivot(tr("分类"));
    _legendModePivot->setCurrentIndex(0);
    connect(_legendModePivot, &ElaPivot::pivotClicked, this, [this](int index) {
        setTimelineByCategory(index == 1);
    });
    legendHeadLayout->addWidget(_legendModePivot);
    legendCardLayout->addWidget(legendHead);

    auto* legendBody = new QWidget(legendCard);
    _legendLayout = new QVBoxLayout(legendBody);
    _legendLayout->setContentsMargins(0, 0, 0, 0);
    _legendLayout->setSpacing(7);
    // 图例不自己滚：行数多就往下长，滚动交给整页外壳。
    legendCardLayout->addWidget(legendBody);
    sideLayout->addWidget(legendCard);
    // 右栏同样不靠拉伸占位：body 里两栏都 AlignTop，多出来的高度由左栏吃掉。
    sideLayout->addStretch();

    body->addWidget(side, 0, Qt::AlignTop);
    rootLayout->addLayout(body, 1);

    // 段落详情气泡（7.1c）：轻量 QLabel，show 后靠定时关不掉的问题不存在——
    // 它是 Window 置顶小标签，点别处或切日时主动 hide。
    // 配色走 shared 取色：原来写死深底浅字，切到浅色主题就是一个突兀的黑块。
    _segmentPopover = new QLabel(this, Qt::ToolTip);
    _segmentPopover->setWordWrap(false);
    _segmentPopover->setMaximumWidth(360);
    refreshPopoverStyle(eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark);
    // Qt::ToolTip 窗口不会因「点别处」自动关（QToolTip::showText 的关闭逻辑
    // 只属于它自己），自建的必须自己监听：应用级过滤器，鼠标按下/滚轮/Esc
    // 且目标不是气泡本身 → hide。
    _segmentPopover->installEventFilter(this);
    qApp->installEventFilter(this);
}

void HistoryPage::setActive(bool active)
{
    if (_active == active) {
        return;
    }
    _active = active;
    if (!active) {
        // 页面不可见时释放整页数据。
        clearContent();
        _loaded = false;
        return;
    }
    if (!_loaded) {
        reload();
    }
}

void HistoryPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (_active && !_loaded) {
        reload();
    }
}

void HistoryPage::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    // 切到别的页面就丢掉明细：翻查多天时内存不随翻页累积。
    clearContent();
    _loaded = false;
}

bool HistoryPage::eventFilter(QObject* watched, QEvent* event)
{
    // 时间轴详情气泡的自动关闭（应用级）：点别处 / 滚轮 / Esc 都收掉气泡；
    // 气泡自己的鼠标事件放行（不影响它里面的文本选择）。
    if (_segmentPopover != nullptr && _segmentPopover->isVisible()
        && watched != _segmentPopover) {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::Wheel:
        case QEvent::KeyPress: {
            auto* keyEvent = event->type() == QEvent::KeyPress
                ? static_cast<QKeyEvent*>(event) : nullptr;
            if (keyEvent == nullptr || keyEvent->key() == Qt::Key_Escape) {
                _segmentPopover->hide();
            }
            break;
        }
        default:
            break;
        }
    }
    // 气泡被隐藏时顺手解绑应用级监听？——不解：installEventFilter 是幂等集合，
    // 反复 show/hide 期间保持监听最简单，气泡析构（页面析构）时 Qt 自动清理。
    return QWidget::eventFilter(watched, event);
}

void HistoryPage::selectDate(const QDate& date)
{
    if (!date.isValid()) {
        return;
    }
    _date = date;
    _loaded = false;
    clearContent();
    updateDateUi();
    reload();
}

void HistoryPage::updateDateUi()
{
    _dateLabel->setText(_date.toString(QStringLiteral("yyyy 年 M 月 d 日")));

    static const QStringList kWeekdays = {tr("星期一"), tr("星期二"), tr("星期三"), tr("星期四"),
                                          tr("星期五"), tr("星期六"), tr("星期日")};
    _weekdayLabel->setText(kWeekdays.value(_date.dayOfWeek() - 1));

    const QDate today = QDate::currentDate();
    _nextButton->setEnabled(_date < today);
    _todayButton->setEnabled(_date != today);

    // ElaCalendarPicker::setSelectedDate 会发 selectedDateChanged，回填时先关掉回环。
    _updatingPicker = true;
    _datePicker->setSelectedDate(_date);
    _updatingPicker = false;
}

void HistoryPage::reload()
{
    if (!_database || !_database->isOpen()) {
        applyEmptyState(tr("数据库未就绪，无法读取历史记录。"));
        return;
    }

    _dayStartMs = _date.startOfDay().toMSecsSinceEpoch();
    _dayEndMs = _date.addDays(1).startOfDay().toMSecsSinceEpoch();

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    _cutoffMs = (_date >= QDate::currentDate()) ? std::min(nowMs, _dayEndMs) : _dayEndMs;
    if (_cutoffMs <= _dayStartMs) {
        applyEmptyState(tr("这一天还没有开始。"));
        return;
    }

    // 明细段只带 app_id，显示名/分类/颜色/统计开关都得先备好一张表。
    // 这里直接取 apps 全表：关掉统计的应用不会出现在聚合结果里，
    // 但它们的段仍在 sessions 中，必须靠 trackEnabled 才判断得出「不该画」。
    _appMetas.clear();
    {
        data::ClassificationRepository repository(*_database);
        const data::Result<QVector<data::AppClassificationEntry>> apps = repository.listApps();
        if (apps.ok()) {
            for (const data::AppClassificationEntry& app : apps.value) {
                AppMeta meta;
                meta.exeName = app.exeName;
                meta.displayName = app.displayName;
                meta.categoryId = app.categoryId;
                meta.colorHex = app.colorHex;
                meta.trackEnabled = app.trackEnabled;
                meta.hidden = app.hidden;
                _appMetas.insert(app.appId, meta);
            }
        }
    }

    // 分类展示值（名字/颜色）走注册表快照：分类管理页的改名与配色存在覆盖键里，
    // 内置 token 表读不到。快照读失败就留空，后面取色取名自然退回内置表。
    {
        const data::Result<data::CategoryRegistrySnapshot> registry =
            data::loadCategoryRegistrySnapshot(*_database);
        _categoryRegistry = registry.ok() ? registry.value : data::CategoryRegistrySnapshot{};
    }

    // 成员应用的身份换成父应用。放在读概览之前——概览按 appId 建条目，
    // 归并后同一父应用下的几行会合并成一行，图例与时间轴自然跟上。
    applyAppLinksToMetas();

    // 概览走一次聚合查询：返回的是按应用/分类汇总后的结果，条目数只跟应用数有关，
    // 不跟段数有关，所以明细可以分页、汇总不必。
    data::DashboardReadModel model(*_database);
    const data::Result<data::TodayOverview> overview = model.loadToday(_dayStartMs, _cutoffMs);
    if (!overview.ok()) {
        applyEmptyState(tr("读取历史数据失败：%1").arg(overview.error));
        return;
    }
    applyOverview(overview.value);

    _hasCursor = false;
    _cursorStartMs = 0;
    _cursorId = 0;
    _exhausted = false;
    _capped = false;
    _loaded = true;

    appendPage();
    rebuildDisplaySegments();
    // 时间轴的数据源与活动流分开：活动流分页，时间轴要整天。
    loadTimelineSegments();
    rebuildFeed();
    rebuildTimeline();
    refreshHourlyPanel();
    updatePagingUi();
}

void HistoryPage::applyOverview(const data::TodayOverview& overview)
{
    _statusLabel->setVisible(false);

    // 聚合查询已把未闭合段按 cutoffMs 封底计入，进行中的那一段不需要再补时长，
    // 否则「活跃时长」会比概览页多出一段。
    struct Entry
    {
        AppMeta meta;
        qint64 durationMs = 0;
    };

    QVector<Entry> entries;
    entries.reserve(overview.apps.size());

    int segmentCount = 0;
    for (const data::AppUsage& usage : overview.apps) {
        // 不在 _appMetas 里的应用才补：整表那份带着 trackEnabled，覆盖掉就丢了。
        if (!_appMetas.contains(usage.appId)) {
            AppMeta meta;
            meta.exeName = usage.exeName;
            meta.displayName = usage.displayName;
            meta.categoryId = usage.categoryId;
            meta.colorHex = usage.colorHex;
            _appMetas.insert(usage.appId, meta);
        }

        Entry entry;
        entry.meta = _appMetas.value(usage.appId);
        entry.durationMs = usage.durationMs;
        entries.append(entry);
        segmentCount += usage.segmentCount;
    }

    // 聚合已把未闭合段按 cutoff 封底计入（读模型的 SQL 带 COALESCE），
    // 所以这里不能再加一遍时长/段数，只把 live 的显示名同步进 _appMetas：
    // 读模型的主查询已经带出 display_name，但分类管理页刚改的名字要等这次刷新才生效。
    if (overview.live.has_value()) {
        const data::LiveSession& live = overview.live.value();
        // 关掉统计 / 已隐藏的应用：主查询已把它排除在 apps 之外，这里不必再补。
        const AppMeta liveMetaCheck = appMetaFor(live.appId);
        if (live.startTimeMs < _dayEndMs && liveMetaCheck.trackEnabled && !liveMetaCheck.hidden) {
            AppMeta liveMeta = _appMetas.value(live.appId);
            liveMeta.exeName = live.exeName;
            liveMeta.displayName = live.displayName;
            _appMetas.insert(live.appId, liveMeta);
        }
    }

    qint64 totalDurationMs = 0;
    QSet<QString> categoryIds;
    for (const Entry& entry : entries) {
        totalDurationMs += entry.durationMs;
        categoryIds.insert(effectiveCategoryId(entry.meta));
    }

    _daySegmentCount = segmentCount;
    _dayAppCount = static_cast<int>(entries.size());
    _dayCategoryCount = static_cast<int>(categoryIds.size());

    _metricTotal->setText(formatDurationShort(totalDurationMs));
    _metricSwitch->setText(tr("%1 次").arg(std::max(0, segmentCount - 1)));
    _metricApps->setText(tr("%1 个").arg(_dayAppCount));
    _metricCategories->setText(tr("%1 类").arg(_dayCategoryCount));

    // 与读模型同样的排序口径：时长降序，相同时按 exeName 升序。
    std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
        if (left.durationMs != right.durationMs) {
            return left.durationMs > right.durationMs;
        }
        return QString::compare(left.meta.exeName, right.meta.exeName, Qt::CaseInsensitive) < 0;
    });

    // 概览原样存一份：切「按应用 / 按分类」只重排图例，不重查库。
    _legendEntries.clear();
    _legendEntries.reserve(entries.size());
    for (const Entry& entry : entries) {
        LegendEntry legend;
        legend.label = shownNameFor(entry.meta);
        legend.colorHex = effectiveColorHex(entry.meta);
        legend.durationMs = entry.durationMs;
        legend.appExeName = entry.meta.exeName;
        legend.categoryId = effectiveCategoryId(entry.meta);
        _legendEntries.append(legend);
    }
    rebuildLegend();
}

void HistoryPage::rebuildLegend()
{
    clearLegend();

    if (!_timelineByCategory) {
        // 应用维度：概览已经按时长降序排好，直接用。
        for (const LegendEntry& entry : _legendEntries) {
            appendLegendRow(entry.label, entry.colorHex, entry.durationMs, entry.appExeName,
                            QString());
        }
        return;
    }

    // 分类维度：同一分类下的应用时长要先并起来。分类标签/颜色走注册表快照，
    // 与时间轴 lane 取的是同一套值（effectiveCategoryId + categoryColorFor）。
    struct Bucket
    {
        QString categoryId;
        qint64 durationMs = 0;
        QString label;
        QString colorHex;
    };
    QVector<Bucket> buckets;
    QHash<QString, int> bucketIndex;
    for (const LegendEntry& entry : _legendEntries) {
        const QString categoryId = entry.categoryId;
        int index = bucketIndex.value(categoryId, -1);
        if (index < 0) {
            Bucket bucket;
            bucket.categoryId = categoryId;
            bucket.label = categoryLabelFor(categoryId);
            bucket.colorHex = categoryColorFor(categoryId);
            index = buckets.size();
            bucketIndex.insert(categoryId, index);
            buckets.append(bucket);
        }
        buckets[index].durationMs += entry.durationMs;
    }

    std::sort(buckets.begin(), buckets.end(),
              [](const Bucket& left, const Bucket& right) {
                  if (left.durationMs != right.durationMs) {
                      return left.durationMs > right.durationMs;
                  }
                  return left.categoryId < right.categoryId;
              });
    for (const Bucket& bucket : buckets) {
        appendLegendRow(bucket.label, bucket.colorHex, bucket.durationMs, QString(),
                        bucket.categoryId);
    }
}

void HistoryPage::setTimelineByCategory(bool byCategory)
{
    if (_timelineByCategory == byCategory) {
        return;
    }
    _timelineByCategory = byCategory;
    QSettings().setValue(QStringLiteral("history/timelineByCategory"), byCategory);

    // 两个入口（时间轴卡上的图标按钮、图例卡上的分段控件）共用同一个开关，
    // 这里把两边的显示同步过来，否则点了一处另一处还是旧状态。
    if (_legendModePivot != nullptr) {
        const QSignalBlocker blocker(_legendModePivot);
        _legendModePivot->setCurrentIndex(byCategory ? 1 : 0);
    }
    if (_legendTitle != nullptr) {
        _legendTitle->setText(byCategory ? tr("分类分布") : tr("应用分布"));
    }

    rebuildLegend();
    rebuildTimeline();
    rebuildFeed();
}

void HistoryPage::appendLegendRow(const QString& label, const QString& colorHex,
                                  qint64 durationMs, const QString& appExeName,
                                  const QString& categoryId)
{
    auto* row = new QPushButton(_legendLayout->parentWidget());
    row->setFlat(true);
    row->setCursor(Qt::PointingHandCursor);
    row->setToolTip(categoryId.isEmpty() ? tr("查看应用详情") : tr("查看分类详情"));
    row->setStyleSheet(QStringLiteral(
                           "QPushButton { text-align:left; border:none; padding:2px 0;"
                           " background:transparent; }"
                           "QPushButton:hover { color:%1; }")
                           .arg(shared::accentColor(eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark)));

    auto* content = new QWidget(row);
    auto* contentLayout = new QHBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(8);

    auto* dot = new QLabel(content);
    dot->setFixedSize(kLegendDotSize, kLegendDotSize);
    dot->setStyleSheet(QStringLiteral("background:%1; border-radius:3px;").arg(colorHex));
    contentLayout->addWidget(dot);
    contentLayout->addWidget(makeLabel(label, 12, content), 1);
    contentLayout->addWidget(makeLabel(formatDurationShort(durationMs), 12, content, true));

    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->addWidget(content);

    if (categoryId.isEmpty()) {
        connect(row, &QPushButton::clicked, this,
                [this, appExeName]() { openAppDetail(appExeName); });
    } else {
        connect(row, &QPushButton::clicked, this,
                [this, categoryId]() { openCategoryDetail(categoryId); });
    }

    _legendLayout->addWidget(row);
}

void HistoryPage::openAppDetail(const QString& exeName)
{
    if (_database == nullptr || !_database->isOpen() || exeName.trimmed().isEmpty()) {
        return;
    }
    DestinationTargetInfo info;
    info.target.mode = data::DestinationTarget::Mode::App;
    info.target.key = exeName.trimmed();
    info.displayName = exeName.trimmed();
    info.secondaryText = exeName.trimmed();
    // 兜底色走主题的次要文字色：没有分类信息时用它画那个小方块，
    // 写死浅色的灰在深色主题下会显得偏白。
    info.color = QColor(shared::secondaryTextColor(
        eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark));

    // 显示名与颜色走 apps 覆盖项。
    data::AppRepository apps(*_database);
    const data::Result<std::optional<data::App>> app = apps.findByExeName(exeName);
    if (app.ok() && app.value.has_value()) {
        if (!app.value->displayName.trimmed().isEmpty()) {
            info.displayName = app.value->displayName.trimmed();
        }
        const QString categoryId = resolveRegistryCategoryId(
            data::resolveCategoryId(app.value->exeName, app.value->categoryId));
        info.color = app.value->colorHex.trimmed().isEmpty()
            ? QColor(categoryColorFor(categoryId)) : QColor(app.value->colorHex.trimmed());
    }
    if (_detailDialog == nullptr) {
        _detailDialog = new DestinationDetailDialog(_database, this);
    }
    _detailDialog->openFor(info, _date);
}

void HistoryPage::clearLegend()
{
    while (_legendLayout->count() > 0) {
        QLayoutItem* item = _legendLayout->takeAt(0);
        if (item->widget() != nullptr) {
            delete item->widget();
        }
        delete item;
    }
}

void HistoryPage::openCategoryDetail(const QString& categoryId)
{
    if (_database == nullptr || !_database->isOpen() || categoryId.trimmed().isEmpty()) {
        return;
    }
    const QString resolved = resolveRegistryCategoryId(categoryId);
    DestinationTargetInfo info;
    info.target.mode = data::DestinationTarget::Mode::Category;
    info.target.key = resolved;
    info.displayName = categoryLabelFor(resolved);
    info.secondaryText = tr("分类");
    info.color = QColor(categoryColorFor(resolved));

    if (_detailDialog == nullptr) {
        _detailDialog = new DestinationDetailDialog(_database, this);
    }
    _detailDialog->openFor(info, _date);
}

void HistoryPage::refreshPopoverStyle(bool dark)
{
    if (_segmentPopover == nullptr) {
        return;
    }
    // ToolTip 是顶层窗口，继承不到页面的调色板，只能自己写全。
    _segmentPopover->setStyleSheet(QStringLiteral("QLabel { background:%1; color:%2;"
                                                  " border-radius:8px; padding:8px 10px; }")
                                       .arg(shared::popupBackgroundColor(dark),
                                            shared::primaryTextColor(dark)));
}

void HistoryPage::refreshLegendStyle(bool dark)
{
    // 图例行在每次 applyOverview 时重建，重建时按当时的主题写样式；
    // 主题切换后调用这里把已有行刷一遍。
    const QString accent = shared::accentColor(dark);
    const QString style = QStringLiteral(
                              "QPushButton { text-align:left; border:none; padding:2px 0;"
                              " background:transparent; }"
                              "QPushButton:hover { color:%1; }")
                              .arg(accent);
    for (int index = 0; index < _legendLayout->count(); ++index) {
        QLayoutItem* item = _legendLayout->itemAt(index);
        if (item != nullptr && item->widget() != nullptr) {
            item->widget()->setStyleSheet(style);
        }
    }
}

void HistoryPage::loadMore()
{
    if (!_loaded) {
        return;
    }
    appendPage();
    rebuildDisplaySegments();
    rebuildFeed();
    // 时间轴用整天全量段，翻页不影响它，不必重建。
    updatePagingUi();
}

bool HistoryPage::appendPage()
{
    if (_exhausted || !_loaded) {
        return false;
    }

    data::SessionRepository repository(*_database);
    int added = 0;

    // keyset 推进：listRange 只按 (start_time_ms, id) 升序返回，没有 id 游标参数，
    // 所以用「上一页最后一行的 start_time_ms」当下界，再在本地按 (start, id) 跳过
    // 已经读过的行。跨下界但仍在进行的段会重复出现一次，同样被去重挡掉。
    for (int round = 0; round < 4; ++round) {
        const qint64 fromMs = _hasCursor ? _cursorStartMs : _dayStartMs;
        const data::Result<QVector<data::SessionSegment>> page =
            repository.listRange(fromMs, _dayEndMs, kPageSize);
        if (!page.ok()) {
            _statusLabel->setText(tr("读取历史明细失败：%1").arg(page.error));
            _statusLabel->setVisible(true);
            _exhausted = true;
            break;
        }

        const QVector<data::SessionSegment>& segments = page.value;
        if (segments.isEmpty()) {
            _exhausted = true;
            break;
        }

        for (const data::SessionSegment& segment : segments) {
            const bool alreadyRead = _hasCursor
                                     && (segment.startTimeMs < _cursorStartMs
                                         || (segment.startTimeMs == _cursorStartMs
                                             && segment.id <= _cursorId));
            if (alreadyRead || _seenIds.contains(segment.id)) {
                continue;
            }
            _seenIds.insert(segment.id);
            // 关掉统计的应用：段还在库里，但概览不计它，明细流里也不该画，
            // 否则上方的时长和下面的行对不上。在这里挡掉，_segments 只装要画的行。
            // 「不在列表显示」的应用同理——它在所有页面都不出现。
            const AppMeta meta = appMetaFor(segment.appId);
            if (!meta.trackEnabled || meta.hidden) {
                continue;
            }
            _segments.append(segment);
            ++added;
        }

        const data::SessionSegment& last = segments.last();
        _cursorStartMs = last.startTimeMs;
        _cursorId = last.id;
        _hasCursor = true;

        if (segments.size() < kPageSize) {
            _exhausted = true;
            break;
        }
        if (added > 0) {
            break;
        }
        // 整页都是同一毫秒上的旧行：SQL 无法表达 id 游标，把下界抬一格跳过去。
        // 正常数据里一段至少几百毫秒，不会走到这里。
        _cursorStartMs = last.startTimeMs + 1;
        _cursorId = 0;
    }

    // 单日保留量到顶就停：继续读只会让内存随翻页增长，界面会明示「只显示最近这么多段」。
    if (_segments.size() >= static_cast<qsizetype>(kMaxRetainedSegments)) {
        _exhausted = true;
        _capped = true;
    }

    return added > 0;
}

void HistoryPage::loadTimelineSegments()
{
    _timelineSegments.clear();
    if (_database == nullptr || !_database->isOpen()) {
        return;
    }

    data::SessionRepository repository(*_database);
    const data::Result<QVector<data::SessionSegment>> all =
        repository.listRange(_dayStartMs, _dayEndMs, kMaxTimelineSegments);
    if (!all.ok()) {
        // 时间轴读失败不该连累活动流：上面已经画出来的照旧，这里静默留空。
        // 用户还能从活动流和右侧概览看到数字，不至于整页空白。
        return;
    }

    _timelineSegments.reserve(all.value.size());
    for (const data::SessionSegment& segment : all.value) {
        // 与 appendPage 同一套过滤：关掉统计的、隐藏的应用都不该出现在时间轴上。
        const AppMeta meta = appMetaFor(segment.appId);
        if (!meta.trackEnabled || meta.hidden) {
            continue;
        }
        _timelineSegments.append(segment);
    }
    _timelineSegments.squeeze();
}

void HistoryPage::rebuildDisplaySegments()
{
    _displaySegments.clear();
    _hiddenShortCount = 0;
    if (_segments.isEmpty()) {
        return;
    }

    const qint64 minDurationMs = std::max<qint64>(0, _minSessionSecs) * 1000;

    // 第一遍：丢掉短于阈值的切换抖动。
    // 实时段一律保留——用户此刻正在用的应用不该因为还没满 5 秒就消失。
    QVector<data::SessionSegment> kept;
    kept.reserve(_segments.size());
    for (const data::SessionSegment& segment : _segments) {
        if (segment.isOpen() || segmentDurationMs(segment) >= minDurationMs) {
            kept.append(segment);
        } else {
            ++_hiddenShortCount;
        }
    }

    // 第二遍：抖动被摘掉之后，同应用的段可能重新变成相邻，把它们黏回去。
    // 例：A 用了十分钟 → 切到 B 两秒 → 切回 A 继续用，
    // 摘掉 B 之后 A 的两段只隔两秒，合成一行才是「一直在用 A」。
    // 间隔阈值取 5 秒：
    // 再宽就会把「离开两分钟又回来」也并成一段，那是真实空档，不该抹掉。
    // 选「显示全部」时不做这一步——用户要看的正是每一次切换。
    //
    // 只作用于活动流：时间轴读 _timelineSegments，不受这里影响。
    const qint64 mergeGapMs = minDurationMs > 0 ? kDirectMergeGapMs : 0;
    _displaySegments.reserve(kept.size());
    for (const data::SessionSegment& segment : kept) {
        if (!_displaySegments.isEmpty()) {
            data::SessionSegment& last = _displaySegments.last();
            const qint64 lastEndMs = last.endTimeMs.value_or(last.startTimeMs);
            const qint64 gapMs = segment.startTimeMs - lastEndMs;
            // 上一段还没闭合就不再往后并：它后面理论上不会有段，真有异常数据也不该
            // 把别人的时长算进「进行中」那一段。
            if (!last.isOpen() && last.appId == segment.appId && gapMs >= 0
                && gapMs <= mergeGapMs) {
                if (segment.isOpen()) {
                    // 并进来的这一段还开着，合并结果同样开着。
                    last.endTimeMs.reset();
                } else if (last.endTimeMs.has_value()) {
                    last.endTimeMs = std::max(*last.endTimeMs,
                                              segment.endTimeMs.value_or(segment.startTimeMs));
                }
                // 标题取更晚那段的（用户最后看到的内容）。
                if (!segment.windowTitle.trimmed().isEmpty()) {
                    last.windowTitle = segment.windowTitle;
                }
                continue;
            }
        }
        _displaySegments.append(segment);
    }
}

void HistoryPage::loadMinSessionSetting()
{
    qint64 secs = _minSessionSecs;
    bool stored = false;
    if (_database != nullptr && _database->isOpen()) {
        data::SettingsRepository settings(*_database);
        const data::Result<std::optional<QString>> value =
            settings.value(engine::minSessionSecsKey());
        if (value.ok() && value.value.has_value()) {
            bool ok = false;
            const qint64 parsed = value.value->toLongLong(&ok);
            if (ok && parsed >= 0) {
                secs = parsed;
                stored = true;
            }
        }
    }
    _minSessionSecs = secs;

    if (_minSessionBox != nullptr) {
        // 值不在预设列表里（比如手改过库）就补一个选项，避免下拉显示空白。
        int index = -1;
        for (int i = 0; i < _minSessionBox->count(); ++i) {
            if (_minSessionBox->itemData(i).toLongLong() == secs) {
                index = i;
                break;
            }
        }
        if (index < 0) {
            _minSessionBox->addItem(tr("≥ %1 秒").arg(secs), QVariant::fromValue<qint64>(secs));
            index = _minSessionBox->count() - 1;
        }
        // 回填会触发 currentIndexChanged，先关掉回环（走 _loaded 判断不够精确）。
        const QSignalBlocker blocker(_minSessionBox);
        _minSessionBox->setCurrentIndex(index);
    }

    // 库里还没有这个键（新装 / 从没动过下拉）：把当前默认值写回去，
    // 这样下次启动读到的就是同一档，不会出现「这次 30 秒、下次 5 秒」。
    if (!stored && _database != nullptr && _database->isOpen()) {
        data::SettingsRepository settings(*_database);
        settings.setValue(engine::minSessionSecsKey(), QString::number(_minSessionSecs));
    }
}

void HistoryPage::applyMinSessionSetting(qint64 secs)
{
    _minSessionSecs = std::max<qint64>(0, secs);
    if (_database != nullptr && _database->isOpen()) {
        data::SettingsRepository settings(*_database);
        settings.setValue(engine::minSessionSecsKey(), QString::number(_minSessionSecs));
    }
    if (!_loaded) {
        return;
    }
    rebuildDisplaySegments();
    rebuildFeed();
    // 时间轴不吃这个阈值（见 _timelineSegments 的注释），改了阈值不必重建色带。
    updatePagingUi();
}

void HistoryPage::rebuildFeed()
{
    QVector<HistoryFeedRow> rows;
    rows.reserve(_displaySegments.size() * 2 + 4);

    QVector<HistoryDayBandWidget::Block> blocks;
    blocks.reserve(_displaySegments.size());

    // 活动流倒序展示（最新在最上）：按时间降序遍历，
    // 「当前运行的」自然排在流的最前面。色带仍按时间正序画，不受影响。
    // 倒序下的 hour 分隔与空档判断都与正序镜像：
    //   hour 分隔   —— 降序中第一次经过某小时时插 header（即该小时最晚的段前）；
    //   空档        —— 这一行（更早）的 end 与上一行（更晚）的 start 之间的间隔。
    QVector<data::SessionSegment> ordered;
    ordered.reserve(_displaySegments.size());
    for (auto it = _displaySegments.crbegin(); it != _displaySegments.crend(); ++it) {
        ordered.append(*it);
    }

    int lastHour = -1;
    // 降序遍历中「上一行」的时间信息：上一行（更晚的段）的开始时刻。
    bool hasLater = false;
    qint64 laterStartMs = 0;

    for (const data::SessionSegment& segment : ordered) {
        const qint64 startMs = std::max(segment.startTimeMs, _dayStartMs);
        const qint64 durationMs = segmentDurationMs(segment);
        const qint64 endMs = startMs + durationMs;
        const bool live = segment.isOpen() && _cutoffMs < _dayEndMs;
        const AppMeta meta = appMetaFor(segment.appId);

        const int hour = QDateTime::fromMSecsSinceEpoch(startMs).time().hour();
        if (hour != lastHour) {
            HistoryFeedRow header;
            header.kind = HistoryFeedRow::Kind::HourHeader;
            header.hourText = QStringLiteral("%1:00").arg(hour, 2, 10, QLatin1Char('0'));
            rows.append(header);
            lastHour = hour;
        }

        if (hasLater && laterStartMs - endMs >= kGapThresholdMs) {
            HistoryFeedRow gap;
            gap.kind = HistoryFeedRow::Kind::Gap;
            gap.startTimeMs = endMs;
            gap.primary = tr("无记录 %1 分钟").arg((laterStartMs - endMs) / 60000);
            gap.secondary = tr("离开或空闲，不计入活跃时长");
            rows.append(gap);
        }

        HistoryFeedRow row;
        row.kind = HistoryFeedRow::Kind::Event;
        row.startTimeMs = startMs;
        row.primary = shownNameFor(meta);
        row.secondary = segment.windowTitle.isEmpty() ? tr("（无窗口标题）") : segment.windowTitle;
        // 分类名跟着覆盖项走：改完分类这里立刻是新分类，和左侧图例一致。
        row.tertiary = tr("%1 · %2").arg(categoryLabelFor(effectiveCategoryId(meta)),
                                         meta.exeName);
        row.trailing = live ? tr("进行中") : formatDurationLong(durationMs);
        row.dotColor = QColor(effectiveColorHex(meta));
        rows.append(row);

        laterStartMs = startMs;
        hasLater = true;

        HistoryDayBandWidget::Block block;
        block.startMs = startMs;
        block.endMs = endMs;
        block.color = row.dotColor;
        if (block.endMs > block.startMs) {
            blocks.append(block);
        }
    }

    _feed->setRows(rows);
    _band->setBlocks(_dayStartMs, _dayEndMs, blocks);
}

qint64 HistoryPage::segmentDurationMs(const data::SessionSegment& segment) const
{
    const qint64 startMs = std::max(segment.startTimeMs, _dayStartMs);
    // 未闭合段按当前时刻裁剪，已闭合段裁到当天末尾；跨零点段两头都裁。
    const qint64 endMs = segment.isOpen() ? _cutoffMs
                                          : std::min(segment.endTimeMs.value_or(_cutoffMs), _dayEndMs);
    return std::max<qint64>(0, endMs - startMs);
}

HistoryPage::AppMeta HistoryPage::appMetaFor(qint64 appId) const
{
    const auto it = _appMetas.constFind(appId);
    if (it == _appMetas.constEnd()) {
        // 读模型只返回当天有段的应用；分页读到的旧段可能属于更早的应用，
        // 这时只拿得到 app_id，名字留空让 shownNameFor 走默认文案。
        return AppMeta();
    }
    return it.value();
}

void HistoryPage::applyAppLinksToMetas()
{
    if (_database == nullptr || !_database->isOpen() || _appMetas.isEmpty()) {
        return;
    }

    const data::Result<data::AppLinks> loadedLinks = data::loadAppLinks(*_database);
    if (!loadedLinks.ok() || loadedLinks.value.isEmpty()) {
        return;
    }
    const data::AppLinks& links = loadedLinks.value;
    const data::Result<QHash<QString, data::AppIdentity>> loadedIdentities =
        data::loadAppIdentities(*_database);
    if (!loadedIdentities.ok()) {
        return;
    }

    for (auto it = _appMetas.begin(); it != _appMetas.end(); ++it) {
        const std::optional<data::AppIdentity> parent =
            data::linkedIdentity(it.value().exeName, links, loadedIdentities.value);
        if (!parent.has_value()) {
            continue;
        }
        // 只换身份字段，trackEnabled / hidden 保持成员自己的：用户关掉某个成员的
        // 统计或把它隐藏，那部分时长本来就不该进任何汇总，换身份也不能把它变回来。
        it.value().exeName = parent->exeName;
        it.value().displayName = parent->displayName;
        it.value().categoryId = parent->categoryId;
        it.value().colorHex = parent->colorHex;
    }
}

QString HistoryPage::shownNameFor(const AppMeta& meta) const
{
    return meta.displayName.trimmed().isEmpty() ? displayNameFor(meta.exeName)
                                                : meta.displayName.trimmed();
}

QString HistoryPage::effectiveCategoryId(const AppMeta& meta) const
{
    // 和 data::resolveCategoryId 同一判据：覆盖项优先，空串回落到内置规则。
    // 再按注册表快照归一：被删/被墓碑的分类 id 统一落 other。
    return resolveRegistryCategoryId(data::resolveCategoryId(meta.exeName, meta.categoryId));
}

QString HistoryPage::effectiveColorHex(const AppMeta& meta) const
{
    const QString customColor = meta.colorHex.trimmed();
    if (!customColor.isEmpty()) {
        return customColor;
    }
    return categoryColorFor(effectiveCategoryId(meta));
}

QString HistoryPage::categoryLabelFor(const QString& categoryId) const
{
    if (_categoryRegistry.entries.isEmpty()) {
        return data::categoryToken(categoryId).label;
    }
    return _categoryRegistry.labelFor(categoryId);
}

QString HistoryPage::categoryColorFor(const QString& categoryId) const
{
    if (_categoryRegistry.entries.isEmpty()) {
        return data::categoryToken(categoryId).colorHex;
    }
    return _categoryRegistry.colorFor(categoryId);
}

QString HistoryPage::resolveRegistryCategoryId(const QString& categoryId) const
{
    if (_categoryRegistry.entries.isEmpty()) {
        return categoryId;
    }
    return _categoryRegistry.resolveId(categoryId);
}

void HistoryPage::updatePagingUi()
{
    const int shown = static_cast<int>(_segments.size());
    const int total = std::max(shown, _daySegmentCount);

    if (_exhausted) {
        _moreButton->setEnabled(false);
        _moreButton->setText(tr("已到底"));
        if (shown == 0) {
            _moreCountLabel->setText(tr("本日没有记录"));
        } else if (_capped) {
            _moreCountLabel->setText(tr("已达单日显示上限 %1 段").arg(shown));
        } else {
            _moreCountLabel->setText(tr("本日已全部载入"));
        }
    } else {
        _moreButton->setEnabled(true);
        _moreButton->setText(tr("加载更多"));
        _moreCountLabel->setText(tr("还有 %1 段").arg(std::max(0, total - shown)));
    }

    _feedMetaLabel->setText(total > 0 ? tr("已加载 %1 / %2 段").arg(shown).arg(total)
                                      : tr("已加载 %1 段").arg(shown));
    // 被最短时长挡掉的抖动段要说明一下，否则用户会以为数据丢了。
    if (_hiddenShortCount > 0) {
        const QString threshold = (_minSessionSecs >= 60 && _minSessionSecs % 60 == 0)
            ? tr("%1 分钟").arg(_minSessionSecs / 60)
            : tr("%1 秒").arg(_minSessionSecs);
        _feedMetaLabel->setText(_feedMetaLabel->text()
                                + tr(" · 已隐藏 %1 个短于 %2 的切换")
                                      .arg(_hiddenShortCount)
                                      .arg(threshold));
    }
    // 顶部的段数/切换次数/应用数仍按聚合口径算真实值。
    // 概览不过滤，只有活动流与时间轴走显示阈值。
    // 那次切换确实发生过，只是不值得占一行；改小反而和应用分布图例对不上。
    _chipLabel->setText(tr("共 %1 段 · %2 个应用").arg(_daySegmentCount).arg(_dayAppCount));
}

void HistoryPage::applyEmptyState(const QString& message)
{
    _statusLabel->setText(message);
    _statusLabel->setVisible(!message.isEmpty());

    _feed->clear();
    _band->clear();
    clearLegend();
    // 空态下切分布模式不能把上一份数据又画回来。
    _legendEntries.clear();
    _timeline->clear();
    _timelineLanes.clear();
    _timelineLanes.squeeze();
    _timelineSegments.clear();
    _timelineSegments.squeeze();

    _metricTotal->setText(QString());
    _metricSwitch->setText(QString());
    _metricApps->setText(QString());
    _metricCategories->setText(QString());
    _feedMetaLabel->setText(QString());
    _moreCountLabel->setText(QString());
    _chipLabel->setText(QString());

    _moreButton->setEnabled(false);
    _moreButton->setText(tr("加载更多"));
}

void HistoryPage::clearContent()
{
    _segments.clear();
    _segments.squeeze();
    _displaySegments.clear();
    _displaySegments.squeeze();
    _timelineSegments.clear();
    _timelineSegments.squeeze();
    _hiddenShortCount = 0;
    _seenIds.clear();
    _appMetas.clear();
    _timelineLanes.clear();
    _timelineLanes.squeeze();
    _categoryRegistry = data::CategoryRegistrySnapshot{};
    _daySegmentCount = 0;
    _dayAppCount = 0;
    _dayCategoryCount = 0;
    _capped = false;
    _hasCursor = false;
    _cursorStartMs = 0;
    _cursorId = 0;
    _exhausted = false;

    _feed->clear();
    _band->clear();
    _timeline->clear();
    _hourlyBars->clear();
    _legendEntries.clear();
    _legendEntries.squeeze();
    if (_segmentPopover != nullptr) {
        _segmentPopover->hide();
    }
    clearLegend();
}

// —— 横向时间轴 ——

namespace
{

// 每分钟取一个「主导项」：同一分钟里谁占的时间最长就画谁。
//
// 主视图是单条色轨，任何时刻只能有一种颜色，所以必须先按分钟归并再画。
// 直接把每个应用的每一段都画上去（旧实现）会出现同一时刻多行重叠，
// 24h 视口下每块常常不足 3px，看起来就是「时间完全不对、看不清时间段」。
//
// 排序规则：时长降序 → 先出现的优先 →
// key 字典序（保证结果稳定，不随容器遍历顺序抖）。
struct MinuteCandidate
{
    QString key;
    QString label;
    QString detail;
    QColor color;
    bool isLive = false;
    qint64 durationMs = 0;
    qint64 firstSeenAtMs = 0;
};

constexpr qint64 kMinuteMs = 60 * 1000;
// 距分钟边界 1 秒以内就算压线：
// 采样间隔让段起点常常差几十毫秒，不吸附的话每分钟都会多出零点几秒的空隙。
constexpr qint64 kMinuteBoundarySnapMs = 1000;

qint64 minuteStartOf(qint64 timeMs, qint64 dayStartMs)
{
    return dayStartMs + ((timeMs - dayStartMs) / kMinuteMs) * kMinuteMs;
}

qint64 snapToMinuteBoundary(qint64 value, qint64 boundary)
{
    return qAbs(value - boundary) <= kMinuteBoundarySnapMs ? boundary : value;
}

} // namespace

void HistoryPage::rebuildTimeline()
{
    // 数据源是 _timelineSegments（当天全量段），不是活动流那批分页段。
    // 原因见 history_page.h 里 _timelineSegments 的注释：时间轴画的是「这一天」，
    // 跟着分页走就只能画出上午头一小段，用户看到的是「跟实际使用时间完全不一样」。
    // lane = 应用（显示名）或分类（生效分类 id），按时长降序，最多 kMaxLanes 条。
    struct LaneBuilder
    {
        QString key;
        QString label;
        QString colorHex;
        qint64 totalDurationMs = 0;
        QVector<TimelineLaneSegment> segments;
    };
    QHash<QString, int> laneIndex;
    QVector<LaneBuilder> builders;

    // 分类色缺失时的兜底色。跟着主题走，写死浅灰在深色下会亮得跳出来。
    const QColor fallbackColor(shared::secondaryTextColor(
        eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark));

    for (const data::SessionSegment& segment : _timelineSegments) {
        const qint64 startMs = std::max(segment.startTimeMs, _dayStartMs);
        const qint64 endMs = segment.isOpen()
            ? _cutoffMs
            : std::min(segment.endTimeMs.value_or(_cutoffMs), _dayEndMs);
        if (endMs <= startMs) {
            continue;
        }
        const AppMeta meta = appMetaFor(segment.appId);

        QString key;
        QString label;
        QString colorHex;
        if (_timelineByCategory) {
            const QString categoryId = effectiveCategoryId(meta);
            key = QStringLiteral("category:%1").arg(categoryId);
            label = categoryLabelFor(categoryId);
            colorHex = effectiveColorHex(meta);
        } else {
            key = QStringLiteral("app:%1").arg(meta.exeName.toLower());
            label = shownNameFor(meta);
            colorHex = effectiveColorHex(meta);
        }

        int index = laneIndex.value(key, -1);
        if (index < 0) {
            LaneBuilder builder;
            builder.key = key;
            builder.label = label;
            builder.colorHex = colorHex;
            index = builders.size();
            laneIndex.insert(key, index);
            builders.append(builder);
        }

        TimelineLaneSegment item;
        item.startMs = startMs;
        item.endMs = endMs;
        item.durationMs = endMs - startMs;
        item.label = builders.at(index).label;
        item.detail = segment.windowTitle.trimmed();
        item.color = colorHex.isEmpty() ? fallbackColor : QColor(colorHex);
        item.isLive = segment.isOpen() && _cutoffMs < _dayEndMs;
        builders[index].segments.append(item);
        builders[index].totalDurationMs += item.durationMs;
    }

    // 相邻短段合并：
    // 同 lane 内、间隔小于 merge_gap（settings 的 timeline_merge_gap_secs，默认 180s）
    // 的相邻段合并成一块显示——21 点连切 41 次屏时时间轴不再是一地碎渣。
    // 只影响时间轴视觉，活动流明细与统计数字不变。
    //
    // 主视图的色带也吃同一个阈值：分钟段拼回连续色块时用它，
    // 否则一分钟一截的色带在 24h 视口下还是碎的。
    qint64 mergeGapMs = 180 * 1000;
    {
        data::SettingsRepository settings(*_database);
        const data::Result<std::optional<QString>> stored =
            settings.value(QStringLiteral("timeline_merge_gap_secs"));
        if (stored.ok() && stored.value.has_value()) {
            bool ok = false;
            const qint64 secs = stored.value->toLongLong(&ok);
            if (ok && secs >= 0) {
                mergeGapMs = secs * 1000;
            }
        }
    }

    for (LaneBuilder& builder : builders) {
        QVector<TimelineLaneSegment> merged;
        merged.reserve(builder.segments.size());
        // segments 按 append 顺序=时间升序（_segments 升序）。
        for (const TimelineLaneSegment& item : builder.segments) {
            if (!merged.isEmpty()) {
                TimelineLaneSegment& last = merged.last();
                // 间隔必须在 [0, mergeGap] 内才合并：负间隔说明两段有交叠（乱序或
                // 同一时刻的重复采样），连起来会让色块时间倒流。
                const qint64 gapMs = item.startMs - last.endMs;
                if (gapMs >= 0 && gapMs <= mergeGapMs) {
                    last.endMs = std::max(last.endMs, item.endMs);
                    // 时长按各段时间累加，不取首尾跨度：合并只是视觉上把碎段连成一块，
                    // 气泡里的时长仍是真实使用时长。
                    last.durationMs += item.durationMs;
                    // 标题取时间更晚那段的（用户最后看到的内容）。
                    if (!item.detail.trimmed().isEmpty()) {
                        last.detail = item.detail;
                    }
                    last.isLive = last.isLive || item.isLive;
                    continue;
                }
            }
            merged.append(item);
        }
        builder.segments = merged;
    }

    std::sort(builders.begin(), builders.end(),
              [](const LaneBuilder& left, const LaneBuilder& right) {
                  if (left.totalDurationMs != right.totalDurationMs) {
                      return left.totalDurationMs > right.totalDurationMs;
                  }
                  return left.key < right.key;
              });
    if (builders.size() > HistoryTimelineWidget::kMaxLanes) {
        builders.resize(HistoryTimelineWidget::kMaxLanes);
    }

    QVector<TimelineLane> lanes;
    lanes.reserve(builders.size());
    for (const LaneBuilder& builder : builders) {
        TimelineLane lane;
        lane.key = builder.key;
        lane.label = builder.label;
        lane.color = builder.colorHex.isEmpty() ? fallbackColor : QColor(builder.colorHex);
        lane.totalDurationMs = builder.totalDurationMs;
        lane.segments = builder.segments;
        lanes.append(lane);
    }
    // 分轨明细留给缩放对话框：那里视口只有几小时，一条 lane 一个应用才看得清。
    _timelineLanes = lanes;

    // —— 主视图：单轨主导色带 ——
    // 先按分钟切桶，每分钟选出占时最长的那个应用/分类，再拼成互不重叠的色带。
    // 视口铺满一整天（刻度 00/06/12/18/24），色带只画到「此刻」为止：
    struct MinuteBucket
    {
        QHash<QString, MinuteCandidate> items;
        // 这一分钟里真正有活动的起止：
        // 分钟边界附近的碎时间吸附到边界，其余按真实重叠算，色带不会凭空拉满一分钟。
        qint64 activeStartMs = 0;
        qint64 activeEndMs = 0;
        bool hasActive = false;
    };
    QHash<qint64, MinuteBucket> buckets;
    for (const data::SessionSegment& segment : _timelineSegments) {
        const qint64 startMs = std::max(segment.startTimeMs, _dayStartMs);
        const qint64 endMs = segment.isOpen()
            ? _cutoffMs
            : std::min(segment.endTimeMs.value_or(_cutoffMs), _dayEndMs);
        if (endMs <= startMs) {
            continue;
        }
        const AppMeta meta = appMetaFor(segment.appId);
        QString key;
        QString label;
        QString colorHex;
        if (_timelineByCategory) {
            const QString categoryId = effectiveCategoryId(meta);
            key = QStringLiteral("category:%1").arg(categoryId);
            label = categoryLabelFor(categoryId);
            colorHex = effectiveColorHex(meta);
        } else {
            key = QStringLiteral("app:%1").arg(meta.exeName.toLower());
            label = shownNameFor(meta);
            colorHex = effectiveColorHex(meta);
        }
        const QColor color = colorHex.isEmpty() ? fallbackColor : QColor(colorHex);
        const bool live = segment.isOpen() && _cutoffMs < _dayEndMs;
        const QString detail = segment.windowTitle.trimmed();

        // 逐分钟切：一分钟可能被多段覆盖（前后两段都在这一分钟里），
        // 所以桶里按 key 累加时长，最后取最大的那个。
        for (qint64 minuteStart = minuteStartOf(startMs, _dayStartMs); minuteStart < endMs;
             minuteStart += kMinuteMs) {
            const qint64 bucketEnd = minuteStart + kMinuteMs;
            const qint64 overlapStart = std::max(startMs, minuteStart);
            const qint64 overlapEnd = std::min(endMs, bucketEnd);
            if (overlapEnd <= overlapStart) {
                continue;
            }
            MinuteBucket& bucket = buckets[minuteStart];
            // 压线吸附：差 1 秒以内算「就在边界上」，否则色带每分钟都会
            // 多出几十毫秒的空隙，一天下来看起来像一格一格的。
            const qint64 visibleStart = snapToMinuteBoundary(overlapStart, minuteStart);
            const qint64 visibleEnd = snapToMinuteBoundary(overlapEnd, bucketEnd);
            if (!bucket.hasActive) {
                bucket.activeStartMs = visibleStart;
                bucket.activeEndMs = visibleEnd;
                bucket.hasActive = true;
            } else {
                bucket.activeStartMs = std::min(bucket.activeStartMs, visibleStart);
                bucket.activeEndMs = std::max(bucket.activeEndMs, visibleEnd);
            }
            MinuteCandidate& candidate = bucket.items[key];
            if (candidate.key.isEmpty()) {
                candidate.key = key;
                candidate.label = label;
                candidate.color = color;
                candidate.firstSeenAtMs = overlapStart;
            }
            candidate.durationMs += overlapEnd - overlapStart;
            candidate.isLive = candidate.isLive || live;
            if (!detail.isEmpty()) {
                candidate.detail = detail;
            }
        }
    }

    QVector<qint64> minuteStarts = buckets.keys().toVector();
    std::sort(minuteStarts.begin(), minuteStarts.end());

    QVector<TimelineLaneSegment> ribbon;
    ribbon.reserve(minuteStarts.size());
    // 合并判断要按「同一个应用/分类」而不是只比显示名：分类模式下同分类的
    // 两个应用名字不同但颜色相同，只比颜色会把它们连成一块。
    QString ribbonKey;
    for (const qint64 minuteStart : minuteStarts) {
        const auto bucketIt = buckets.constFind(minuteStart);
        if (bucketIt == buckets.constEnd() || bucketIt->items.isEmpty()) {
            continue;
        }
        const MinuteBucket& bucket = bucketIt.value();
        const MinuteCandidate* dominant = nullptr;
        for (const MinuteCandidate& candidate : bucket.items) {
            if (dominant == nullptr || candidate.durationMs > dominant->durationMs
                || (candidate.durationMs == dominant->durationMs
                    && (candidate.firstSeenAtMs < dominant->firstSeenAtMs
                        || (candidate.firstSeenAtMs == dominant->firstSeenAtMs
                            && candidate.key < dominant->key)))) {
                dominant = &candidate;
            }
        }
        if (dominant == nullptr) {
            continue;
        }

        TimelineLaneSegment item;
        item.startMs = std::max(bucket.activeStartMs, _dayStartMs);
        item.endMs = std::min(bucket.activeEndMs, _cutoffMs);
        if (item.endMs <= item.startMs) {
            continue;
        }
        item.durationMs = dominant->durationMs;
        item.label = dominant->label;
        item.detail = dominant->detail;
        item.color = dominant->color;
        item.isLive = dominant->isLive;

        // 同 key 且首尾相接（或间隔在 merge_gap 内）的相邻分钟段连成一块：
        // 一个连续用了一小时的窗口不该被切成 60 个色块。
        if (!ribbon.isEmpty()) {
            TimelineLaneSegment& last = ribbon.last();
            const qint64 gapMs = item.startMs - last.endMs;
            if (last.label == item.label && last.color == item.color && ribbonKey == dominant->key
                && gapMs >= 0 && gapMs <= mergeGapMs) {
                last.endMs = item.endMs;
                last.durationMs += item.durationMs;
                last.isLive = last.isLive || item.isLive;
                if (!item.detail.isEmpty()) {
                    last.detail = item.detail;
                }
                continue;
            }
        }
        ribbonKey = dominant->key;
        ribbon.append(item);
    }

    _timeline->setSegments(_dayStartMs, _dayEndMs, ribbon);
}

void HistoryPage::refreshHourlyPanel()
{
    if (_database == nullptr || !_database->isOpen()) {
        return;
    }
    data::HourlyReadModel model(*_database);
    const data::Result<data::HourlyActivityDay> day =
        model.loadTotal(_dayStartMs, std::min(_cutoffMs, _dayEndMs));
    if (day.ok()) {
        _hourlyBars->setTotal(day.value.hourlyMs);
    }
}

void HistoryPage::showSegmentPopover(const TimelineLaneSegment& segment)
{
    if (_segmentPopover == nullptr) {
        return;
    }
    const QDateTime start = QDateTime::fromMSecsSinceEpoch(segment.startMs);
    const QDateTime end = QDateTime::fromMSecsSinceEpoch(segment.endMs);
    // 时长走同一个格式化：不足一分钟给秒数，否则短段会显示成「0 分」。
    QString text = QStringLiteral("<b>%1</b><br>%2 – %3 · %4")
                       .arg(segment.label.toHtmlEscaped(),
                            start.toString(QStringLiteral("HH:mm")),
                            segment.isLive ? tr("进行中") : end.toString(QStringLiteral("HH:mm")),
                            formatDurationLong(segment.durationMs));
    if (!segment.detail.trimmed().isEmpty()) {
        // 明细那行压暗：颜色也得跟着主题走，HTML 里写死会在另一套主题下糊掉。
        text += QStringLiteral("<br><span style='color:%1;'>%2</span>")
                    .arg(shared::secondaryTextColor(
                             eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark),
                         segment.detail.trimmed().left(48).toHtmlEscaped());
    }
    _segmentPopover->setText(text);

    const QPoint global = QCursor::pos();
    _segmentPopover->adjustSize();
    _segmentPopover->move(global.x() + 12, global.y() + 12);
    _segmentPopover->show();
}

void HistoryPage::openZoomDialog(qint64 focusMs)
{
    if (_timeline == nullptr || _database == nullptr || !_database->isOpen()) {
        return;
    }
    if (_zoomDialog == nullptr) {
        _zoomDialog = new HistoryTimelineZoomDialog(this);
        connect(_zoomDialog, &HistoryTimelineZoomDialog::dateChanged,
                this, &HistoryPage::onZoomDateChanged);
    }
    // 把分轨明细原样交给对话框（主视图只画色带，分轨数据在这儿留着）。
    _zoomDialog->openFor(_date, focusMs, _timelineLanes, _dayStartMs, _dayEndMs);
}

void HistoryPage::onZoomDateChanged(const QDate& date)
{
    // 翻页：复用主视图的换日逻辑，然后把新数据推回对话框。
    if (date == _date) {
        return;
    }
    selectDate(date);
    if (_zoomDialog != nullptr) {
        _zoomDialog->updateLanes(_timelineLanes, _dayStartMs, _dayEndMs);
    }
}

} // namespace patina::ui
