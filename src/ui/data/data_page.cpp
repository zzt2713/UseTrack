#include "ui/data/data_page.h"

#include "ui/data/activity_heatmap_widget.h"
#include "ui/data/trend_line_chart_widget.h"
#include "ui/destination/destination_detail_dialog.h"

#include "ElaComboBox.h"
#include "ElaIconButton.h"
#include "ElaLineEdit.h"
#include "ElaPivot.h"
#include "ElaScrollPageArea.h"
#include "ElaText.h"
#include "ElaTheme.h"

#include "shared/theme_styles.h"

#include <QColor>
#include <QCompleter>
#include <QDateTime>
#include <QEvent>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLayoutItem>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStringList>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <optional>

namespace patina::ui
{

namespace
{

constexpr int kRefreshIntervalMs = 60000;
constexpr int kLegendDotSize = 9;
constexpr int kLegendColumns = 3;
// 热力图最多画 53 周（约一年）。数据也按这个长度取，切换范围时只在内存里截尾巴。
constexpr int kHeatmapWeeks = 53;
constexpr int kHistoryDays = 371;
constexpr int kAppSeriesLimit = 5;
constexpr qint64 kHourMs = 60 * 60 * 1000;
constexpr qint64 kMinuteMs = 60 * 1000;
QString formatDurationLong(qint64 ms)
{
    const qint64 totalMinutes = std::max<qint64>(0, ms) / kMinuteMs;
    const qint64 hours = totalMinutes / 60;
    const qint64 minutes = totalMinutes % 60;
    if (hours > 0) {
        return QStringLiteral("%1 小时 %2 分")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1 分").arg(minutes);
}

QString formatDurationShort(qint64 ms)
{
    const qint64 totalMinutes = std::max<qint64>(0, ms) / kMinuteMs;
    const qint64 hours = totalMinutes / 60;
    const qint64 minutes = totalMinutes % 60;
    if (hours > 0) {
        return QStringLiteral("%1h %2m")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1m").arg(minutes);
}

// 「Code.exe」→「Code」：图例里带 .exe 太长，也不好看。
QString displayNameFor(const QString& exeName)
{
    QString name = exeName.trimmed();
    if (name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        name.chop(4);
    }
    if (name.isEmpty()) {
        return QStringLiteral("未知应用");
    }
    name[0] = name.at(0).toUpper();
    return name;
}

ElaScrollPageArea* makeCard(QWidget* parent)
{
    auto* card = new ElaScrollPageArea(parent);
    // 它的构造函数里写死了 setFixedHeight(75)（给统计卡用的），当容器使必须先解开。
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
        // setTextStyle() 会把字号改成该样式的预设值，所以字号要在它之后设回去。
        label->setTextStyle(ElaTextType::BodyStrong);
        label->setTextPixelSize(pixelSize);
    }
    return label;
}

// 一行「说明 + 数值」。
QWidget* makeMetric(const QString& caption, QLabel** valueOut, QWidget* parent)
{
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    layout->addWidget(makeLabel(caption, 11, row));
    auto* value = makeLabel(QString(), 13, row, true);
    layout->addWidget(value);
    *valueOut = value;
    return row;
}

// 指标 tile（k=标题 / v=大数字 / d=说明行）。
QWidget* makeMetricTile(const QString& caption, QLabel** valueOut, QLabel** noteOut,
                        QWidget* parent)
{
    // tile 样式：k=灰字标题 / v=24px 大数字 / d=灰字说明，卡片带边框圆角与面板底色。
    // 数值行用 ElaText 的 BodyStrong 字重近似 font-weight 650。
    auto* tile = new QWidget(parent);
    tile->setObjectName(QStringLiteral("dataMetricTile"));
    tile->setStyleSheet(QStringLiteral(
        "#dataMetricTile { border:1px solid %1; border-radius:14px;"
        " background:%2; }")
        .arg(shared::borderColor(eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark),
             eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark
                 ? QStringLiteral("#1C1F25")
                 : QStringLiteral("#FFFFFF")));
    auto* layout = new QVBoxLayout(tile);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(2);
    layout->addWidget(makeLabel(caption, 11, tile));
    auto* value = makeLabel(QString(), 24, tile, true);
    layout->addWidget(value);
    auto* note = makeLabel(QString(), 11, tile);
    note->setProperty("patinaSecondary", true);
    layout->addWidget(note);
    *valueOut = value;
    *noteOut = note;
    return tile;
}

} // namespace

DataPage::DataPage(data::Database* database, QWidget* parent)
    : QWidget(parent), _database(database)
{
    buildUi();
    loadViewState();

    _refreshTimer = new QTimer(this);
    _refreshTimer->setInterval(kRefreshIntervalMs);
    _refreshTimer->setTimerType(Qt::CoarseTimer);
    connect(_refreshTimer, &QTimer::timeout, this, &DataPage::refresh);

    connect(eTheme, &ElaTheme::themeModeChanged, this, [this](ElaThemeType::ThemeMode mode) {
        // 自绘控件按主题色重画；位图缓存里记了主题，模式一变就自动失效重建。
        _heatmap->update();
        _trendChart->update();
        _appChart->update();
        refreshDateEditStyle(mode == ElaThemeType::ThemeMode::Dark);
        // 选择器与选中项标签的颜色也是自己写的样式表。
        const bool dark = mode == ElaThemeType::ThemeMode::Dark;
        if (_seriesPicker != nullptr) {
            shared::applyComboBoxTheme(_seriesPicker, dark);
        }
        refreshSeriesChips();
        // 指标 tile 的边框/底色写在样式表里，主题一换要整组重铺。
        for (QWidget* tile : findChildren<QWidget*>(QStringLiteral("dataMetricTile"))) {
            tile->setStyleSheet(QStringLiteral(
                "#dataMetricTile { border:1px solid %1; border-radius:14px;"
                " background:%2; }")
                .arg(shared::borderColor(mode == ElaThemeType::ThemeMode::Dark),
                     mode == ElaThemeType::ThemeMode::Dark ? QStringLiteral("#1C1F25")
                                                           : QStringLiteral("#FFFFFF")));
        }
    });
}

DataPage::~DataPage() = default;

void DataPage::buildUi()
{
    // 外层 ElaScrollPage 已经提供了滚动容器，本页只做布局。
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 16, 24, 24);
    layout->setSpacing(14);

    auto* header = new QWidget(this);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(12);

    auto* titleBox = new QWidget(header);
    auto* titleLayout = new QVBoxLayout(titleBox);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(2);
    titleLayout->addWidget(makeLabel(tr("数据"), 19, titleBox, true));
    _subtitleLabel = makeLabel(QString(), 12, titleBox);
    // 区间文案能到「2025 年 1 月 1 日 – 2026 年 1 月 6 日 · 共 371 天」这么长，
    // 不放开最小宽度的话它会把整行顶宽，右边四个区间控件跟着挤出可视区。
    _subtitleLabel->setMinimumWidth(0);
    titleLayout->addWidget(_subtitleLabel);
    headerLayout->addWidget(titleBox);
    headerLayout->addStretch();

    _rangePivot = new ElaPivot(header);
    _rangePivot->setTextPixelSize(13);
    _rangePivot->setPivotSpacing(16);
    _rangePivot->setMarkWidth(22);
    _rangePivot->appendPivot(tr("7 天"));
    _rangePivot->appendPivot(tr("30 天"));
    _rangePivot->appendPivot(tr("90 天"));
    _rangePivot->appendPivot(tr("自定义"));
    _rangePivot->setCurrentIndex(0);
    connect(_rangePivot, &ElaPivot::pivotClicked, this, [this](int index) {
        switch (index) {
        case 0:
            _range = Range::Days7;
            break;
        case 2:
            _range = Range::Days90;
            break;
        case 3:
            _range = Range::Custom;
            // 第一次切自定义：默认最近 30 天。
            if (!_customStart.isValid() || !_customEnd.isValid()) {
                _customEnd = QDate::currentDate();
                _customStart = _customEnd.addDays(-29);
            }
            break;
        default:
            _range = Range::Days30;
            break;
        }
        saveViewState();
        refresh();
    });
    headerLayout->addWidget(_rangePivot);

    // 自定义区间的两个日期输入。
    // 边框色跟着明暗主题走：写死浅灰在深色主题下会亮得像一条高光。
    _customStartEdit = new QLineEdit(header);
    _customStartEdit->setPlaceholderText(tr("开始日期"));
    _customStartEdit->setFixedSize(104, 26);
    _customEndEdit = new QLineEdit(header);
    _customEndEdit->setPlaceholderText(tr("结束日期"));
    _customEndEdit->setFixedSize(104, 26);
    refreshDateEditStyle(eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark);
    connect(_customStartEdit, &QLineEdit::editingFinished, this, [this]() {
        const QDate parsed = QDate::fromString(_customStartEdit->text().trimmed(),
                                               QStringLiteral("yyyy-MM-dd"));
        if (parsed.isValid()) {
            _customStart = parsed;
            saveViewState();
            if (_range == Range::Custom) {
                refresh();
            }
        }
    });
    connect(_customEndEdit, &QLineEdit::editingFinished, this, [this]() {
        const QDate parsed = QDate::fromString(_customEndEdit->text().trimmed(),
                                               QStringLiteral("yyyy-MM-dd"));
        if (parsed.isValid()) {
            _customEnd = parsed;
            saveViewState();
            if (_range == Range::Custom) {
                refresh();
            }
        }
    });
    headerLayout->addWidget(_customStartEdit);
    _customRangeSeparator = makeLabel(QStringLiteral("→"), 12, header);
    headerLayout->addWidget(_customRangeSeparator);
    headerLayout->addWidget(_customEndEdit);

    // 初始是「7 天」档，三个控件先藏起来；refresh() 之前它们默认是显示的。
    _customStartEdit->setVisible(false);
    _customEndEdit->setVisible(false);
    _customRangeSeparator->setVisible(false);
    layout->addWidget(header);

    _statusLabel = makeLabel(QString(), 13, this);
    _statusLabel->setWordWrap(true);
    _statusLabel->setVisible(false);
    layout->addWidget(_statusLabel);

    // ---- 指标带（区间总时长 / 日均 / 峰值日）----
    // .c-tiles 是 grid 1.3fr 1fr 1fr + 14px 间距，这里用 stretch 比例近似（1.3:1:1）。
    auto* metricBand = new QWidget(this);
    auto* metricLayout = new QHBoxLayout(metricBand);
    metricLayout->setContentsMargins(0, 0, 0, 0);
    metricLayout->setSpacing(14);
    metricLayout->addWidget(makeMetricTile(tr("区间总时长"), &_totalTileValue,
                                           &_totalTileNote, metricBand), 13);
    metricLayout->addWidget(makeMetricTile(tr("日均"), &_avgTileValue,
                                           &_avgTileNote, metricBand), 10);
    metricLayout->addWidget(makeMetricTile(tr("峰值日"), &_peakTileValue,
                                           &_peakTileNote, metricBand), 10);
    layout->addWidget(metricBand);

    // 热力图：通栏，固定高度。
    auto* heatCard = makeCard(this);
    heatCard->setFixedHeight(214);
    auto* heatLayout = new QVBoxLayout(heatCard);
    heatLayout->setContentsMargins(16, 14, 16, 14);
    heatLayout->setSpacing(10);

    auto* heatHead = new QWidget(heatCard);
    auto* heatHeadLayout = new QHBoxLayout(heatHead);
    heatHeadLayout->setContentsMargins(0, 0, 0, 0);
    heatHeadLayout->setSpacing(14);
    heatHeadLayout->addWidget(makeLabel(tr("活跃热力图"), 14, heatHead, true));
    heatHeadLayout->addStretch();
    heatHeadLayout->addWidget(makeMetric(tr("有记录"), &_heatActiveLabel, heatHead));
    heatHeadLayout->addWidget(makeMetric(tr("单日峰值"), &_heatPeakLabel, heatHead));
    heatLayout->addWidget(heatHead);

    _heatmap = new ActivityHeatmapWidget(heatCard);
    heatLayout->addWidget(_heatmap, 1);
    layout->addWidget(heatCard);

    // 下半：左趋势右应用曲线，各占一半。
    auto* bottom = new QHBoxLayout();
    bottom->setContentsMargins(0, 0, 0, 0);
    bottom->setSpacing(14);

    auto* trendCard = makeCard(this);
    auto* trendLayout = new QVBoxLayout(trendCard);
    trendLayout->setContentsMargins(16, 14, 16, 14);
    trendLayout->setSpacing(10);

    auto* trendHead = new QWidget(trendCard);
    auto* trendHeadLayout = new QHBoxLayout(trendHead);
    trendHeadLayout->setContentsMargins(0, 0, 0, 0);
    trendHeadLayout->setSpacing(14);
    trendHeadLayout->addWidget(makeLabel(tr("活动趋势"), 14, trendHead, true));
    trendHeadLayout->addStretch();
    trendHeadLayout->addWidget(makeMetric(tr("总时长"), &_trendTotalLabel, trendHead));
    trendHeadLayout->addWidget(makeMetric(tr("日均"), &_trendAverageLabel, trendHead));

    _granularityPivot = new ElaPivot(trendHead);
    _granularityPivot->setTextPixelSize(12);
    _granularityPivot->setPivotSpacing(12);
    _granularityPivot->setMarkWidth(18);
    _granularityPivot->appendPivot(tr("按天"));
    _granularityPivot->appendPivot(tr("按小时"));
    _granularityPivot->setCurrentIndex(0);
    connect(_granularityPivot, &ElaPivot::pivotClicked, this, [this](int index) {
        _granularity = index == 1 ? Granularity::Hour : Granularity::Day;
        saveViewState();
        applyRange();
    });
    trendHeadLayout->addWidget(_granularityPivot);
    trendLayout->addWidget(trendHead);

    _trendChart = new TrendLineChartWidget(trendCard);
    trendLayout->addWidget(_trendChart, 1);
    bottom->addWidget(trendCard, 1);

    auto* appCard = makeCard(this);
    auto* appLayout = new QVBoxLayout(appCard);
    appLayout->setContentsMargins(16, 14, 16, 14);
    appLayout->setSpacing(10);

    auto* appHead = new QWidget(appCard);
    auto* appHeadLayout = new QHBoxLayout(appHead);
    appHeadLayout->setContentsMargins(0, 0, 0, 0);
    appHeadLayout->setSpacing(10);
    appHeadLayout->addWidget(makeLabel(tr("应用曲线"), 14, appHead, true));

    // 应用 / 分类模式切换。
    _seriesModePivot = new ElaPivot(appHead);
    _seriesModePivot->setTextPixelSize(12);
    _seriesModePivot->setPivotSpacing(12);
    _seriesModePivot->setMarkWidth(18);
    _seriesModePivot->appendPivot(tr("应用"));
    _seriesModePivot->appendPivot(tr("分类"));
    _seriesModePivot->setCurrentIndex(0);
    connect(_seriesModePivot, &ElaPivot::pivotClicked, this, [this](int index) {
        _seriesMode = index == 1 ? SeriesMode::Category : SeriesMode::App;
        saveViewState();
        applyRange();
    });
    appHeadLayout->addWidget(_seriesModePivot);
    // 元信息紧跟切换器，伸缩条放在它后面。
    _appMetaLabel = makeLabel(QString(), 11, appHead);
    _appMetaLabel->setMinimumWidth(0);
    appHeadLayout->addWidget(_appMetaLabel);
    appHeadLayout->addStretch();

    // 应用序列选择。三卡横排放不下 sidebar，改成卡头一个可搜索下拉：
    // 选中的应用进下面那排标签，不选就回落「时长前五」。只在应用模式显示。
    _seriesPicker = new ElaComboBox(appHead);
    _seriesPicker->setFixedWidth(190);
    // 必须经基类指针调：ElaComboBox 自己声明了一个同名的非虚 setEditable，
    // 直接调会命中它（看不到实现，不确定有没有转给 QComboBox）。走基类
    // 保证 QComboBox::setEditable 被调用，lineEdit() 才有东西返回。
    static_cast<QComboBox*>(_seriesPicker)->setEditable(true);
    _seriesPicker->setInsertPolicy(QComboBox::NoInsert);
    if (QLineEdit* pickerEdit = _seriesPicker->lineEdit()) {
        pickerEdit->setPlaceholderText(tr("选择要对比的应用"));
        pickerEdit->setClearButtonEnabled(true);
        // 输入框自带的边框跟 Ela 下拉的圆角打架，去掉。
        pickerEdit->setFrame(false);
    }
    // 文字色要跟着明暗主题走，否则深色底上会拿默认黑字画。
    shared::applyComboBoxTheme(_seriesPicker, eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark);
    connect(_seriesPicker, &QComboBox::activated, this, [this](int index) {
        const QString exeName = _seriesPicker->itemData(index).toString();
        if (exeName.isEmpty()) {
            return;
        }
        toggleSeriesSelection(exeName);
        _seriesPicker->setCurrentIndex(-1);
        if (QLineEdit* pickerEdit = _seriesPicker->lineEdit()) {
            pickerEdit->clear();
        }
        applyRange();
    });
    appHeadLayout->addWidget(_seriesPicker);

    _seriesClearButton = new ElaIconButton(ElaIconType::IconName::FilterCircleXmark, 14, 26, 26, appHead);
    _seriesClearButton->setToolTip(tr("清空选择，回到时长前五"));
    connect(_seriesClearButton, &ElaIconButton::clicked, this, [this]() {
        if (_seriesSelection.isEmpty()) {
            return;
        }
        clearSeriesSelection();
        applyRange();
    });
    appHeadLayout->addWidget(_seriesClearButton);

    appLayout->addWidget(appHead);

    // 选中项标签行：空时整行隐藏，不占高度。
    _seriesChipsHost = new QWidget(appCard);
    _seriesChipsLayout = new QHBoxLayout(_seriesChipsHost);
    _seriesChipsLayout->setContentsMargins(0, 0, 0, 0);
    _seriesChipsLayout->setSpacing(6);
    _seriesChipsHost->setVisible(false);
    appLayout->addWidget(_seriesChipsHost);

    _appChart = new TrendLineChartWidget(appCard);
    appLayout->addWidget(_appChart, 1);

    auto* legendHost = new QWidget(appCard);
    _legendLayout = new QGridLayout(legendHost);
    _legendLayout->setContentsMargins(0, 0, 0, 0);
    _legendLayout->setHorizontalSpacing(14);
    _legendLayout->setVerticalSpacing(6);
    appLayout->addWidget(legendHost);
    bottom->addWidget(appCard, 1);

    layout->addLayout(bottom, 1);

    applyEmptyState(tr("正在读取数据…"));
}

void DataPage::setActive(bool active)
{
    if (!active) {
        _active = false;
        _refreshTimer->stop();
        // 页面隐藏时把图表数据和位图缓存一起放掉，下次进来重新取。
        _heatmap->clear();
        _trendChart->clear();
        _appChart->clear();
        clearLegend();
        _full = data::TrendData();
        return;
    }

    _active = true;
    if (_refreshTimer->isActive()) {
        return;
    }
    _refreshTimer->start();
    refresh();
}

qint64 DataPage::cachedBytes() const
{
    return _heatmap->cachedBytes() + _trendChart->cachedBytes() + _appChart->cachedBytes();
}

void DataPage::loadViewState()
{
    const QSettings settings;
    const QString range = settings.value(QStringLiteral("data/range")).toString();
    if (range == QLatin1String("days7")) {
        _range = Range::Days7;
    } else if (range == QLatin1String("days30")) {
        _range = Range::Days30;
    } else if (range == QLatin1String("days90")) {
        _range = Range::Days90;
    } else if (range == QLatin1String("custom")) {
        _range = Range::Custom;
    }
    _granularity = settings.value(QStringLiteral("data/granularity")).toString()
                           == QLatin1String("hour")
                       ? Granularity::Hour
                       : Granularity::Day;
    _seriesMode = settings.value(QStringLiteral("data/seriesMode")).toString()
                          == QLatin1String("category")
                      ? SeriesMode::Category
                      : SeriesMode::App;

    const QDate start =
        QDate::fromString(settings.value(QStringLiteral("data/customStart")).toString(),
                          QStringLiteral("yyyy-MM-dd"));
    const QDate end = QDate::fromString(settings.value(QStringLiteral("data/customEnd")).toString(),
                                        QStringLiteral("yyyy-MM-dd"));
    if (start.isValid() && end.isValid()) {
        _customStart = start;
        _customEnd = end;
    }

    // 三个分段控件是「显示态」，得跟读回来的枚举对上。setCurrentIndex 会发
    // pCurrentIndexChanged，但 pivotClicked 只在鼠标点的时候发，所以不会回灌成一次写盘。
    if (_rangePivot != nullptr) {
        int rangeIndex = 1;
        switch (_range) {
        case Range::Days7:
            rangeIndex = 0;
            break;
        case Range::Days30:
            rangeIndex = 1;
            break;
        case Range::Days90:
            rangeIndex = 2;
            break;
        case Range::Custom:
            rangeIndex = 3;
            break;
        }
        const QSignalBlocker blocker(_rangePivot);
        _rangePivot->setCurrentIndex(rangeIndex);
    }
    if (_granularityPivot != nullptr) {
        const QSignalBlocker blocker(_granularityPivot);
        _granularityPivot->setCurrentIndex(_granularity == Granularity::Hour ? 1 : 0);
    }
    if (_seriesModePivot != nullptr) {
        const QSignalBlocker blocker(_seriesModePivot);
        _seriesModePivot->setCurrentIndex(_seriesMode == SeriesMode::Category ? 1 : 0);
    }
}

void DataPage::saveViewState() const
{
    QString range = QStringLiteral("days30");
    switch (_range) {
    case Range::Days7:
        range = QStringLiteral("days7");
        break;
    case Range::Days30:
        range = QStringLiteral("days30");
        break;
    case Range::Days90:
        range = QStringLiteral("days90");
        break;
    case Range::Custom:
        range = QStringLiteral("custom");
        break;
    }

    QSettings settings;
    settings.setValue(QStringLiteral("data/range"), range);
    settings.setValue(QStringLiteral("data/granularity"),
                      _granularity == Granularity::Hour ? QStringLiteral("hour")
                                                        : QStringLiteral("day"));
    settings.setValue(QStringLiteral("data/seriesMode"),
                      _seriesMode == SeriesMode::Category ? QStringLiteral("category")
                                                          : QStringLiteral("app"));
    if (_customStart.isValid()) {
        settings.setValue(QStringLiteral("data/customStart"),
                          _customStart.toString(QStringLiteral("yyyy-MM-dd")));
    }
    if (_customEnd.isValid()) {
        settings.setValue(QStringLiteral("data/customEnd"),
                          _customEnd.toString(QStringLiteral("yyyy-MM-dd")));
    }
}

void DataPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (_active && !_refreshTimer->isActive()) {
        _refreshTimer->start();
        refresh();
    }
}

void DataPage::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    // 兜底：即使 setActive(false) 没被调用也停表。
    _refreshTimer->stop();
}

void DataPage::refresh()
{
    if (!_database || !_database->isOpen()) {
        applyEmptyState(tr("数据库未就绪，无法读取数据。"));
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const QDate today = QDateTime::fromMSecsSinceEpoch(nowMs).date();
    const QDate firstDate = today.addDays(-(kHistoryDays - 1));
    const qint64 rangeStartMs = QDateTime(firstDate, QTime(0, 0)).toMSecsSinceEpoch();

    data::DataReadModel model(*_database);
    const data::Result<data::TrendData> result = model.loadTrend(rangeStartMs, nowMs);
    if (!result.ok()) {
        applyEmptyState(tr("读取数据失败：%1").arg(result.error));
        return;
    }

    _full = result.value;
    _statusLabel->setVisible(false);

    // 分类展示值走注册表快照：分类管理页的改名与配色存在覆盖键里，
    // 内置 token 表读不到。快照读失败就留空，后面取色取名自然退回内置表。
    const data::Result<data::CategoryRegistrySnapshot> registry =
        data::loadCategoryRegistrySnapshot(*_database);
    _categoryRegistry = registry.ok() ? registry.value : data::CategoryRegistrySnapshot{};

    // 热力图固定画满一年：这是本页的主视图，不跟着 7/30/90 天变。
    _heatmap->setDays(_full.days, kHeatmapWeeks);
    _heatActiveLabel->setText(tr("%1 天").arg(_full.activeDayCount));
    _heatPeakLabel->setText(formatDurationShort(_full.peakDayMs));

    // 选择器的候选跟着区间走：切了 7/30/90 天，能选的应用列表也该变。
    reloadSeriesOptions();

    applyRange();
}

void DataPage::applyRange()
{
    // 日期输入的显隐先更新：下面的空数据分支会直接 return，
    // 放在后面的话「库里没数据时切到自定义档」看不到输入框，等于没法选区间。
    const bool customVisible = _range == Range::Custom;
    _customStartEdit->setVisible(customVisible);
    _customEndEdit->setVisible(customVisible);
    // 中间那个箭头也得跟着藏，否则非自定义档会孤零零留个「→」。
    _customRangeSeparator->setVisible(customVisible);

    // 应用序列选择器只在应用模式有意义（分类模式画的是分类片）。
    const bool appMode = _seriesMode == SeriesMode::App;
    if (_seriesPicker != nullptr) {
        _seriesPicker->setVisible(appMode);
    }
    if (_seriesClearButton != nullptr) {
        _seriesClearButton->setVisible(appMode);
    }
    if (!appMode && _seriesChipsHost != nullptr) {
        _seriesChipsHost->setVisible(false);
    }

    if (_full.days.isEmpty()) {
        applyEmptyState(tr("这段时间还没有记录到有效活动。"));
        return;
    }

    // 自定义档按日期截区间；快捷档截尾巴。
    data::TrendData view;
    if (_range == Range::Custom && _customStart.isValid() && _customEnd.isValid()) {
        const QDate start = std::min(_customStart, _customEnd);
        const QDate end = std::max(_customStart, _customEnd);
        const qint64 startMs = QDateTime(start.startOfDay()).toMSecsSinceEpoch();
        view = tailOfRange(_full, startMs);
        if (view.days.isEmpty()) {
            applyEmptyState(tr("这段时间还没有记录到有效活动。"));
            return;
        }
    } else {
        view = tailOf(_full, selectedDayCount());
        if (view.days.isEmpty()) {
            applyEmptyState(tr("这段时间还没有记录到有效活动。"));
            return;
        }
    }

    const QDate firstDate = QDateTime::fromMSecsSinceEpoch(view.days.first().dayStartMs).date();
    const QDate lastDate = QDateTime::fromMSecsSinceEpoch(view.days.last().dayStartMs).date();
    _subtitleLabel->setText(tr("%1 – %2 · 共 %3 天")
                                .arg(firstDate.toString(QStringLiteral("yyyy 年 M 月 d 日")),
                                     lastDate.toString(QStringLiteral("yyyy 年 M 月 d 日")))
                                .arg(view.days.size()));

    // 日期输入回填（自定义档时同步显示当前区间）。
    if (_range == Range::Custom) {
        _customStartEdit->setText(_customStart.toString(QStringLiteral("yyyy-MM-dd")));
        _customEndEdit->setText(_customEnd.toString(QStringLiteral("yyyy-MM-dd")));
    }

    _trendTotalLabel->setText(formatDurationShort(view.totalDurationMs));
    const qint64 averageMs = view.totalDurationMs / std::max(1, static_cast<int>(view.days.size()));
    _trendAverageLabel->setText(formatDurationShort(averageMs));

    // 指标带：数值 + 说明行。
    _totalTileValue->setText(formatDurationShort(view.totalDurationMs));
    _totalTileNote->setText(
        tr("%1 天 · 有记录 %2 天")
            .arg(view.days.size())
            .arg(view.activeDayCount));
    _avgTileValue->setText(formatDurationShort(averageMs));
    _avgTileNote->setText(tr("按区间天数平均"));
    // 峰值日：数值 + 那天的日期（M/D）。
    QDate peakDate;
    for (const data::TrendDay& day : view.days) {
        if (day.durationMs == view.peakDayMs && view.peakDayMs > 0) {
            peakDate = QDateTime::fromMSecsSinceEpoch(day.dayStartMs).date();
            break;
        }
    }
    _peakTileValue->setText(formatDurationShort(view.peakDayMs));
    _peakTileNote->setText(
        peakDate.isValid()
            ? peakDate.toString(QStringLiteral("yyyy 年 M 月 d 日"))
            : tr("暂无记录"));

    if (_granularity == Granularity::Hour) {
        // 24 个桶：每个桶是「该小时平均每天的活跃分钟数」。
        QVector<qint64> hourlyTotal(24, 0);
        for (const data::TrendDay& day : view.days) {
            for (int hour = 0; hour < 24; ++hour) {
                hourlyTotal[hour] += day.hourlyMs[static_cast<size_t>(hour)];
            }
        }
        const qreal divisor = std::max(1, static_cast<int>(view.days.size()));
        QStringList labels;
        TrendLineChartWidget::Series series;
        series.fillArea = true;
        series.showDots = false;
        series.values.reserve(24);
        for (int hour = 0; hour < 24; ++hour) {
            labels.append(QString::number(hour));
            series.values.append(static_cast<qreal>(hourlyTotal.at(hour)) / divisor / static_cast<qreal>(kMinuteMs));
        }
        _trendChart->setLabels(labels);
        _trendChart->setSeries({series});
        _trendChart->setUnitSuffix(QStringLiteral("m"));
        _trendChart->setMinimumDomain(30.0);
        _trendChart->setValueFormatter([](qreal value) {
            return tr("平均 %1 分钟").arg(QString::number(std::round(value), 'f', 0));
        });
    } else {
        QStringList labels;
        TrendLineChartWidget::Series series;
        series.fillArea = true;
        series.showDots = view.days.size() <= 31;
        series.values.reserve(view.days.size());
        for (const data::TrendDay& day : view.days) {
            const QDate date = QDateTime::fromMSecsSinceEpoch(day.dayStartMs).date();
            labels.append(QStringLiteral("%1/%2").arg(date.month()).arg(date.day()));
            series.values.append(static_cast<qreal>(day.durationMs) / static_cast<qreal>(kHourMs));
        }
        _trendChart->setLabels(labels);
        _trendChart->setSeries({series});
        _trendChart->setUnitSuffix(QStringLiteral("h"));
        _trendChart->setMinimumDomain(1.0);
        _trendChart->setValueFormatter([](qreal value) {
            return formatDurationLong(static_cast<qint64>(std::llround(value * static_cast<qreal>(kHourMs))));
        });
    }

    if (_seriesMode == SeriesMode::Category) {
        rebuildCategoryChart(view);
    } else {
        rebuildAppChart(view);
    }
}

void DataPage::applyEmptyState(const QString& message)
{
    _statusLabel->setText(message);
    _statusLabel->setVisible(true);
    _subtitleLabel->setText(QString());
    _heatmap->clear();
    _trendChart->clear();
    _appChart->clear();
    clearLegend();
    _heatActiveLabel->clear();
    _heatPeakLabel->clear();
    _trendTotalLabel->clear();
    _trendAverageLabel->clear();
    _appMetaLabel->clear();
    _totalTileValue->clear();
    _totalTileNote->clear();
    _avgTileValue->clear();
    _avgTileNote->clear();
    _peakTileValue->clear();
    _peakTileNote->clear();
    // 空态下选择器没有候选，标签行也不该留着上一份的选择。
    if (_seriesPicker != nullptr) {
        _seriesPicker->clear();
        _seriesPicker->setCurrentIndex(-1);
    }
    if (_seriesChipsHost != nullptr) {
        _seriesChipsHost->setVisible(false);
    }
}

void DataPage::refreshDateEditStyle(bool dark)
{
    const QString style = QStringLiteral("QLineEdit { border:1px solid %1; border-radius:8px;"
                                         " background:transparent; padding:0 8px; }")
                              .arg(shared::borderColor(dark));
    if (_customStartEdit != nullptr) {
        _customStartEdit->setStyleSheet(style);
    }
    if (_customEndEdit != nullptr) {
        _customEndEdit->setStyleSheet(style);
    }
}

void DataPage::reloadSeriesOptions()
{
    if (_seriesPicker == nullptr) {
        return;
    }

    // 重建下拉项时不要触发 activated（activated 只在用户点选时发，
    // 但 clear() 会连带把 currentIndex 打到 -1，先屏蔽掉更稳）。
    const QSignalBlocker blocker(_seriesPicker);
    _seriesPicker->clear();

    // 候选来自区间内的应用序列（已按总时长降序、关联后是父应用）。
    // 只受 kMaxAppOptions 上限约束——那是 data 层的事，UI 不另设门槛。
    for (const data::AppTrendSeries& app : _full.apps) {
        const QString key = app.exeName.trimmed().toLower();
        if (key.isEmpty()) {
            continue;
        }
        const QString label = app.displayName.trimmed().isEmpty() ? displayNameFor(app.exeName)
                                                                  : app.displayName.trimmed();
        const QString categoryId =
            resolveRegistryCategoryId(data::resolveCategoryId(app.exeName, app.categoryId));
        const QString customColor = app.colorHex.trimmed();
        const QColor color = customColor.isEmpty() ? QColor(categoryColorFor(categoryId))
                                                   : QColor(customColor);
        const QString text = QStringLiteral("%1  ·  %2")
                                 .arg(label, formatDurationShort(app.totalDurationMs));
        // 颜色点用图标画：下拉项没法只给文字上色。
        QPixmap dot(10, 10);
        dot.fill(Qt::transparent);
        QPainter painter(&dot);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(0, 0, 10, 10);
        painter.end();
        // QComboBox 内部是 QStandardItemModel，但 model() 的静态类型是
        // QAbstractItemModel（没有 appendRow），所以走 addItem + 回填数据。
        const int row = _seriesPicker->count();
        _seriesPicker->addItem(QIcon(dot), text);
        _seriesPicker->setItemData(row, key, Qt::UserRole);
    }

    // 可搜索：QCompleter 按包含匹配过滤，默认前缀匹配在中文应用名上基本没法用。
    if (auto* completer = _seriesPicker->completer()) {
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setFilterMode(Qt::MatchContains);
        completer->setCompletionMode(QCompleter::PopupCompletion);
    }
    _seriesPicker->setCurrentIndex(-1);
    if (QLineEdit* pickerEdit = _seriesPicker->lineEdit()) {
        pickerEdit->clear();
    }
}

void DataPage::refreshSeriesChips()
{
    if (_seriesChipsLayout == nullptr || _seriesChipsHost == nullptr) {
        return;
    }

    while (QLayoutItem* item = _seriesChipsLayout->takeAt(0)) {
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    // 选中项按当前区间内的时长降序排，跟图例顺序一致，读起来才对得上。
    QHash<QString, qint64> durationByKey;
    QHash<QString, QString> labelByKey;
    for (const data::AppTrendSeries& app : _full.apps) {
        const QString key = app.exeName.trimmed().toLower();
        durationByKey.insert(key, app.totalDurationMs);
        // 展示名取 apps 表的覆盖项；没有覆盖就用 exe 推导名，并保留原始大小写。
        labelByKey.insert(key, app.displayName.trimmed().isEmpty() ? displayNameFor(app.exeName)
                                                                   : app.displayName.trimmed());
    }
    QStringList ordered = _seriesSelection;
    std::sort(ordered.begin(), ordered.end(), [&durationByKey](const QString& left,
                                                               const QString& right) {
        return durationByKey.value(left) > durationByKey.value(right);
    });

    for (const QString& key : ordered) {
        const QString label = labelByKey.value(key, displayNameFor(key));
        auto* chip = new QPushButton(QStringLiteral("%1  ✕").arg(label), _seriesChipsHost);
        chip->setFlat(true);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setStyleSheet(QStringLiteral(
            "QPushButton { text-align:left; border:1px solid %1; border-radius:10px;"
            " padding:2px 8px; background:transparent; color:%2; }"
            "QPushButton:hover { text-decoration:line-through; }")
            .arg(shared::borderColor(eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark),
                 shared::secondaryTextColor(eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark)));
        connect(chip, &QPushButton::clicked, this, [this, key]() {
            toggleSeriesSelection(key);
            applyRange();
        });
        _seriesChipsLayout->addWidget(chip);
    }
    _seriesChipsLayout->addStretch();
    _seriesChipsHost->setVisible(!ordered.isEmpty());
}

void DataPage::toggleSeriesSelection(const QString& exeName)
{
    const QString key = exeName.trimmed().toLower();
    if (key.isEmpty()) {
        return;
    }
    if (_seriesSelection.contains(key)) {
        _seriesSelection.removeAll(key);
    } else {
        _seriesSelection.append(key);
    }
}

void DataPage::clearSeriesSelection()
{
    _seriesSelection.clear();
}

QVector<data::AppTrendSeries> DataPage::resolveSeriesApps(const data::TrendData& view) const
{
    // 没勾选：画时长前五。
    if (_seriesSelection.isEmpty()) {
        const int seriesCount = std::min(kAppSeriesLimit, static_cast<int>(view.apps.size()));
        QVector<data::AppTrendSeries> top;
        top.reserve(seriesCount);
        for (int index = 0; index < seriesCount; ++index) {
            top.append(view.apps.at(index));
        }
        return top;
    }

    // 有勾选：按 view.apps 的原序（总时长降序）取交集。勾了但区间内没数据的
    // 应用自然被过滤掉，不会画一条全零的线。
    QVector<data::AppTrendSeries> picked;
    picked.reserve(_seriesSelection.size());
    for (const data::AppTrendSeries& app : view.apps) {
        if (_seriesSelection.contains(app.exeName.trimmed().toLower())) {
            picked.append(app);
        }
    }
    return picked;
}

void DataPage::clearLegend()
{
    while (_legendLayout->count() > 0) {
        QLayoutItem* item = _legendLayout->takeAt(0);
        if (item->widget() != nullptr) {
            delete item->widget();
        }
        delete item;
    }
}

// 应用曲线：勾选的序列（或默认 Top5），各画一条线。图例行可点开详情。
void DataPage::rebuildAppChart(const data::TrendData& view)
{
    QStringList appLabels;
    for (const data::TrendDay& day : view.days) {
        const QDate date = QDateTime::fromMSecsSinceEpoch(day.dayStartMs).date();
        appLabels.append(QStringLiteral("%1/%2").arg(date.month()).arg(date.day()));
    }

    // 有勾选就画勾选的（按总时长降序），没勾选回落时长前五。
    // 图例点击仍可开应用详情。
    const QVector<data::AppTrendSeries> pickedApps = resolveSeriesApps(view);
    const int seriesCount = static_cast<int>(pickedApps.size());

    QVector<TrendLineChartWidget::Series> appSeries;
    QVector<data::AppTrendSeries> matchedApps;
    for (int index = 0; index < seriesCount; ++index) {
        const data::AppTrendSeries& app = pickedApps.at(index);
        const QString categoryId =
            resolveRegistryCategoryId(data::resolveCategoryId(app.exeName, app.categoryId));
        const QString customColor = app.colorHex.trimmed();
        TrendLineChartWidget::Series series;
        series.name = app.displayName.trimmed().isEmpty() ? displayNameFor(app.exeName)
                                                          : app.displayName.trimmed();
        series.color = customColor.isEmpty() ? QColor(categoryColorFor(categoryId))
                                             : QColor(customColor);
        series.fillArea = false;
        series.showDots = false;
        series.values.reserve(app.dayDurationsMs.size());
        for (const qint64 dayMs : app.dayDurationsMs) {
            series.values.append(static_cast<qreal>(dayMs) / static_cast<qreal>(kHourMs));
        }
        appSeries.append(series);
        matchedApps.append(app);
    }

    _appChart->setLabels(appLabels);
    _appChart->setSeries(appSeries);
    _appChart->setUnitSuffix(QStringLiteral("h"));
    _appChart->setMinimumDomain(1.0);
    _appChart->setValueFormatter([](qreal value) {
        return formatDurationLong(static_cast<qint64>(std::llround(value * static_cast<qreal>(kHourMs))));
    });

    _appMetaLabel->setText(appSeries.isEmpty()
        ? tr("暂无应用")
        : (_seriesSelection.isEmpty()
               ? tr("时长前 %1 个应用").arg(appSeries.size())
               : tr("已选 %1 个应用").arg(appSeries.size())));

    refreshSeriesChips();

    clearLegend();
    for (int i = 0; i < appSeries.size() && i < matchedApps.size(); ++i) {
        const data::AppTrendSeries& app = matchedApps.at(i);

        auto* row = new QPushButton(appSeries.at(i).name, _legendLayout->parentWidget());
        row->setFlat(true);
        row->setCursor(Qt::PointingHandCursor);
        row->setStyleSheet(QStringLiteral(
            "QPushButton { text-align:left; border:none; padding:2px 4px; color:%1; }"
            "QPushButton:hover { text-decoration:underline; }").arg(appSeries.at(i).color.name()));
        const QString exeName = app.exeName;
        // 图例点击 → 应用维度详情弹窗，定位到区间最后一天（今天）。
        // 不能固定打开昨天——曲线今天的峰值点开详情却是空的。
        // rangeEndMs 就是今天当前时刻，date() 即今天。
        const QDate anchorDate = QDateTime::fromMSecsSinceEpoch(view.rangeEndMs).date();
        connect(row, &QPushButton::clicked, this, [this, exeName, anchorDate]() {
            openAppDetail(exeName, anchorDate);
        });

        auto* wrapper = new QWidget(_legendLayout->parentWidget());
        auto* wrapperLayout = new QHBoxLayout(wrapper);
        wrapperLayout->setContentsMargins(0, 0, 0, 0);
        wrapperLayout->setSpacing(6);
        auto* dot = new QLabel(wrapper);
        dot->setFixedSize(kLegendDotSize, kLegendDotSize);
        dot->setStyleSheet(QStringLiteral("background:%1; border-radius:3px;").arg(appSeries.at(i).color.name()));
        wrapperLayout->addWidget(dot);
        wrapperLayout->addWidget(row, 1);
        wrapperLayout->addWidget(makeLabel(formatDurationShort(app.totalDurationMs), 11, wrapper, true));

        const int gridIndex = i;
        _legendLayout->addWidget(wrapper, gridIndex / kLegendColumns, gridIndex % kLegendColumns);
    }
}

// 7.3c 分类趋势：区间内各分类的日时长聚合（应用聚到生效分类）。
void DataPage::rebuildCategoryChart(const data::TrendData& view)
{
    QStringList labels;
    for (const data::TrendDay& day : view.days) {
        const QDate date = QDateTime::fromMSecsSinceEpoch(day.dayStartMs).date();
        labels.append(QStringLiteral("%1/%2").arg(date.month()).arg(date.day()));
    }

    // exe 序列按生效分类归组求和（口径与 Dashboard 分类分布一致）。
    // 分类解析走注册表快照（与读模型同一来源）——悬空 id（分类被删）
    // 统一落 other，避免这里与统计页各算一套。
    struct CategoryBucket
    {
        QString categoryId;
        QString label;
        QColor color;
        QVector<qint64> dayDurationsMs;
        qint64 totalMs = 0;
    };
    QHash<QString, int> bucketIndex;
    QVector<CategoryBucket> buckets;

    for (const data::AppTrendSeries& app : view.apps) {
        const QString categoryId = resolveRegistryCategoryId(
            data::resolveCategoryId(app.exeName, app.categoryId));
        int index = bucketIndex.value(categoryId, -1);
        if (index < 0) {
            CategoryBucket bucket;
            bucket.categoryId = categoryId;
            bucket.label = categoryLabelFor(categoryId);
            bucket.color = QColor(categoryColorFor(categoryId));
            bucket.dayDurationsMs.fill(0, view.days.size());
            index = buckets.size();
            bucketIndex.insert(categoryId, index);
            buckets.append(bucket);
        }
        for (int dayIndex = 0; dayIndex < app.dayDurationsMs.size() && dayIndex < view.days.size();
             ++dayIndex) {
            buckets[index].dayDurationsMs[dayIndex] += app.dayDurationsMs.at(dayIndex);
        }
    }
    // 合计（图例展示用）。
    for (CategoryBucket& bucket : buckets) {
        bucket.totalMs = 0;
        for (const qint64 dayMs : bucket.dayDurationsMs) {
            bucket.totalMs += dayMs;
        }
    }

    std::sort(buckets.begin(), buckets.end(),
              [](const CategoryBucket& left, const CategoryBucket& right) {
                  return left.totalMs > right.totalMs;
              });

    QVector<TrendLineChartWidget::Series> seriesList;
    for (const CategoryBucket& bucket : buckets) {
        TrendLineChartWidget::Series series;
        series.name = bucket.label;
        series.color = bucket.color;
        series.fillArea = false;
        series.showDots = false;
        series.values.reserve(bucket.dayDurationsMs.size());
        for (const qint64 dayMs : bucket.dayDurationsMs) {
            series.values.append(static_cast<qreal>(dayMs) / static_cast<qreal>(kHourMs));
        }
        seriesList.append(series);
    }

    _appChart->setLabels(labels);
    _appChart->setSeries(seriesList);
    _appChart->setUnitSuffix(QStringLiteral("h"));
    _appChart->setMinimumDomain(1.0);
    _appChart->setValueFormatter([](qreal value) {
        return formatDurationLong(static_cast<qint64>(std::llround(value * static_cast<qreal>(kHourMs))));
    });
    _appMetaLabel->setText(buckets.isEmpty() ? tr("暂无分类") : tr("共 %1 个分类").arg(buckets.size()));

    clearLegend();
    for (int i = 0; i < buckets.size(); ++i) {
        const CategoryBucket& bucket = buckets.at(i);

        auto* row = new QPushButton(bucket.label, _legendLayout->parentWidget());
        row->setFlat(true);
        row->setCursor(Qt::PointingHandCursor);
        row->setStyleSheet(QStringLiteral(
            "QPushButton { text-align:left; border:none; padding:2px 4px; color:%1; }"
            "QPushButton:hover { text-decoration:underline; }").arg(bucket.color.name()));
        const QString categoryId = bucket.categoryId;
        // 同应用图例：锚点=区间最后一天（今天），不减一天。
        const QDate anchorDate = QDateTime::fromMSecsSinceEpoch(view.rangeEndMs).date();
        connect(row, &QPushButton::clicked, this, [this, categoryId, anchorDate]() {
            openCategoryDetail(categoryId, anchorDate);
        });

        auto* wrapper = new QWidget(_legendLayout->parentWidget());
        auto* wrapperLayout = new QHBoxLayout(wrapper);
        wrapperLayout->setContentsMargins(0, 0, 0, 0);
        wrapperLayout->setSpacing(6);
        auto* dot = new QLabel(wrapper);
        dot->setFixedSize(kLegendDotSize, kLegendDotSize);
        dot->setStyleSheet(QStringLiteral("background:%1; border-radius:3px;").arg(bucket.color.name()));
        wrapperLayout->addWidget(dot);
        wrapperLayout->addWidget(row, 1);
        wrapperLayout->addWidget(makeLabel(formatDurationShort(bucket.totalMs), 11, wrapper, true));

        const int gridIndex = i;
        _legendLayout->addWidget(wrapper, gridIndex / kLegendColumns, gridIndex % kLegendColumns);
    }
}

void DataPage::openAppDetail(const QString& exeName, const QDate& anchorDate)
{
    if (!_database || !_database->isOpen() || exeName.trimmed().isEmpty()) {
        return;
    }
    DestinationTargetInfo info;
    info.target.mode = data::DestinationTarget::Mode::App;
    info.target.key = exeName.trimmed();
    info.displayName = displayNameFor(exeName);
    info.secondaryText = exeName.trimmed();

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
    _detailDialog->openFor(info, anchorDate);
}

void DataPage::openCategoryDetail(const QString& categoryId, const QDate& anchorDate)
{
    if (!_database || !_database->isOpen() || categoryId.trimmed().isEmpty()) {
        return;
    }
    const QString effectiveCategoryId = resolveRegistryCategoryId(categoryId);
    DestinationTargetInfo info;
    info.target.mode = data::DestinationTarget::Mode::Category;
    info.target.key = effectiveCategoryId;
    info.displayName = categoryLabelFor(effectiveCategoryId);
    info.secondaryText = tr("分类");
    info.color = QColor(categoryColorFor(effectiveCategoryId));
    if (_detailDialog == nullptr) {
        _detailDialog = new DestinationDetailDialog(_database, this);
    }
    _detailDialog->openFor(info, anchorDate);
}

QString DataPage::categoryLabelFor(const QString& categoryId) const
{
    if (_categoryRegistry.entries.isEmpty()) {
        return data::categoryToken(categoryId).label;
    }
    return _categoryRegistry.labelFor(categoryId);
}

QString DataPage::categoryColorFor(const QString& categoryId) const
{
    if (_categoryRegistry.entries.isEmpty()) {
        return data::categoryToken(categoryId).colorHex;
    }
    return _categoryRegistry.colorFor(categoryId);
}

QString DataPage::resolveRegistryCategoryId(const QString& categoryId) const
{
    if (_categoryRegistry.entries.isEmpty()) {
        return categoryId;
    }
    return _categoryRegistry.resolveId(categoryId);
}

int DataPage::selectedDayCount() const
{
    switch (_range) {
    case Range::Days7:
        return 7;
    case Range::Days90:
        return 90;
    case Range::Days30:
    default:
        return 30;
    }
}

void DataPage::resolveRangeMs(qint64* startMsOut, qint64* endMsOut) const
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const QDate today = QDateTime::fromMSecsSinceEpoch(nowMs).date();

    if (_range == Range::Custom && _customStart.isValid() && _customEnd.isValid()) {
        // 两端可反，自动归一。
        const QDate start = std::min(_customStart, _customEnd);
        const QDate end = std::min(std::max(_customStart, _customEnd), today);
        *startMsOut = QDateTime(start.startOfDay()).toMSecsSinceEpoch();
        *endMsOut = std::min<qint64>(
            QDateTime(end.addDays(1).startOfDay()).toMSecsSinceEpoch(), nowMs);
        return;
    }

    const QDate firstDate = today.addDays(-(selectedDayCount() - 1));
    *startMsOut = QDateTime(firstDate.startOfDay()).toMSecsSinceEpoch();
    *endMsOut = nowMs;
}

data::TrendData DataPage::tailOf(const data::TrendData& source, int dayCount)
{
    data::TrendData view;
    if (source.days.isEmpty() || dayCount <= 0) {
        return view;
    }

    const int start = std::max(0, static_cast<int>(source.days.size()) - dayCount);
    view.rangeStartMs = source.days.at(start).dayStartMs;
    view.rangeEndMs = source.rangeEndMs;
    view.days = source.days.mid(start);

    for (const data::TrendDay& day : view.days) {
        view.totalDurationMs += day.durationMs;
        if (day.durationMs > 0) {
            ++view.activeDayCount;
        }
        view.peakDayMs = std::max(view.peakDayMs, day.durationMs);
    }

    // 应用序列要跟着同一天数裁剪，否则曲线长度和横轴标签对不上。
    // 注意必须复制 categoryId/colorHex/displayName：曲线与图例的颜色、名字
    // 都从它们来——丢了就全部回落规则表，手动归属的应用（无规则命中）会被
    // 画成 other 灰。
    for (const data::AppTrendSeries& app : source.apps) {
        data::AppTrendSeries series;
        series.exeName = app.exeName;
        series.displayName = app.displayName;
        series.categoryId = app.categoryId;
        series.colorHex = app.colorHex;
        series.dayDurationsMs = app.dayDurationsMs.mid(std::min(start, static_cast<int>(app.dayDurationsMs.size())));
        for (const qint64 dayMs : series.dayDurationsMs) {
            series.totalDurationMs += dayMs;
        }
        if (series.totalDurationMs > 0) {
            view.apps.append(series);
        }
    }

    std::sort(view.apps.begin(), view.apps.end(), [](const data::AppTrendSeries& left, const data::AppTrendSeries& right) {
        if (left.totalDurationMs != right.totalDurationMs) {
            return left.totalDurationMs > right.totalDurationMs;
        }
        return left.exeName.compare(right.exeName, Qt::CaseInsensitive) < 0;
    });
    return view;
}

data::TrendData DataPage::tailOfRange(const data::TrendData& source, qint64 startMs)
{
    data::TrendData view;
    if (source.days.isEmpty()) {
        return view;
    }

    // 找到第一个 dayStartMs >= startMs 的下标（days 按时间升序）。
    int start = 0;
    while (start < source.days.size() && source.days.at(start).dayStartMs < startMs) {
        ++start;
    }
    if (start >= source.days.size()) {
        return view;
    }

    view.rangeStartMs = source.days.at(start).dayStartMs;
    view.rangeEndMs = source.rangeEndMs;
    view.days = source.days.mid(start);

    for (const data::TrendDay& day : view.days) {
        view.totalDurationMs += day.durationMs;
        if (day.durationMs > 0) {
            ++view.activeDayCount;
        }
        view.peakDayMs = std::max(view.peakDayMs, day.durationMs);
    }

    // 同 tailOf：复制元数据（categoryId/colorHex/displayName），否则曲线颜色
    // 与图例名回落规则表，手动归属的应用画成 other 灰。
    for (const data::AppTrendSeries& app : source.apps) {
        data::AppTrendSeries series;
        series.exeName = app.exeName;
        series.displayName = app.displayName;
        series.categoryId = app.categoryId;
        series.colorHex = app.colorHex;
        series.dayDurationsMs = app.dayDurationsMs.mid(std::min(start, static_cast<int>(app.dayDurationsMs.size())));
        for (const qint64 dayMs : series.dayDurationsMs) {
            series.totalDurationMs += dayMs;
        }
        if (series.totalDurationMs > 0) {
            view.apps.append(series);
        }
    }

    std::sort(view.apps.begin(), view.apps.end(), [](const data::AppTrendSeries& left, const data::AppTrendSeries& right) {
        if (left.totalDurationMs != right.totalDurationMs) {
            return left.totalDurationMs > right.totalDurationMs;
        }
        return left.exeName.compare(right.exeName, Qt::CaseInsensitive) < 0;
    });
    return view;
}

} // namespace patina::ui
