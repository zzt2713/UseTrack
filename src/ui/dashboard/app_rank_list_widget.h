#ifndef PATINA_UI_DASHBOARD_APP_RANK_LIST_WIDGET_H
#define PATINA_UI_DASHBOARD_APP_RANK_LIST_WIDGET_H

#include "data/repositories.h"

#include <QColor>
#include <QPoint>
#include <QVector>
#include <QWidget>

class QContextMenuEvent;
class QMouseEvent;

namespace patina::data
{
class Database;
}

namespace patina::ui
{

// 应用排行列表。
//
// 每行自绘：exe 真图标（shared/app_icon_cache，取不到退回首字母占位）、
// 应用名、分类、时长与占比、底部进度条。行数不设上限，超出由外层滚动区处理。
//
// 内存：本控件自己不缓存 pixmap，图标缓存集中在 shared/app_icon_cache
// （上限 256 项、只存缩放后的位图）。
class AppRankListWidget : public QWidget
{
    Q_OBJECT

public:
    // database 只用来读分类注册表（分类名/色可被用户改），可为空——
    // 空时退回内置分类表。不持有所有权，每次 setApps 时按需装载一次。
    explicit AppRankListWidget(data::Database* database, QWidget* parent = nullptr);

    // apps 需已按 durationMs 降序；totalDurationMs 用来算占比。
    void setApps(const QVector<data::AppUsage>& apps, qint64 totalDurationMs);
    void clear();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    // 用户点击了某一行（打开该应用的详情弹窗）。
    void appClicked(const QString& exeName);

    // 快速分类：右键某一行。globalPos 是屏幕坐标，categoryId 是该行
    // 生效分类 id（可能为空 = 未分类），displayName 是展示名。
    void appContextMenuRequested(const QString& exeName, const QString& categoryId,
                                 const QString& displayName, const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    int rowAt(int localY) const;

    ::patina::data::Database* _database = nullptr;

    struct Row
    {
        QString exeName;
        QString exePath;
        QString displayName;
        QString categoryId;   // 生效分类 id；空 = 未分类（other）
        QString categoryLabel;
        QColor categoryColor;
        qint64 durationMs = 0;
        int percentage = 0;
    };

    QVector<Row> _rows;
    int _hoverRow = -1;
};

} // namespace patina::ui

#endif // PATINA_UI_DASHBOARD_APP_RANK_LIST_WIDGET_H
