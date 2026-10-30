#include "ui/dashboard/dashboard_page.h"

#include "ui/dashboard/app_rank_list_widget.h"
#include "ui/dashboard/category_donut_widget.h"
#include "ui/destination/destination_detail_dialog.h"
#include "ui/shared/hourly_bars_widget.h"
#include "ui/shared/quick_classification_menu.h"

#include "ElaIconButton.h"
#include "ElaMessageBar.h"
#include "ElaScrollPageArea.h"
#include "ElaText.h"
#include "ElaTheme.h"

#include "data/repositories.h"

#include "shared/theme_styles.h"

#include <QDate>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace patina::ui
{
namespace
{

// 页面可见时的刷新间隔。没有进行中的段时数字是分钟级的，30 秒足够，不必每秒重算；
// 一旦有活跃段，秒级的数字（进行中时长、分布占比）就在变，按 2 秒跟一轮。
constexpr int kRefreshIntervalMs = 30000;
constexpr int kLiveRefreshIntervalMs = 2000;
// 页面激活后还没见过进行中的段时，按 2 秒探这么久。引擎首段要等 1~2 秒的采样，
// 首帧就判「没有进行中」然后落回 30 秒，界面会空等半分钟才显示当前前台。
constexpr qint64 kStartupProbeWindowMs = 60000;
constexpr int kLegendDotSize = 9;
// Ela 浮层提示停留时长（与 classification_page.cpp 同值）。
constexpr int kMessageMsec = 2600;

QString formatDurationLong(qint64 ms)
{
    const qint64 totalMinutes = std::max<qint64>(0, ms) / 60000;
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
    const qint64 totalMinutes = std::max<qint64>(0, ms) / 60000;
    const qint64 hours = totalMinutes / 60;
    const qint64 minutes = totalMinutes % 60;
    if (hours > 0) {
        return QStringLiteral("%1h %2m")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1m").arg(minutes);
}

QString formatClock(qint64 timestampMs)
{
    return QDateTime::fromMSecsSinceEpoch(timestampMs).toString(QStringLiteral("HH:mm"));
}

// 卡片容器：Ela 的 ElaScrollPageArea 自带圆角与主题底色，用它省掉一套自绘。
//
// 注意它的构造函数里写死了 setFixedHeight(75)（给统计卡用的），
// 这里当容器使必须先把高度约束解开，否则卡片会被压成 75px。
ElaScrollPageArea* makeCard(QWidget* parent)
{
    auto* card = new ElaScrollPageArea(parent);
    card->setMinimumHeight(0);
    card->setMaximumHeight(QWIDGETSIZE_MAX);
    card->setBorderRadius(14);
    return card;
}

// 文字标签一律用 ElaText：它继承 QLabel，但取色走 ElaTheme，
// 深浅色切换时不用自己维护一套调色板（控件优先 Ela）。
//
// 两点注意：
//   - ElaText 构造时默认开着 wordWrap，放进水平布局会把图例行挤扁，这里统一关掉，
//     只有需要多行的正文再单独打开；
//   - setTextStyle() 会把字号改成该样式的预设值，所以只用它取字重，
//     字号在它之后重新设回去。
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

DashboardPage::DashboardPage(data::Database* database, QWidget* parent)
    : QWidget(parent), _database(database)
{
    buildUi();

    _refreshTimer = new QTimer(this);
    _refreshTimer->setInterval(kRefreshIntervalMs);
    _refreshTimer->setTimerType(Qt::CoarseTimer);
    connect(_refreshTimer, &QTimer::timeout, this, &DashboardPage::refresh);

    // 主题切换要重画自绘控件：它们读的是 ElaTheme 当前颜色，不重建也能跟上，
    // 但需要一次 update() 才会真的重画。
    connect(eTheme, &ElaTheme::themeModeChanged, this, [this](ElaThemeType::ThemeMode mode) {
        _donut->update();
        _appList->update();
        _hourlyBars->clear();
        refreshHourly();
        refreshDeltaStyle(mode == ElaThemeType::ThemeMode::Dark);
    });

    // 排行点击 → 应用维度详情弹窗（三入口之一）。
    connect(_appList, &AppRankListWidget::appClicked, this,
            [this](const QString& exeName) { openAppDetail(exeName); });

    // 快速分类：排行行右键 → 重命名 / 改分类。
    _quickClassification = new QuickClassificationMenu(_database, this);
    _quickClassification->setHandlers(
        [this](const QString& exeName, const QString& displayName) {
            saveQuickOverride(exeName, displayName, QString());
        },
        [this](const QString& exeName, const QString& categoryId) {
            saveQuickOverride(exeName, QString(), categoryId);
        });
    // 关联应用：把这一行并到另一个应用名下，或解除关联。
    _quickClassification->setAppLinkHandler(
        [this](const QString& exeName, const QString& parentExeName) {
            saveAppLink(exeName, parentExeName);
        });
    // 按应用关掉标题记录。
    _quickClassification->setCaptureTitleHandler(
        [this](const QString& exeName, bool captureTitle) {
            saveAppCaptureTitle(exeName, captureTitle);
        });
    // 「不在列表显示」：从排行行右键也能藏，省得再去分类页找同一张卡片。
    _quickClassification->setHiddenHandler(
        [this](const QString& exeName, bool hidden) { saveAppHidden(exeName, hidden); });
    connect(_appList, &AppRankListWidget::appContextMenuRequested, this,
            &DashboardPage::openQuickClassification);

    // 小时图模式持久化：重启保持。
    _hourlyCategoryMode =
        QSettings().value(QStringLiteral("dashboard/hourlyCategoryMode"), false).toBool();
    _hourlyBars->setCategoryMode(_hourlyCategoryMode);

    refreshDeltaStyle(eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark);
}

DashboardPage::~DashboardPage() = default;

void DashboardPage::buildUi()
{
    // 外层 ElaScrollPage 已经提供了滚动容器（addCentralWidget 会把本页塞进
    // 它的滚动区），这里再套一层就会形成嵌套滚动，所以本页只做布局。
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 16, 24, 24);
    layout->setSpacing(14);

    // ---- 页头：标题 + 日期 / 统计截止 ----
    auto* header = new QWidget(this);
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(2);

    auto* title = makeLabel(tr("今日概览"), 19, header, true);
    headerLayout->addWidget(title);

    auto* metaRow = new QWidget(header);
    auto* metaLayout = new QHBoxLayout(metaRow);
    metaLayout->setContentsMargins(0, 0, 0, 0);
    metaLayout->setSpacing(10);
    _dateLabel = makeLabel(QString(), 12, metaRow);
    metaLayout->addWidget(_dateLabel);
    // 与昨日对比（涨绿、跌红、持平灰）。
    _dayDeltaLabel = makeLabel(QString(), 12, metaRow);
    metaLayout->addWidget(_dayDeltaLabel);
    metaLayout->addStretch();
    headerLayout->addWidget(metaRow);
    layout->addWidget(header);

    _statusLabel = makeLabel(QString(), 13, this);
    // 错误信息里带 SQL 错误原文，可能很长，允许折行。
    _statusLabel->setWordWrap(true);
    _statusLabel->setVisible(false);
    layout->addWidget(_statusLabel);

    // ---- 主体：左（分类分布 + 进行中） / 右（应用排行） ----
    auto* body = new QWidget(this);
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(14);

    auto* leftColumn = new QWidget(body);
    auto* leftLayout = new QVBoxLayout(leftColumn);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(14);

    auto* focusCard = makeCard(leftColumn);
    auto* focusLayout = new QVBoxLayout(focusCard);
    focusLayout->setContentsMargins(18, 16, 18, 16);
    focusLayout->setSpacing(10);

    auto* focusHeader = new QWidget(focusCard);
    auto* focusHeaderLayout = new QHBoxLayout(focusHeader);
    focusHeaderLayout->setContentsMargins(0, 0, 0, 0);
    focusHeaderLayout->addWidget(makeLabel(tr("分类分布"), 14, focusHeader, true));
    focusHeaderLayout->addStretch();
    _categoryCountLabel = makeLabel(QString(), 12, focusHeader);
    focusHeaderLayout->addWidget(_categoryCountLabel);
    focusLayout->addWidget(focusHeader);

    auto* focusBody = new QWidget(focusCard);
    auto* focusBodyLayout = new QHBoxLayout(focusBody);
    focusBodyLayout->setContentsMargins(0, 0, 0, 0);
    focusBodyLayout->setSpacing(18);

    _donut = new CategoryDonutWidget(focusBody);
    focusBodyLayout->addWidget(_donut, 0, Qt::AlignTop);

    auto* legendHost = new QWidget(focusBody);
    _legendLayout = new QVBoxLayout(legendHost);
    _legendLayout->setContentsMargins(0, 8, 0, 0);
    _legendLayout->setSpacing(9);
    _legendLayout->addStretch();
    focusBodyLayout->addWidget(legendHost, 1);

    focusLayout->addWidget(focusBody);

    _totalLabel = makeLabel(QString(), 12, focusCard);
    focusLayout->addWidget(_totalLabel);
    leftLayout->addWidget(focusCard);

    // 小时活动图独立一张卡（总/分类切换，settings 持久化）。
    auto* hourlyCard = makeCard(leftColumn);
    auto* hourlyLayout = new QVBoxLayout(hourlyCard);
    hourlyLayout->setContentsMargins(18, 16, 18, 14);
    hourlyLayout->setSpacing(8);

    auto* hourlyHeader = new QWidget(hourlyCard);
    auto* hourlyHeaderLayout = new QHBoxLayout(hourlyHeader);
    hourlyHeaderLayout->setContentsMargins(0, 0, 0, 0);
    hourlyHeaderLayout->addWidget(makeLabel(tr("小时活动"), 14, hourlyHeader, true));
    hourlyHeaderLayout->addStretch();
    _hourlyTotalChip = makeLabel(QString(), 11, hourlyHeader);
    hourlyHeaderLayout->addWidget(_hourlyTotalChip);

    _hourlyModeButton = new ElaIconButton(ElaIconType::LayerGroup, 12, 26, 26, hourlyHeader);
    _hourlyModeButton->setBorderRadius(8);
    _hourlyModeButton->setToolTip(tr("切换 总计 / 按分类"));
    connect(_hourlyModeButton, &ElaIconButton::clicked, this, [this]() {
        setHourlyCategoryMode(!_hourlyCategoryMode);
    });
    hourlyHeaderLayout->addWidget(_hourlyModeButton);
    hourlyLayout->addWidget(hourlyHeader);

    _hourlyBars = new HourlyBarsWidget(hourlyCard);
    _hourlyBars->setFixedHeight(120);
    hourlyLayout->addWidget(_hourlyBars);
    leftLayout->addWidget(hourlyCard);

    auto* liveCard = makeCard(leftColumn);
    auto* liveLayout = new QVBoxLayout(liveCard);
    liveLayout->setContentsMargins(18, 16, 18, 16);
    liveLayout->setSpacing(8);
    liveLayout->addWidget(makeLabel(tr("进行中"), 14, liveCard, true));
    _liveLabel = makeLabel(QString(), 13, liveCard);
    _liveLabel->setWordWrap(true);
    liveLayout->addWidget(_liveLabel);

    auto* note = makeLabel(
        tr("口径：按 [今日 00:00, 统计截止] 裁剪，进行中的段以统计截止时刻封底计入。"),
        11, liveCard);
    note->setWordWrap(true);
    liveLayout->addWidget(note);
    liveLayout->addStretch();
    leftLayout->addWidget(liveCard, 1);

    bodyLayout->addWidget(leftColumn, 5);

    auto* rankCard = makeCard(body);
    auto* rankLayout = new QVBoxLayout(rankCard);
    rankLayout->setContentsMargins(18, 16, 18, 16);
    rankLayout->setSpacing(10);

    auto* rankHeader = new QWidget(rankCard);
    auto* rankHeaderLayout = new QHBoxLayout(rankHeader);
    rankHeaderLayout->setContentsMargins(0, 0, 0, 0);
    rankHeaderLayout->addWidget(makeLabel(tr("应用排行"), 14, rankHeader, true));
    rankHeaderLayout->addStretch();
    _appCountLabel = makeLabel(QString(), 12, rankHeader);
    rankHeaderLayout->addWidget(_appCountLabel);
    rankLayout->addWidget(rankHeader);

    // 排行列表直接放进卡片，不套内层滚动区：外层 ElaScrollPage 已经在滚，
    // 再套一层会让滚轮在内层失效、也拿不到整页的连续滚动。
    // 行数多于卡片高度时，靠外层整页滚动继续往下看。
    _appList = new AppRankListWidget(_database, rankCard);
    rankLayout->addWidget(_appList, 0, Qt::AlignTop);
    rankLayout->addStretch();

    bodyLayout->addWidget(rankCard, 7);
    // 主体吃掉全部剩余高度：页面被塞进 ElaScrollPage 的滚动区，
    // 再补一个 stretch 会和主体抢空间，把排行卡压扁。
    layout->addWidget(body, 1);

    applyEmptyState(tr("正在读取数据…"));
}

void DashboardPage::setActive(bool active)
{
    if (!active) {
        _active = false;
        _refreshTimer->stop();
        // 页面隐藏时释放列表数据：重新可见时整批重建，成本是一次 SQL。
        _appList->clear();
        _donut->clear();
        _hourlyBars->clear();
        clearLegend();
        return;
    }

    _active = true;
    // 每次重新激活都重开探活窗口：收托盘再回来时引擎可能刚起，第一段还没落库。
    _liveSeen = false;
    _activatedAtMs = QDateTime::currentMSecsSinceEpoch();
    // 已经可见并在刷新，重复的激活请求直接忽略，避免同一次切页查两遍库。
    if (_refreshTimer->isActive()) {
        return;
    }
    _refreshTimer->start();
    // refresh() 会按这次的查询结果把间隔调到 2 秒或 30 秒。
    refresh();
}

void DashboardPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // 与 hideEvent 配对：主窗口收进托盘再打开时，定时器要靠这里恢复。
    if (_active && !_refreshTimer->isActive()) {
        _refreshTimer->start();
        refresh();
    }
}

void DashboardPage::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    // 兜底：即使 setActive(false) 没被调用（比如父窗口整体隐藏），也停表。
    _refreshTimer->stop();
}

void DashboardPage::refresh()
{
    if (!_database || !_database->isOpen()) {
        applyEmptyState(tr("数据库未就绪，无法读取今日数据。"));
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 dayStartMs = data::localDayStartMs(nowMs);

    data::DashboardReadModel model(*_database);
    const data::Result<data::TodayOverview> result = model.loadToday(dayStartMs, nowMs);
    if (!result.ok()) {
        applyEmptyState(tr("读取今日数据失败：%1").arg(result.error));
        return;
    }

    // 昨日总时长（SessionRepository 只给已闭合段；昨天已经过完，本来就没有未闭合段，
    // 与今日的 COALESCE 口径等价）。
    const QDate today = QDateTime::fromMSecsSinceEpoch(nowMs).date();
    const QDate yesterday = today.addDays(-1);
    const qint64 yesterdayStartMs = QDateTime(yesterday.startOfDay()).toMSecsSinceEpoch();
    const qint64 yesterdayEndMs = QDateTime(today.startOfDay()).toMSecsSinceEpoch();
    data::SessionRepository sessionRepo(*_database);
    const data::Result<qint64> yesterdayResult =
        sessionRepo.totalClosedDurationMs(yesterdayStartMs, yesterdayEndMs);

    applyOverview(result.value);

    if (yesterdayResult.ok()) {
        const qint64 delta = result.value.totalDurationMs - yesterdayResult.value;
        const QString deltaText = formatDurationLong(std::abs(delta));
        if (delta > 0) {
            _dayDeltaDirection = 1;
            _dayDeltaLabel->setText(tr("▲ 较昨日多 %1").arg(deltaText));
        } else if (delta < 0) {
            _dayDeltaDirection = -1;
            _dayDeltaLabel->setText(tr("▼ 较昨日少 %1").arg(deltaText));
        } else {
            _dayDeltaDirection = 0;
            _dayDeltaLabel->setText(tr("— 与昨日持平"));
        }
    } else {
        _dayDeltaDirection = 0;
        _dayDeltaLabel->setText(QString());
    }
    refreshDeltaStyle(eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark);

    refreshHourly();
}

void DashboardPage::refreshDeltaStyle(bool dark)
{
    if (_hourlyTotalChip != nullptr) {
        _hourlyTotalChip->setStyleSheet(shared::secondaryTextStyle(dark));
    }
    if (_dayDeltaLabel == nullptr || _dayDeltaLabel->text().isEmpty()) {
        return;
    }
    if (_dayDeltaDirection > 0) {
        _dayDeltaLabel->setStyleSheet(shared::successTextStyle(dark));
    } else if (_dayDeltaDirection < 0) {
        _dayDeltaLabel->setStyleSheet(shared::dangerTextStyle(dark));
    } else {
        _dayDeltaLabel->setStyleSheet(shared::secondaryTextStyle(dark));
    }
}

void DashboardPage::refreshHourly()
{
    if (!_database || !_database->isOpen()) {
        return;
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 dayStartMs = data::localDayStartMs(nowMs);

    data::HourlyReadModel model(*_database);
    qint64 sum = 0;
    if (_hourlyCategoryMode) {
        const data::Result<QVector<data::HourlyCategorySlice>> slices =
            model.loadByCategory(dayStartMs, nowMs);
        if (!slices.ok()) {
            return;
        }
        // 转成控件切片（前 6 类，其余并入「其他」由控件处理）。
        QVector<HourlyBarsWidget::CategorySlice> widgetSlices;
        for (const data::HourlyCategorySlice& slice : slices.value) {
            HourlyBarsWidget::CategorySlice widgetSlice;
            widgetSlice.label = slice.label;
            widgetSlice.color = QColor(slice.colorHex);
            std::copy(slice.hourlyMs, slice.hourlyMs + 24, widgetSlice.hourlyMs);
            widgetSlices.append(widgetSlice);
            for (int hour = 0; hour < 24; ++hour) {
                sum += slice.hourlyMs[hour];
            }
        }
        _hourlyBars->setCategory(widgetSlices);
    } else {
        const data::Result<data::HourlyActivityDay> day =
            model.loadTotal(dayStartMs, nowMs);
        if (!day.ok()) {
            return;
        }
        _hourlyBars->setTotal(day.value.hourlyMs);
        for (int hour = 0; hour < 24; ++hour) {
            sum += day.value.hourlyMs[hour];
        }
    }

    _hourlyTotalChip->setText(tr("今日 %1").arg(formatDurationShort(sum)));
}

void DashboardPage::setHourlyCategoryMode(bool categoryMode)
{
    if (_hourlyCategoryMode == categoryMode) {
        return;
    }
    _hourlyCategoryMode = categoryMode;
    QSettings().setValue(QStringLiteral("dashboard/hourlyCategoryMode"), categoryMode);
    _hourlyBars->setCategoryMode(categoryMode);
    refreshHourly();
}

void DashboardPage::openQuickClassification(const QString& exeName, const QString& categoryId,
                                            const QString& displayName, const QPoint& globalPos)
{
    if (_quickClassification == nullptr) {
        return;
    }
    // 当前已关联的父应用（没有则为空），菜单用它给「关联应用」子菜单打勾。
    QString currentParent;
    if (_database != nullptr && _database->isOpen()) {
        const data::Result<data::AppLinks> loadedLinks = data::loadAppLinks(*_database);
        if (loadedLinks.ok()) {
            currentParent = loadedLinks.value.value(exeName.trimmed().toLower());
        }
    }
    // 这个应用当前记不记标题（没设过就是记），菜单用它给开关打勾。
    bool captureTitle = true;
    if (_database != nullptr && _database->isOpen()) {
        const data::Result<data::AppOverridePatch> loadedOverride =
            data::loadAppOverride(*_database, exeName);
        if (loadedOverride.ok() && loadedOverride.value.captureTitle.has_value()) {
            captureTitle = *loadedOverride.value.captureTitle;
        }
    }
    // 菜单结构：重命名在上，分类在下。选了重命名就用当前展示名
    // 打开输入框；选了分类直接走保存。
    // 「不在列表显示」的勾选状态要按库里的现值给，所以先查一次。
    bool hidden = false;
    if (_database != nullptr && _database->isOpen()) {
        data::AppRepository apps(*_database);
        const data::Result<std::optional<data::App>> current = apps.findByExeName(exeName);
        if (current.ok() && current.value.has_value()) {
            hidden = current.value->hidden;
        }
    }
    if (_quickClassification->showCategoryMenu(exeName, categoryId, globalPos, currentParent,
                                               captureTitle, hidden)) {
        _quickClassification->showRename(exeName, displayName, globalPos);
    }
}

void DashboardPage::saveAppCaptureTitle(const QString& exeName, bool captureTitle)
{
    if (_database == nullptr || !_database->isOpen() || exeName.trimmed().isEmpty()) {
        return;
    }

    data::AppOverridePatch patch;
    patch.exeName = exeName;
    patch.captureTitle = captureTitle;

    const data::Status saved = data::saveAppOverrides(*_database, {patch});
    if (!saved.ok()) {
        ElaMessageBar::error(ElaMessageBarType::Top, tr("设置失败"), saved.error,
                             kMessageMsec, this);
        return;
    }

    // 引擎的覆盖项缓存有 5 秒 TTL，不需要主动失效；下一帧采样就会按新值走。
    ElaMessageBar::success(ElaMessageBarType::Top, tr("已更新"),
                           captureTitle ? tr("「%1」已恢复记录窗口标题。").arg(exeName)
                                        : tr("「%1」不再记录窗口标题。").arg(exeName),
                           kMessageMsec, this);
}

void DashboardPage::saveAppHidden(const QString& exeName, bool hidden)
{
    if (_database == nullptr || !_database->isOpen() || exeName.trimmed().isEmpty()) {
        return;
    }

    // 与 saveQuickOverride 同一套「整份状态」写法：读现值合成一整条 patch，
    // 只改 hidden 一列，其余原样交回去。
    data::AppRepository apps(*_database);
    const data::Result<std::optional<data::App>> current = apps.findByExeName(exeName);
    if (!current.ok()) {
        ElaMessageBar::error(ElaMessageBarType::Top, tr("设置失败"), current.error,
                             kMessageMsec, this);
        return;
    }
    if (!current.value.has_value()) {
        ElaMessageBar::error(ElaMessageBarType::Top, tr("设置失败"),
                             tr("应用 %1 还没有记录。").arg(exeName), kMessageMsec, this);
        return;
    }

    data::AppClassificationPatch patch;
    patch.appId = current.value->id;
    patch.displayName = current.value->displayName;
    patch.categoryId = current.value->categoryId;
    patch.colorHex = current.value->colorHex;
    patch.trackEnabled = current.value->trackEnabled;
    patch.hidden = hidden;

    data::ClassificationRepository repository(*_database);
    const data::Status saved = repository.applyPatches({patch});
    if (!saved.ok()) {
        ElaMessageBar::error(ElaMessageBarType::Top, tr("设置失败"), saved.error,
                             kMessageMsec, this);
        return;
    }

    // 读模型已按 apps.hidden 过滤，整页重查一次就看不到它了。
    refresh();
    ElaMessageBar::success(ElaMessageBarType::Top, tr("已更新"),
                           hidden ? tr("「%1」已从各页面隐藏，可在分类页的「已隐藏」里恢复。").arg(exeName)
                                  : tr("「%1」已恢复显示。").arg(exeName),
                           kMessageMsec, this);
}

void DashboardPage::saveAppLink(const QString& exeName, const QString& parentExeName)
{
    if (_database == nullptr || !_database->isOpen() || exeName.trimmed().isEmpty()) {
        return;
    }

    const data::Status saved = data::setAppLink(*_database, exeName, parentExeName);
    if (!saved.ok()) {
        ElaMessageBar::error(ElaMessageBarType::Top, tr("关联失败"), saved.error,
                             kMessageMsec, this);
        return;
    }

    // 关联只改读时归并口径，历史行一个字节没动；整页重查一次就能看到合并后的数字。
    refresh();
    ElaMessageBar::success(ElaMessageBarType::Top, tr("已更新"),
                           parentExeName.trimmed().isEmpty()
                               ? tr("「%1」已解除关联。").arg(exeName)
                               : tr("「%1」已并入「%2」。").arg(exeName, parentExeName),
                           kMessageMsec, this);
}

void DashboardPage::saveQuickOverride(const QString& exeName, const QString& displayName,
                                      const QString& categoryId)
{
    if (_database == nullptr || !_database->isOpen() || exeName.trimmed().isEmpty()) {
        return;
    }
    // null = 未指定该列；空串在 categoryId 上是「清空覆盖」的合法值
    //（QuickClassificationMenu 选「其他」时传 null QString 表达清空）。
    const bool setDisplayName = !displayName.isNull();
    const bool setCategoryId = !categoryId.isNull();
    if (!setDisplayName && !setCategoryId) {
        return;
    }

    // 读现值合成整份 patch：ClassificationRepository 的写入口是「整份状态」，
    // 快速分类只动一列，另外几列必须带着原值一起交回去。
    data::AppRepository apps(*_database);
    const data::Result<std::optional<data::App>> current = apps.findByExeName(exeName);
    if (!current.ok()) {
        ElaMessageBar::error(ElaMessageBarType::Top, tr("保存失败"), current.error,
                             kMessageMsec, this);
        return;
    }
    if (!current.value.has_value()) {
        ElaMessageBar::error(ElaMessageBarType::Top, tr("保存失败"),
                             tr("应用 %1 还没有记录。").arg(exeName), kMessageMsec, this);
        return;
    }

    data::AppClassificationPatch patch;
    patch.appId = current.value->id;
    patch.displayName = setDisplayName ? displayName : current.value->displayName;
    // 清空覆盖（null）→ 空串（repo 的「恢复默认」语义）；否则用新 id。
    patch.categoryId = setCategoryId
        ? (categoryId.isNull() ? QString(QStringLiteral("")) : categoryId)
        : current.value->categoryId;
    patch.colorHex = current.value->colorHex;
    patch.trackEnabled = current.value->trackEnabled;
    // hidden 必须带原值：快速改分类不该把隐藏状态顺手清掉。
    patch.hidden = current.value->hidden;

    data::ClassificationRepository repository(*_database);
    const data::Status saved = repository.applyPatches({patch});
    if (!saved.ok()) {
        ElaMessageBar::error(ElaMessageBarType::Top, tr("保存失败"), saved.error,
                             kMessageMsec, this);
        return;
    }

    // 写完立刻把新状态刷到界面：这里整页重查一次库，
    // 排行/图例/小时图一起跟上（30 秒定时刷新走的是同一条路径）。
    refresh();
    ElaMessageBar::success(ElaMessageBarType::Top, tr("已保存"),
                           tr("「%1」的改动已生效。").arg(exeName), kMessageMsec, this);
}

void DashboardPage::openAppDetail(const QString& exeName)
{
    if (!_database || !_database->isOpen() || exeName.trimmed().isEmpty()) {
        return;
    }

    // 目标信息：显示名走 apps 覆盖项，颜色走生效分类色。
    DestinationTargetInfo info;
    info.target.mode = data::DestinationTarget::Mode::App;
    info.target.key = exeName.trimmed();
    info.displayName = exeName.trimmed();
    info.secondaryText = exeName.trimmed();

    data::AppRepository apps(*_database);
    const data::Result<std::optional<data::App>> app = apps.findByExeName(exeName);
    if (app.ok() && app.value.has_value()) {
        if (!app.value->displayName.trimmed().isEmpty()) {
            info.displayName = app.value->displayName.trimmed();
        }
        // 生效分类色：分类管理页改的配色只在注册表覆盖键里，
        // 快照读不到才退回内置 token 表。点一次详情才查一次库，不在热路径上。
        const QString categoryId =
            data::resolveCategoryId(app.value->exeName, app.value->categoryId);
        const data::Result<data::CategoryRegistrySnapshot> registry =
            data::loadCategoryRegistrySnapshot(*_database);
        QString categoryColor;
        if (registry.ok() && !registry.value.entries.isEmpty()) {
            categoryColor = registry.value.colorFor(registry.value.resolveId(categoryId));
        } else {
            categoryColor = data::categoryToken(categoryId).colorHex;
        }
        info.color = app.value->colorHex.trimmed().isEmpty()
            ? QColor(categoryColor) : QColor(app.value->colorHex.trimmed());
    }

    if (_detailDialog == nullptr) {
        _detailDialog = new DestinationDetailDialog(_database, this);
    }
    _detailDialog->openFor(info, QDate::currentDate());
}

void DashboardPage::applyOverview(const data::TodayOverview& overview)
{
    _statusLabel->setVisible(false);

    const QDateTime cutoff = QDateTime::fromMSecsSinceEpoch(overview.cutoffMs);
    _dateLabel->setText(tr("%1 · 统计截止 %2")
                            .arg(cutoff.toString(QStringLiteral("yyyy 年 M 月 d 日")),
                                 cutoff.toString(QStringLiteral("HH:mm"))));

    _totalLabel->setText(tr("总时长 %1 · 共 %2 个应用")
                             .arg(formatDurationLong(overview.totalDurationMs))
                             .arg(overview.apps.size()));
    _categoryCountLabel->setText(tr("%1 个分类").arg(overview.categories.size()));
    _appCountLabel->setText(tr("共 %1 个").arg(overview.apps.size()));

    _donut->setCategories(overview.categories);
    // 圆心两行：大字给小时，小字给分钟 + 说明（与 preview/2a-dashboard.html 的方案 A 一致）。
    const qint64 totalMinutes = std::max<qint64>(0, overview.totalDurationMs) / 60000;
    const qint64 totalHours = totalMinutes / 60;
    const qint64 restMinutes = totalMinutes % 60;
    if (totalHours > 0) {
        _donut->setCenterText(tr("%1 小时").arg(totalHours),
                              tr("%1 分 · 总计").arg(restMinutes, 2, 10, QLatin1Char('0')));
    } else {
        _donut->setCenterText(tr("%1 分").arg(restMinutes), tr("总计"));
    }

    clearLegend();
    for (const data::CategoryUsage& category : overview.categories) {
        auto* row = new QWidget(_legendLayout->parentWidget());
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);

        auto* dot = new QLabel(row);
        dot->setFixedSize(kLegendDotSize, kLegendDotSize);
        dot->setStyleSheet(QStringLiteral("background:%1; border-radius:3px;").arg(category.colorHex));
        rowLayout->addWidget(dot);
        rowLayout->addWidget(makeLabel(category.label, 12, row), 1);
        rowLayout->addWidget(makeLabel(formatDurationShort(category.durationMs), 12, row, true));

        _legendLayout->insertWidget(_legendLayout->count() - 1, row);
    }

    if (overview.live.has_value()) {
        const data::LiveSession& live = overview.live.value();
        // 分类管理页改过名的应用，这里也要用新名字，否则「进行中」还挂着旧 exe 名。
        const QString liveName = live.windowTitle.isEmpty()
            ? (live.displayName.trimmed().isEmpty() ? live.exeName : live.displayName.trimmed())
            : live.windowTitle;
        _liveLabel->setText(tr("当前前台：%1\n自 %2 起已持续 %3（已计入上方总时长）")
                                .arg(liveName,
                                     formatClock(live.startTimeMs),
                                     formatDurationLong(live.durationMs)));
    } else {
        _liveLabel->setText(tr("当前没有进行中的活动段。"));
    }

    // 进行中的时长每刷新一次就变长一点，靠 30 秒一轮根本看不出在动。
    // 放在「空数据早退」之前：没有应用数据也可能有活跃段。
    syncRefreshInterval(overview.live.has_value());

    if (overview.apps.isEmpty()) {
        _appList->clear();
        _statusLabel->setText(tr("今天还没有记录到有效活动。"));
        _statusLabel->setVisible(true);
        return;
    }

    _appList->setApps(overview.apps, overview.totalDurationMs);
}

void DashboardPage::applyEmptyState(const QString& message)
{
    // 日期行也要跟着刷新：否则出错时它会停在上一轮成功读取的日期上。
    const QDateTime today = QDateTime::currentDateTime();
    _dateLabel->setText(today.toString(QStringLiteral("yyyy 年 M 月 d 日")));

    _statusLabel->setText(message);
    _statusLabel->setVisible(true);
    _appList->clear();
    _donut->clear();
    _hourlyBars->clear();
    clearLegend();
    _totalLabel->setText(QString());
    _dayDeltaDirection = 0;
    _dayDeltaLabel->setText(QString());
    _hourlyTotalChip->setText(QString());
    _appCountLabel->setText(QString());
    _categoryCountLabel->setText(QString());
    _liveLabel->setText(tr("当前没有进行中的活动段。"));

    // 读失败时不知道还有没有活跃段，回落到低频，免得每 2 秒重复报错。
    syncRefreshInterval(false);
}

void DashboardPage::syncRefreshInterval(bool hasLive)
{
    if (_refreshTimer == nullptr) {
        return;
    }
    if (hasLive) {
        _liveSeen = true;
    }
    // 刚激活、还没见过进行中的段：按 2 秒探一段时间，等引擎把第一段落库。
    // 超时后如果确实没有活跃段，才回落到 30 秒，免得空转。
    const bool probing = !_liveSeen
        && _activatedAtMs > 0
        && QDateTime::currentMSecsSinceEpoch() - _activatedAtMs < kStartupProbeWindowMs;
    const int interval = (hasLive || probing) ? kLiveRefreshIntervalMs : kRefreshIntervalMs;
    if (_refreshTimer->interval() != interval) {
        _refreshTimer->setInterval(interval);
    }
}

void DashboardPage::clearLegend()
{
    // 末尾那项是 stretch，单独保留。
    while (_legendLayout->count() > 1) {
        QLayoutItem* item = _legendLayout->takeAt(0);
        if (item->widget() != nullptr) {
            delete item->widget();
        }
        delete item;
    }
}

} // namespace patina::ui
