#ifndef PATINA_UI_HISTORY_HISTORY_DAY_BAND_H
#define PATINA_UI_HISTORY_HISTORY_DAY_BAND_H

#include <QColor>
#include <QVector>
#include <QWidget>

namespace patina::ui
{

// 一天 24 小时的色带：把当日各段按时间落点画成色块。
//
// 单独成一个控件是因为它要随窗口宽度重排（按比例映射到像素），
// 而不是像活动流那样固定行高。
class HistoryDayBandWidget : public QWidget
{
    Q_OBJECT

public:
    struct Block
    {
        qint64 startMs = 0;
        qint64 endMs = 0;
        QColor color;
    };

    explicit HistoryDayBandWidget(QWidget* parent = nullptr);
    ~HistoryDayBandWidget() override;

    void setBlocks(qint64 dayStartMs, qint64 dayEndMs, const QVector<Block>& blocks);
    void clear();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    qint64 _dayStartMs = 0;
    qint64 _dayEndMs = 0;
    QVector<Block> _blocks;
};

} // namespace patina::ui

#endif // PATINA_UI_HISTORY_HISTORY_DAY_BAND_H
