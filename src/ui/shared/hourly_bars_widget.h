#ifndef PATINA_UI_SHARED_HOURLY_BARS_WIDGET_H
#define PATINA_UI_SHARED_HOURLY_BARS_WIDGET_H

#include <QColor>
#include <QPixmap>
#include <QVector>
#include <QWidget>

namespace patina::ui
{

// 24 柱小时活动图（QPainter 自绘；Dashboard 与 History 的小时面板共用）。
//
// 两种模式：
//   total    —— 单序列，柱高 = 该小时活跃分钟数（0..60）；
//   category —— 堆叠柱，最多 6 类，其余并入「其他」（调用方先聚好）。
// hover 浮层报该小时的分钟数（分类模式逐类报）。
//
// 静态层位图缓存，hover 只重画浮层；clear() 释放。
class HourlyBarsWidget : public QWidget
{
    Q_OBJECT

public:
    struct CategorySlice
    {
        QString label;
        QColor color;
        qint64 hourlyMs[24] = {};
    };

    explicit HourlyBarsWidget(QWidget* parent = nullptr);

    void setTotal(const qint64* hourlyMs);           // 24 项
    void setCategory(const QVector<CategorySlice>& slices); // 已按总时长降序
    void setCategoryMode(bool categoryMode);
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
    qreal plotTop() const;
    qreal plotHeight() const;
    int hourAt(qreal localX) const;
    void renderCache();
    void releaseCache();

    bool _categoryMode = false;
    qint64 _totalMs[24] = {};
    QVector<CategorySlice> _slices;   // 已按总时长降序；调用方保证 ≤6 类 + 其他

    int _hoverHour = -1;

    QPixmap _cache;
    QSize _cacheSize;
    int _cacheMode = -1;
};

} // namespace patina::ui

#endif // PATINA_UI_SHARED_HOURLY_BARS_WIDGET_H
