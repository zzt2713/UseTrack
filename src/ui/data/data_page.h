#ifndef PATINA_UI_DATA_DATA_PAGE_H
#define PATINA_UI_DATA_DATA_PAGE_H

// ui/ 只经 data/ 的对外入口取数，不写 SQL、不 include platform/。
#include "data/repositories.h"

#include <QDate>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QGridLayout;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QVBoxLayout;
class ElaComboBox;
class ElaIconButton;
class ElaPivot;

namespace patina::ui
{

class ActivityHeatmapWidget;
class TrendLineChartWidget;
class DestinationDetailDialog;

// 「数据」页：热力图 + 活动趋势 + 应用/分类曲线。
//
// 补全：区间支持 7/30/90 快捷档 + 自定义双日期（rolling/custom 语义，
// 纯本地计算）；应用曲线的序列改为搜索面板勾选（替换写死
// Top5）；新增分类趋势模式；热力图加 hover tooltip。
//
// 页面隐藏时停表、清数据、释放三张图的位图缓存。
class DataPage : public QWidget
{
    Q_OBJECT

public:
    explicit DataPage(data::Database* database, QWidget* parent = nullptr);
    ~DataPage() override;

    void setActive(bool active);

    // 供内存回归测试断言「关闭后位图缓存归零」。
    qint64 cachedBytes() const;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    enum class Range
    {
        Days7,
        Days30,
        Days90,
        Custom,
    };

    enum class Granularity
    {
        Day,
        Hour,
    };

    // 曲线卡的模式：应用序列 / 分类序列。
    enum class SeriesMode
    {
        App,
        Category,
    };

    void buildUi();
    void refresh();
    void applyRange();
    // 区间 / 粒度 / 曲线模式 / 自定义日期都是「看数据的习惯」，
    // 每次进来都退回默认等于让人重选一遍。用 QSettings 记在本地，
    // 与 Dashboard 的小时图模式同一套约定。
    void loadViewState();
    void saveViewState() const;
    void applyEmptyState(const QString& message);
    void clearLegend();
    int selectedDayCount() const;
    // 当前选中区间的起止（自定义档用日期；快捷档按今天回推）。
    void resolveRangeMs(qint64* startMsOut, qint64* endMsOut) const;
    // 从长区间里截取最后 dayCount 天，保持「日 / 应用」两份数据同步裁剪。
    static data::TrendData tailOf(const data::TrendData& source, int dayCount);
    // 从长区间里截取 startMs 起的所有天（自定义档用）。
    static data::TrendData tailOfRange(const data::TrendData& source, qint64 startMs);

    void rebuildAppChart(const data::TrendData& view);
    void rebuildCategoryChart(const data::TrendData& view);

    // 应用曲线的序列选择。三卡横排放不下 sidebar，改成卡头一个可搜索下拉 +
    // 选中项标签行：够用来追某个排不进前五的应用，不挤占图表宽度。
    void reloadSeriesOptions();
    void refreshSeriesChips();
    void toggleSeriesSelection(const QString& exeName);
    void clearSeriesSelection();
    // 当前要画哪些应用：有勾选就按勾选（顺序按总时长降序），否则回落前五。
    QVector<data::AppTrendSeries> resolveSeriesApps(const data::TrendData& view) const;

    void openAppDetail(const QString& exeName, const QDate& anchorDate);
    void openCategoryDetail(const QString& categoryId, const QDate& anchorDate);

    // 分类的生效名/色：分类管理页改的覆盖值只在注册表快照里，
    // 读不到时退回内置 token 表。
    QString categoryLabelFor(const QString& categoryId) const;
    QString categoryColorFor(const QString& categoryId) const;
    // 注册表归一：被删/墓碑的分类 id 统一落 other；快照为空时原样返回。
    QString resolveRegistryCategoryId(const QString& categoryId) const;

    // 自定义区间那两个 QLineEdit 的边框色。QLineEdit 不吃 Ela 主题，
    // 只能自己写样式表，于是颜色也得自己跟着明暗切。
    void refreshDateEditStyle(bool dark);

    data::Database* _database = nullptr;
    QTimer* _refreshTimer = nullptr;
    bool _active = false;

    Range _range = Range::Days7;
    Granularity _granularity = Granularity::Day;
    SeriesMode _seriesMode = SeriesMode::App;
    QDate _customStart;
    QDate _customEnd;

    // 区间内最长一份数据（53 周），切换范围时从这里截取。
    data::TrendData _full;

    // 分类注册表快照：refresh 时读一次，供分类曲线与详情弹框取生效名/色。
    // 读失败就留空，取色取名退回内置 token 表。
    data::CategoryRegistrySnapshot _categoryRegistry;

    QLabel* _subtitleLabel = nullptr;
    QLabel* _statusLabel = nullptr;
    QLabel* _heatActiveLabel = nullptr;
    QLabel* _heatPeakLabel = nullptr;
    QLabel* _trendTotalLabel = nullptr;
    QLabel* _trendAverageLabel = nullptr;
    QLabel* _appMetaLabel = nullptr;

    // 3a 方案 C 指标带（区间总时长 / 日均 / 峰值日，各带说明行）。
    QLabel* _totalTileValue = nullptr;
    QLabel* _totalTileNote = nullptr;
    QLabel* _avgTileValue = nullptr;
    QLabel* _avgTileNote = nullptr;
    QLabel* _peakTileValue = nullptr;
    QLabel* _peakTileNote = nullptr;

    ElaPivot* _rangePivot = nullptr;
    ElaPivot* _granularityPivot = nullptr;
    ElaPivot* _seriesModePivot = nullptr;
    QLineEdit* _customStartEdit = nullptr;
    QLineEdit* _customEndEdit = nullptr;
    // 两个日期框中间那个「→」，非自定义档要跟输入框一起藏。
    QLabel* _customRangeSeparator = nullptr;
    DestinationDetailDialog* _detailDialog = nullptr;

    // 应用曲线的手动选择（空 = 回落前五）。存的是 exeName 小写规范形，
    // 与 data::statisticalAppKey 的口径一致，关联后勾的是父应用。
    ElaComboBox* _seriesPicker = nullptr;
    ElaIconButton* _seriesClearButton = nullptr;
    QWidget* _seriesChipsHost = nullptr;
    QHBoxLayout* _seriesChipsLayout = nullptr;
    QStringList _seriesSelection;

    ActivityHeatmapWidget* _heatmap = nullptr;
    TrendLineChartWidget* _trendChart = nullptr;
    TrendLineChartWidget* _appChart = nullptr;
    QGridLayout* _legendLayout = nullptr;
};

} // namespace patina::ui

#endif // PATINA_UI_DATA_DATA_PAGE_H
