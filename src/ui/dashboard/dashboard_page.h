#ifndef PATINA_UI_DASHBOARD_DASHBOARD_PAGE_H
#define PATINA_UI_DASHBOARD_DASHBOARD_PAGE_H

// ui/ 只经 data/ 的对外入口取数，不写 SQL、不 include platform/。
#include "data/repositories.h"

#include <QPoint>
#include <QWidget>

class QLabel;
class QTimer;
class QVBoxLayout;
class ElaIconButton;

namespace patina::ui
{

class CategoryDonutWidget;
class AppRankListWidget;
class HourlyBarsWidget;
class DestinationDetailDialog;
class QuickClassificationMenu;

// 今日概览页（ui/dashboard/）。
//
// 只经 data/ 的读模型取数，不写 SQL、不碰 platform/。
// 定时刷新只在页面可见时跑；隐藏时停表并释放列表数据。
class DashboardPage : public QWidget
{
    Q_OBJECT

public:
    // database 可为空：此时页面显示「数据库未就绪」，不崩。
    explicit DashboardPage(data::Database* database, QWidget* parent = nullptr);
    ~DashboardPage() override;

    // 切页时调用：可见才开始定时刷新，隐藏就停表并释放列表缓存。
    void setActive(bool active);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void buildUi();
    void refresh();
    void applyOverview(const data::TodayOverview& overview);
    void applyEmptyState(const QString& message);
    void clearLegend();
    void refreshHourly();
    void openAppDetail(const QString& exeName);

    // 涨跌徽标与「今日合计」chip 的颜色。QLabel 只能靠样式表上色，
    // 所以把「当前是涨是跌」记下来，主题一变按新色重铺（不是重新算数）。
    void refreshDeltaStyle(bool dark);

    // 有进行中的段时提频到 2 秒，段闭合后回落到 30 秒。
    // 只在有进行中的段时才提频。
    void syncRefreshInterval(bool hasLive);

    data::Database* _database = nullptr;
    QTimer* _refreshTimer = nullptr;
    bool _active = false;

    // 页面激活后的一段时间内按 2 秒轮询，等引擎把第一段落库；
    // 见过进行中的段之后就看 live 状态决定间隔。
    bool _liveSeen = false;
    qint64 _activatedAtMs = 0;

    QLabel* _dateLabel = nullptr;
    QLabel* _statusLabel = nullptr;
    QLabel* _totalLabel = nullptr;
    QLabel* _dayDeltaLabel = nullptr;
    QLabel* _liveLabel = nullptr;
    QLabel* _appCountLabel = nullptr;
    QLabel* _categoryCountLabel = nullptr;

    CategoryDonutWidget* _donut = nullptr;
    AppRankListWidget* _appList = nullptr;
    HourlyBarsWidget* _hourlyBars = nullptr;
    DestinationDetailDialog* _detailDialog = nullptr;
    ElaIconButton* _hourlyModeButton = nullptr;
    QLabel* _hourlyTotalChip = nullptr;
    QVBoxLayout* _legendLayout = nullptr;

    // 小时图「总计 / 按分类」模式（settings 持久化，重启保持）。
    bool _hourlyCategoryMode = false;
    void setHourlyCategoryMode(bool categoryMode);

    // 与昨日的对比方向：-1 少 / 0 持平 / 1 多。只在 refresh() 里更新，
    // 主题切换时靠它重铺颜色，不必再查一次库。
    int _dayDeltaDirection = 0;

    // 快速分类：排行行右键 → 重命名 / 改分类，即时落库后整页刷新。
    void openQuickClassification(const QString& exeName, const QString& categoryId,
                                 const QString& displayName, const QPoint& globalPos);
    // 单字段保存：只传要改的一列。
    //   displayName：空串 = 不动它；categoryId：空串 = 不动它，
    //   **null QString**（categoryId.isNull()）= 清空覆盖项（选回「其他」）。
    //   两个字段都未指定是调用错误，直接忽略。
    void saveQuickOverride(const QString& exeName, const QString& displayName,
                           const QString& categoryId);
    // 关联应用：把 exeName 并到 parentExeName 名下（parentExeName 为空 = 解除）。
    void saveAppLink(const QString& exeName, const QString& parentExeName);
    // 按应用标题录制：写 settings 的 __app_override 键（引擎 5 秒 TTL 后生效）。
    void saveAppCaptureTitle(const QString& exeName, bool captureTitle);
    // 「不在列表显示」：隐藏后这个应用从概览的排行、图例、小时图里消失，
    // 下次整页刷新即生效（读模型已按 apps.hidden 过滤）。
    void saveAppHidden(const QString& exeName, bool hidden);
    QuickClassificationMenu* _quickClassification = nullptr;
};

} // namespace patina::ui

#endif // PATINA_UI_DASHBOARD_DASHBOARD_PAGE_H
