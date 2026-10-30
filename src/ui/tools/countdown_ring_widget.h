#ifndef PATINA_UI_TOOLS_COUNTDOWN_RING_WIDGET_H
#define PATINA_UI_TOOLS_COUNTDOWN_RING_WIDGET_H

#include <QString>
#include <QWidget>

namespace patina::ui
{

// 番茄钟的进度环。QPainter 自绘（不用 QtCharts）。
//
// 只负责画：剩余时长与阶段文案均由调用方算好塞进来。
// 不缓存位图——一帧也就一个环加两行字，缓存反而要管失效。
class CountdownRingWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CountdownRingWidget(QWidget* parent = nullptr);
    ~CountdownRingWidget() override;

    // progress 0..1；小于 0 表示没有可度量的进度（画成空环）。
    void setProgress(qreal progress);
    void setPrimaryText(const QString& text);
    void setSecondaryText(const QString& text);
    // 暂停态把环画淡，跟运行态区分开。
    void setPaused(bool paused);
    void clear();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    qreal _progress = -1.0;
    QString _primaryText;
    QString _secondaryText;
    bool _paused = false;
};

} // namespace patina::ui

#endif // PATINA_UI_TOOLS_COUNTDOWN_RING_WIDGET_H
