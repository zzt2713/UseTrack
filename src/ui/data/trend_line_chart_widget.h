#ifndef PATINA_UI_DATA_TREND_LINE_CHART_WIDGET_H
#define PATINA_UI_DATA_TREND_LINE_CHART_WIDGET_H

#include <QColor>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <functional>

class QMouseEvent;
class QPainter;
class QResizeEvent;

namespace patina::ui
{

// 趋势折线（QPainter 自绘，不引 QtCharts）。
//
// 只负责画：坐标轴、网格、平滑曲线、面积填充、数据点、hover 竖线与浮层。
// 聚合与单位换算全部由调用方完成，这里只接收已经换算好的浮点值，
// 所以同一控件既能画「活动趋势」（单序列 + 面积 + 点），
// 也能画「应用曲线」（多序列、不填面积、不画点）。
//
// 静态层（网格 + 曲线）缓存在一张位图上，hover 时只重画浮层；
// 位图随数据、尺寸、主题变化失效，页面隐藏时由调用方 clear() 彻底释放。
class TrendLineChartWidget : public QWidget
{
    Q_OBJECT

public:
    struct Series
    {
        QString name;  // hover 浮层里的名字；单序列时可以留空
        QColor color;
        QVector<qreal> values;
        bool fillArea = false;
        bool showDots = false;
    };

    explicit TrendLineChartWidget(QWidget* parent = nullptr);

    // 横轴标签，数量必须与每条序列的 values 一致。
    void setLabels(const QStringList& labels);
    void setSeries(const QVector<Series>& series);

    // 纵轴刻度的单位后缀，例如 "h" 或 "m"。
    void setUnitSuffix(const QString& suffix);
    // hover 浮层里数值的文案。留空则与纵轴刻度一致。
    void setValueFormatter(std::function<QString(qreal)> formatter);
    // 纵轴下限：按小时粒度时希望至少显示到 30 分钟，避免数据很小时曲线贴顶。
    void setMinimumDomain(qreal minimumDomain);

    void clear();

    // 当前静态层位图占用字节数，供内存回归测试断言「关闭后归零」。
    qint64 cachedBytes() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    int pointCount() const;
    qreal maxValue() const;
    // 绘图区几何：paintEvent 与鼠标命中必须用同一套算法。
    qreal plotLeft() const;
    qreal plotWidth() const;
    qreal plotTop() const;
    qreal plotHeight() const;
    qreal valueAt(int seriesIndex, int pointIndex) const;
    int indexAt(qreal localX) const;
    QString formatTick(qreal value) const;

    void renderCache();
    void drawStaticLayer(QPainter& painter) const;
    void releaseCache();

    QStringList _labels;
    QVector<Series> _series;
    QString _unitSuffix;
    std::function<QString(qreal)> _valueFormatter;
    qreal _minimumDomain = 0.0;
    int _hoverIndex = -1;

    QPixmap _cache;
    QSize _cacheSize;
    int _cacheMode = -1;  // ElaThemeType::ThemeMode，存成 int 免得头文件拖 Ela
};

} // namespace patina::ui

#endif // PATINA_UI_DATA_TREND_LINE_CHART_WIDGET_H
