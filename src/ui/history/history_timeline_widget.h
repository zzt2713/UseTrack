#ifndef PATINA_UI_HISTORY_HISTORY_TIMELINE_WIDGET_H
#define PATINA_UI_HISTORY_HISTORY_TIMELINE_WIDGET_H

#include "data/repositories.h"

#include <QColor>
#include <QPixmap>
#include <QVector>
#include <QWidget>

class QMouseEvent;
class QResizeEvent;

namespace patina::ui
{

// 横向时间轴的一个段落（一段已在调用方按 mode 分组好：应用或分类）。
struct TimelineLaneSegment
{
    qint64 startMs = 0;
    qint64 endMs = 0;
    qint64 durationMs = 0;
    QString label;          // 气泡标题（应用显示名 / 分类名）
    QString detail;         // 气泡正文（标题样本或窗口标题；空 = 无标题时段）
    QColor color;
    bool isLive = false;
};

// 一条 lane（一个应用 / 一个分类）。
struct TimelineLane
{
    QString key;            // 应用 exe 名 / 分类 id
    QString label;
    QColor color;
    qint64 totalDurationMs = 0;
    QVector<TimelineLaneSegment> segments;
};

// 横向时间轴，两种画法：
//
//  - Ribbon：主视图。**单条色轨**，每个时刻只有一个颜色——哪一分钟谁占得最多
//    就画谁。这样「同一时刻多行重叠、24h 下每块不足 3px」的问题就不存在。
//  - Lanes：缩放对话框里的分 lane 明细，一个应用/分类一条轨，带左列标签与
//    右列累计时长。
//
// 整幅静态层位图缓存，hover 描边只重画浮层；clear() 释放。
class HistoryTimelineWidget : public QWidget
{
    Q_OBJECT

public:
    // lane 数上限：一天里的应用个数可能上百，lane 太多时间轴失去意义。
    // 只作用于 Lanes 模式（Ribbon 不分轨，不截断）。
    static constexpr int kMaxLanes = 5;

    enum class Mode
    {
        Ribbon,
        Lanes,
    };

    explicit HistoryTimelineWidget(QWidget* parent = nullptr);

    // Ribbon：单轨主导色带。segments 按时间升序、互不重叠。
    void setSegments(qint64 viewportStartMs, qint64 viewportEndMs,
                     const QVector<TimelineLaneSegment>& segments);

    // Lanes：分轨明细（缩放对话框）。viewport 是可见时间窗。
    void setLanes(qint64 viewportStartMs, qint64 viewportEndMs,
                  const QVector<TimelineLane>& lanes);

    void clear();

    Mode mode() const { return _mode; }
    qint64 viewportStartMs() const { return _viewportStartMs; }
    qint64 viewportEndMs() const { return _viewportEndMs; }

    qint64 cachedBytes() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    // 点了某个段（气泡数据）。
    void segmentClicked(const patina::ui::TimelineLaneSegment& segment);
    // 双击某处（进入缩放对话框，携带双击位置对应的时间）。
    void zoomRequested(qint64 focusMs);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    struct Hit
    {
        int lane = -1;
        int segment = -1;
    };

    Hit hitAt(const QPointF& position) const;
    // 色轨矩形：Ribbon 一条通栏，Lanes 每条 lane 一段（去掉左右标签列）。
    QRectF trackRect() const;
    QRectF laneTrackRect(int lane) const;
    // 色轨（不含左标签列与右时长列）的可用宽度。
    qreal trackWidthFor() const;
    // 段在 track 上的矩形：先按视口裁剪再换算像素，视口外的段返回空矩形。
    QRectF segmentRect(const QRectF& track, const TimelineLaneSegment& segment, qreal span) const;
    void renderCache();
    void renderRibbon(QPainter& painter);
    void renderLanes(QPainter& painter);
    void releaseCache();

    Mode _mode = Mode::Ribbon;
    qint64 _viewportStartMs = 0;
    qint64 _viewportEndMs = 1;
    QVector<TimelineLane> _lanes;
    QVector<TimelineLaneSegment> _segments;

    int _hoverLane = -1;
    int _hoverSegment = -1;

    QPixmap _cache;
    QSize _cacheSize;
    int _cacheMode = -1;
};

} // namespace patina::ui

#endif // PATINA_UI_HISTORY_HISTORY_TIMELINE_WIDGET_H
