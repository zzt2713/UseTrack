#ifndef PATINA_UI_HISTORY_HISTORY_TIMELINE_ZOOM_DIALOG_H
#define PATINA_UI_HISTORY_HISTORY_TIMELINE_ZOOM_DIALOG_H

#include "ui/history/history_timeline_widget.h"
#include "ElaDialog.h"

#include <QDate>
#include <QDialog>
#include <QVector>

class ElaIconButton;
class ElaPushButton;
class QLabel;

namespace patina::ui
{

class HistoryTimelineWidget;

// 7.1b 缩放对话框。
//
// 小时级视口（1–24h，默认 4h）看段落细节；日期翻页；±按钮缩放。
// 数据由 HistoryPage 载好传入（对话框不查库，保持弹窗轻）。
class HistoryTimelineZoomDialog : public ElaDialog
{
    Q_OBJECT

public:
    explicit HistoryTimelineZoomDialog(QWidget* parent = nullptr);

    // 打开：日期 + 视口焦点（双击的时间点，视口围绕它展开）。
    void openFor(const QDate& date, qint64 focusMs,
                 const QVector<TimelineLane>& lanes, qint64 dayStartMs, qint64 dayEndMs);

    // 供翻页时整批换数据（调用方重新载入新日期的 lanes）。
    void updateLanes(const QVector<TimelineLane>& lanes, qint64 dayStartMs, qint64 dayEndMs);

signals:
    // 日期变了（调用方载入新日期的数据再 updateLanes 回来）。
    void dateChanged(const QDate& date);

protected:
    void hideEvent(QHideEvent* event) override;

private:
    void buildUi();
    void normalizeViewportAround(qint64 focusMs);
    void refreshView();
    void shiftDate(int days);
    void zoomBy(int deltaHours);
    // 视口标签与底部提示的次要色，主题一换要重铺。
    void refreshThemeStyles(bool dark);

    QDate _date;
    qint64 _dayStartMs = 0;
    qint64 _dayEndMs = 1;
    qint64 _viewportStartMs = 0;
    qint64 _viewportDurationMs = 4LL * 3600 * 1000;
    bool _hasDayBounds = false;
    QVector<TimelineLane> _lanes;

    QLabel* _dateLabel = nullptr;
    QLabel* _viewportLabel = nullptr;
    QLabel* _hintLabel = nullptr;
    HistoryTimelineWidget* _timeline = nullptr;
    ElaIconButton* _prevButton = nullptr;
    ElaIconButton* _nextButton = nullptr;
    ElaPushButton* _todayButton = nullptr;
};

} // namespace patina::ui

#endif // PATINA_UI_HISTORY_HISTORY_TIMELINE_ZOOM_DIALOG_H
