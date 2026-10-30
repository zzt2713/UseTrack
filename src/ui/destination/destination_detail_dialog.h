#ifndef PATINA_UI_DESTINATION_DETAIL_DIALOG_H
#define PATINA_UI_DESTINATION_DETAIL_DIALOG_H

#include "data/repositories.h"
#include "ElaDialog.h"

#include <QColor>
#include <QDate>
#include <QDialog>
#include <QVector>

class ElaIconButton;
class ElaPushButton;
class QLabel;
class QVBoxLayout;

namespace patina::ui
{

// 目标描述：弹窗头部要展示的一切。
struct DestinationTargetInfo
{
    data::DestinationTarget target;
    QString displayName;
    QString secondaryText;   // exe 名 / 成员数
    QColor color;
    // 时间轴/明细的底色（应用维度 = 应用色，分类维度 = 分类色）。
    // 分类色优先用用户覆盖，没有则用内置分类色。
};

// 时间轴全景条里的一块（文件级：DayTimelineWidget 与弹窗都用）。
struct DestinationTimelineBlock
{
    qint64 startMs = 0;
    qint64 endMs = 0;
    QColor color;
    bool isLive = false;
};

// 当日全景条（自绘）。完整定义在 cpp 的匿名 namespace 之外——
// 弹窗与它互相引用类型，前置声明就够了。
class DestinationDayTimelineWidget;

// 详情弹窗（网页维度裁剪）。
//
// 一个弹窗三种状态：
//   应用维度（图标 + exe 名）/ 分类维度（分类色 + 成员数）/ 空态。
// 组成：日期翻页条 + 当日段全景条（时间轴）+ 明细列表 + 最短时长过滤。
// 三个入口共用：Dashboard 排行点行 / History 分布图例点行 / Data 图例点应用。
//
// 生命周期：关闭即清数据；每次翻日重新查（单日单目标规模有限）。
class DestinationDetailDialog : public ElaDialog
{
    Q_OBJECT

public:
    // database 可为空：此时打开会显示「数据库未就绪」。
    explicit DestinationDetailDialog(data::Database* database, QWidget* parent = nullptr);

    // 打开并定位到目标 + 日期。每次调用都整体重置。
    void openFor(const DestinationTargetInfo& target, const QDate& date);

protected:
    void hideEvent(QHideEvent* event) override;

private:
    void buildUi();
    void loadDay();
    void shiftDate(int days);
    void adjustMinDuration(int delta);
    void applyRecords();
    void updateControls();
    void clearData();
    // exe 名 → exe 完整路径（应用图标要用，apps 表里查）。
    QString resolveAppIconPath(const QString& exeName) const;

    // 副标题、空态、无图标兜底色、明细行时间列都靠样式表上色，
    // 主题一换得重铺一遍（明细行是动态建的，按当前主题直接生成）。
    void refreshThemeStyles(bool dark);
    QString secondaryStyle() const;
    QString iconFallbackStyle() const;

    data::Database* _database = nullptr;

    struct TimelineBlock
    {
        qint64 startMs = 0;
        qint64 endMs = 0;
        QColor color;
        bool isLive = false;
    };

    DestinationTargetInfo _target;
    QDate _date;
    qint64 _minDurationMs = 0;
    data::DestinationDetailDay _day;

    QLabel* _iconLabel = nullptr;
    QLabel* _titleLabel = nullptr;
    QLabel* _subtitleLabel = nullptr;
    QLabel* _statusLabel = nullptr;
    QLabel* _dateLabel = nullptr;
    QLabel* _totalLabel = nullptr;
    QLabel* _minLabel = nullptr;
    QLabel* _emptyLabel = nullptr;
    ElaIconButton* _prevDateButton = nullptr;
    ElaIconButton* _nextDateButton = nullptr;
    ElaPushButton* _todayButton = nullptr;
    ElaIconButton* _minMinusButton = nullptr;
    ElaIconButton* _minPlusButton = nullptr;
    QVBoxLayout* _recordsLayout = nullptr;

    // 时间轴自绘区。
    DestinationDayTimelineWidget* _timeline = nullptr;
};

} // namespace patina::ui

#endif // PATINA_UI_DESTINATION_DETAIL_DIALOG_H
