#ifndef PATINA_UI_DASHBOARD_CATEGORY_DONUT_WIDGET_H
#define PATINA_UI_DASHBOARD_CATEGORY_DONUT_WIDGET_H

#include "data/repositories.h"

#include <QVector>
#include <QWidget>

class QMouseEvent;

namespace patina::ui
{

// 分类分布环形图（QPainter 自绘，不引 QtCharts）。
//
// 只画数据，不持有业务状态：调用方给一份按大小排好序的分类列表即可。
// 中心显示总时长，由调用方通过 setCenterText 提供，避免这里重复算时间格式。
//
// 悬停：鼠标落在某段弧上时该段加亮描边，并跟手显示「分类名 · 时长」提示。
class CategoryDonutWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CategoryDonutWidget(QWidget* parent = nullptr);

    void setCategories(const QVector<data::CategoryUsage>& categories);
    void setCenterText(const QString& primary, const QString& secondary);
    void clear();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    // 命中测试：位置 → 分类索引（按极角反查落在哪段弧上；环外/环心 → -1）。
    int segmentAt(const QPointF& position) const;
    // 圆环外接矩形（绘制与命中测试共用同一套几何）。
    QRectF ringRect() const;

    QVector<data::CategoryUsage> _categories;
    QString _centerPrimary;
    QString _centerSecondary;
    int _hoverSegment = -1;
};

} // namespace patina::ui

#endif // PATINA_UI_DASHBOARD_CATEGORY_DONUT_WIDGET_H
