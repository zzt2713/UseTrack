#ifndef PATINA_UI_HISTORY_HISTORY_FEED_WIDGET_H
#define PATINA_UI_HISTORY_HISTORY_FEED_WIDGET_H

#include <QColor>
#include <QFont>
#include <QString>
#include <QVector>
#include <QWidget>

class QPainter;

namespace patina::ui
{

// 活动流的一行。
//
// 三种行（小时分隔、无记录空档、一次应用切换）共用同一个自绘列表：
// 「一段一个 QWidget」的做法在翻页时会把控件数堆到上千，与内存红线
// 的释放要求相冲突，这里只留数据行，绘制走 QPainter。
struct HistoryFeedRow
{
    enum class Kind
    {
        HourHeader,
        Gap,
        Event
    };

    Kind kind = Kind::Event;

    // 仅 HourHeader 使用，形如 "09:00"。
    QString hourText;

    // 左侧时间列显示的分钟刻度（Event 取段开始，Gap 取上一段结束）。
    qint64 startTimeMs = 0;

    // Event：应用名 / 窗口标题 / 「分类 · exeName」 / 时长文本。
    // Gap：主文案 / 提示文案 / 空 / 空。
    QString primary;
    QString secondary;
    QString tertiary;
    QString trailing;

    QColor dotColor;
};

// 自绘的活动流列表。行高按内容算，行数由调用方用 keyset 分页控制。
class HistoryFeedWidget : public QWidget
{
    Q_OBJECT

public:
    explicit HistoryFeedWidget(QWidget* parent = nullptr);
    ~HistoryFeedWidget() override;

    // 整体替换：分页与过滤都重建行，避免增量维护时行与段对不上。
    void setRows(const QVector<HistoryFeedRow>& rows);
    void clear();
    bool isEmpty() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct Row
    {
        HistoryFeedRow data;
        int top = 0;
        int height = 0;
    };

    int measureRow(const HistoryFeedRow& row) const;
    void paintHourHeader(QPainter& painter, const Row& row) const;
    void paintGapRow(QPainter& painter, const Row& row, bool lastRow) const;
    void paintEventRow(QPainter& painter, const Row& row, bool lastRow) const;

    QFont _nameFont;
    QFont _titleFont;
    QFont _metaFont;
    QFont _valueFont;
    QFont _hourFont;

    int _hourHeaderHeight = 0;
    int _gapRowHeight = 0;
    int _eventRowHeight = 0;

    QVector<Row> _rows;
    int _contentHeight = 0;
};

} // namespace patina::ui

#endif // PATINA_UI_HISTORY_HISTORY_FEED_WIDGET_H
