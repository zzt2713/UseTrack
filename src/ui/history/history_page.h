#ifndef PATINA_UI_HISTORY_HISTORY_PAGE_H
#define PATINA_UI_HISTORY_HISTORY_PAGE_H

#include <QDate>
#include <QHash>
#include <QSet>
#include <QVector>
#include <QWidget>

#include "data/repositories.h"
#include "ui/history/history_timeline_widget.h"

class ElaCalendarPicker;
class ElaComboBox;
class ElaIconButton;
class ElaPivot;
class ElaPushButton;
class ElaText;
class QLabel;
class QVBoxLayout;

namespace patina::ui
{

class HistoryDayBandWidget;
class HistoryFeedWidget;
class HistoryTimelineWidget;
class HistoryTimelineZoomDialog;
class HourlyBarsWidget;
class DestinationDetailDialog;

// 历史时间轴页：按天回看 session 明细。
//
// 方案 B：横向时间轴独立置顶卡（默认全天），活动流收成下方矮卡；
// 右侧概览栏不变。主视图是单条色带——每个时刻只有一个颜色（哪一分钟谁
// 占得最多就画谁），双击进缩放对话框看分应用/分类的明细轨。
//
// 取数按时间窗口推进（SessionRepository::listRange 的 fromMs 游标），每次只把
// 一页段读进内存；换日或页面隐藏时整页数据连同 appId→应用元信息映射一起释放。
class HistoryPage : public QWidget
{
    Q_OBJECT

public:
    explicit HistoryPage(data::Database* database, QWidget* parent = nullptr);
    ~HistoryPage() override;

    void setActive(bool active);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    // 时间轴详情气泡的自动关闭：应用级监听「点别处 / 滚轮 / Esc」即 hide。
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildUi();
    void selectDate(const QDate& date);
    void reload();
    void loadMore();
    bool appendPage();
    void rebuildFeed();
    void rebuildTimeline();
    // 时间轴数据源：一次把当天全量段读进来（独立于活动流分页）。
    // 与 appendPage 不同，这里不设分页游标、也不做单日保留上限的截断——
    // 时间轴必须画完整一天，截断会让用户以为「后面那几个小时没用电脑」。
    void loadTimelineSegments();
    // 把 _segments 编译成「要显示的段」：先合并同应用的碎片切换，再按
    // min_session_secs 丢掉短段。
    // 只供活动流使用；时间轴走 _timelineSegments，不经过这一层。
    void rebuildDisplaySegments();
    void loadMinSessionSetting();
    void applyMinSessionSetting(qint64 secs);
    void refreshHourlyPanel();
    void showSegmentPopover(const TimelineLaneSegment& segment);
    void openZoomDialog(qint64 focusMs);
    void onZoomDateChanged(const QDate& date);
    void applyOverview(const data::TodayOverview& overview);
    void applyEmptyState(const QString& message);
    void clearContent();
    void clearLegend();
    void updateDateUi();
    void updatePagingUi();
    void appendLegendRow(const QString& label, const QString& colorHex, qint64 durationMs,
                         const QString& appExeName, const QString& categoryId);
    // 图例数据。按应用/分类两种模式共用同一份当日概览，
    // 切模式只重排重画，不重新查库（否则「加载更多」的分页会被打回第一页）。
    struct LegendEntry
    {
        QString label;
        QString colorHex;
        qint64 durationMs = 0;
        // 应用维度才填 exeName；分类维度才填 categoryId。点行时按哪个非空走哪个详情。
        QString appExeName;
        QString categoryId;
    };
    void rebuildLegend();
    void setTimelineByCategory(bool byCategory);

    // 历史明细段只带 app_id，显示名/分类/颜色都得靠这张表补。
    // 里面的 displayName/categoryId/colorHex 是分类管理页写的覆盖项原样带出来，
    // 空串表示走默认（与读模型同一口径），所以分类一改，图例色和明细行的小圆点就跟着变。
    // trackEnabled 也要带：关掉统计的应用不进任何汇总，明细流里同样不该出现，
    // 否则时间轴和上方的数字会各说一套。hidden 同理——隐藏的应用在所有页面
    // 都不出现，明细流里也必须挡掉。
    struct AppMeta
    {
        QString exeName;
        QString displayName;
        QString categoryId;
        QString colorHex;
        bool trackEnabled = true;
        bool hidden = false;
    };

    AppMeta appMetaFor(qint64 appId) const;
    QString shownNameFor(const AppMeta& meta) const;

    // 成员应用归到父应用名下。_appMetas 在 reload 时按关联表改写：
    // 成员的 exeName/displayName/categoryId/colorHex 换成父的，于是活动流、
    // 时间轴、图例、详情入口全都自动按父应用算，不必各自再查一次关联表。
    // 键仍是 appId（段表给的就是 appId），只是值已经换成了父的身份。
    void applyAppLinksToMetas();

    // 生效分类 / 生效颜色：用户覆盖优先，空串回落到内置规则与分类色。
    // 与 data::resolveCategoryId 同一判据，读模型和本页不会各算一套。
    QString effectiveCategoryId(const AppMeta& meta) const;
    QString effectiveColorHex(const AppMeta& meta) const;

    // 分类的显示名 / 颜色：分类管理页改的是 __category_*_override 覆盖键，
    // 只有注册表快照读得到；内置 token 表只是快照读失败时的兜底。
    QString categoryLabelFor(const QString& categoryId) const;
    QString categoryColorFor(const QString& categoryId) const;
    QString resolveRegistryCategoryId(const QString& categoryId) const;

    // 入口：应用分布图例点行 → 应用维度详情弹窗。
    void openAppDetail(const QString& exeName);
    // 入口：分类分布图例点行 → 分类维度详情弹窗。
    void openCategoryDetail(const QString& categoryId);

    // 段详情气泡的配色。原来是写死的深底浅字，浅色主题下像个黑块。
    void refreshPopoverStyle(bool dark);
    // 应用分布图例行的 hover 色（强调色）。样式表在重建行时逐行写。
    void refreshLegendStyle(bool dark);

    qint64 segmentDurationMs(const data::SessionSegment& segment) const;

    data::Database* _database = nullptr;

    QDate _date;
    bool _active = false;
    bool _loaded = false;
    bool _updatingPicker = false;

    qint64 _dayStartMs = 0;
    qint64 _dayEndMs = 0;
    qint64 _cutoffMs = 0;
    int _daySegmentCount = 0;
    int _dayAppCount = 0;
    int _dayCategoryCount = 0;

    // keyset 游标：上一页最后一行的 (start_time_ms, id)，严格按 ORDER BY 推进。
    bool _hasCursor = false;
    qint64 _cursorStartMs = 0;
    qint64 _cursorId = 0;
    bool _exhausted = false;
    // 撞到单日保留上限而停（与「读到当天末尾」区分开，界面文案不同）。
    bool _capped = false;

    QVector<data::SessionSegment> _segments;
    // 过滤后的显示集：活动流读它。
    // _segments 保留原样，分页游标与「已加载 N 段」仍按真实加载量算。
    QVector<data::SessionSegment> _displaySegments;
    // 时间轴专用：当天全量段，独立于活动流分页，也不吃「最短可见时长」过滤。
    //
    // 为什么不能复用 _displaySegments：活动流是分页加载的（首屏 60 段），而时间轴
    // 画的是「这一天」，若跟着分页走，就只能画出上午头一小段，用户看到的是
    // 「跟实际使用时间完全不一样」。时间轴吃的是整天数据，不经过显示阈值。
    //
    // 最短可见时长同理：那个阈值是给活动流挡切换抖动的，时间轴若也照做，
    // 真实使用里成片的短切换会被抹掉，色带中间凭空出现大段空白。
    QVector<data::SessionSegment> _timelineSegments;
    QSet<qint64> _seenIds;
    QHash<qint64, AppMeta> _appMetas;
    // 时间轴分轨明细：主视图画单轨色带，这份按应用/分类分好组的数据留给
    // 缩放对话框（那里的视口只有几小时，分轨才看得清）。重建时间轴时一起算。
    QVector<TimelineLane> _timelineLanes;
    // 分类注册表快照：分类管理页改的名字/颜色全在覆盖键里，reload 时读一次。
    // 读失败就留空，取色/取名退回内置 token 表。
    data::CategoryRegistrySnapshot _categoryRegistry;

    // 最短可见时长（秒）。默认 30 秒：低于半分钟的切换基本都是「点一下就走」，
    // 留在活动流里只会把真实使用切成一地碎片。默认 300 秒的话连正常
    // 的短时使用都会被藏掉，30 秒是挡抖动和留细节之间的折中。
    // 设为 0 表示不过滤。存 settings 键 min_session_secs。
    qint64 _minSessionSecs = 30;
    // 被过滤掉的碎段数，只用于界面文案（顶部数字仍按真实段数算）。
    int _hiddenShortCount = 0;

    ElaIconButton* _prevButton = nullptr;
    ElaIconButton* _nextButton = nullptr;
    ElaPushButton* _todayButton = nullptr;
    ElaCalendarPicker* _datePicker = nullptr;
    ElaComboBox* _minSessionBox = nullptr;
    ElaText* _dateLabel = nullptr;
    ElaText* _weekdayLabel = nullptr;
    ElaText* _chipLabel = nullptr;
    ElaText* _statusLabel = nullptr;
    ElaText* _feedMetaLabel = nullptr;
    ElaText* _moreCountLabel = nullptr;
    ElaText* _metricTotal = nullptr;
    ElaText* _metricSwitch = nullptr;
    ElaText* _metricApps = nullptr;
    ElaText* _metricCategories = nullptr;

    ElaPushButton* _moreButton = nullptr;
    HistoryFeedWidget* _feed = nullptr;
    HistoryDayBandWidget* _band = nullptr;
    QVBoxLayout* _legendLayout = nullptr;

    // —— 横向时间轴（方案 B：独立置顶卡）——
    HistoryTimelineWidget* _timeline = nullptr;
    HistoryTimelineZoomDialog* _zoomDialog = nullptr;
    DestinationDetailDialog* _detailDialog = nullptr;
    HourlyBarsWidget* _hourlyBars = nullptr;
    ElaIconButton* _timelineModeButton = nullptr;
    // 图例分布模式的分段控件（按应用 / 按分类）。
    ElaPivot* _legendModePivot = nullptr;
    // 图例标题（「应用分布」/「分类分布」随模式变）。
    ElaText* _legendTitle = nullptr;
    // true = 按分类分 lane；false = 按应用分 lane。
    // 与图例分布模式是同一个开关：时间轴 lane 和右侧图例必须同步，
    // 否则左边按分类分轨、右边却列应用，看的人没法对上。
    bool _timelineByCategory = false;
    // 当日概览原样留一份：切分布模式要重排图例，但不应重查库。
    QVector<LegendEntry> _legendEntries;

    // 气泡：一个轻量 QLabel，随段定位显示后自动关闭。
    QLabel* _segmentPopover = nullptr;
};

} // namespace patina::ui

#endif // PATINA_UI_HISTORY_HISTORY_PAGE_H
