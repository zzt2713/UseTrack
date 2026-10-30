#ifndef PATINA_UI_DATA_ACTIVITY_HEATMAP_WIDGET_H
#define PATINA_UI_DATA_ACTIVITY_HEATMAP_WIDGET_H

#include "data/repositories.h"

#include <QDate>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QVector>
#include <QWidget>

class QMouseEvent;
class QPainter;
class QResizeEvent;

namespace patina::ui
{

// 活跃热力图（QPainter 自绘，不引 QtCharts）。
//
// 一列一周、七行对应周一到周日，颜色深浅按当天绝对时长分档
// （<30 分钟 / 30 分钟–2 小时 / 2–5 小时 / ≥5 小时）。用绝对阈值而不是「相对区间峰值」：
// 区间里只有几分钟数据时，峰值那天会被拉成最深一档，看着像用了一整天。
//
// 整张网格缓存成位图，hover 只重画浮层与描边；页面隐藏时 clear() 释放。
class ActivityHeatmapWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ActivityHeatmapWidget(QWidget* parent = nullptr);

    // days 必须按时间升序且首尾相接（DataReadModel::loadTrend 的 days 就是这个形状）。
    // maxWeeks > 0 时只画最后 maxWeeks 列，超出的部分直接丢弃。
    void setDays(const QVector<data::TrendDay>& days, int maxWeeks);
    void clear();

    qint64 cachedBytes() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    struct Cell
    {
        bool valid = false;
        bool hasRecord = false;
        qint64 durationMs = 0;
        QDate date;
    };

    struct Week
    {
        QString monthLabel;
        QVector<Cell> cells;
    };

    void rebuildWeeks(const QVector<data::TrendDay>& days, int maxWeeks);
    void layoutGrid();
    bool cellAt(const QPointF& position, int* week, int* row) const;

    void renderCache();
    void drawStaticLayer(QPainter& painter) const;
    void releaseCache();

    QVector<Week> _weeks;

    qreal _cellSize = 0.0;
    qreal _gap = 3.0;
    qreal _leftPad = 26.0;
    qreal _topPad = 15.0;

    int _hoverWeek = -1;
    int _hoverRow = -1;

    QPixmap _cache;
    QSize _cacheSize;
    int _cacheMode = -1;  // ElaThemeType::ThemeMode，存成 int 免得头文件拖 Ela
};

} // namespace patina::ui

#endif // PATINA_UI_DATA_ACTIVITY_HEATMAP_WIDGET_H
