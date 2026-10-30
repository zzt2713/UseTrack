#ifndef PATINA_UI_CLASSIFICATION_APP_CLASSIFICATION_CARD_H
#define PATINA_UI_CLASSIFICATION_APP_CLASSIFICATION_CARD_H

// ui/ 只经 data/ 的对外入口取数，不写 SQL、不 include platform/。
#include "data/repositories.h"

#include <QPoint>
#include <QWidget>

class ElaComboBox;
class ElaIconButton;
class ElaLineEdit;
class ElaToggleSwitch;
class QContextMenuEvent;
class QEvent;
class QLabel;
class QPushButton;

namespace patina::ui
{

// 单个应用一张卡片：改名、改分类、改颜色、切统计。
//
// 卡片只负责展示与收集改动，不碰数据库：任何一次点击都通过信号交给页面，
// 由页面记进草稿，等用户点「保存」时再整批落库（写事务在 data/ 里）。
//
// 卡片本身不持有位图缓存，隐藏时不需要额外释放。
class AppClassificationCard : public QWidget
{
    Q_OBJECT

public:
    explicit AppClassificationCard(QWidget* parent = nullptr);
    ~AppClassificationCard() override;

    // 分类下拉的候选。要在 setEntry 之前给，否则下拉是空的。
    void setCategories(const QVector<data::CategoryToken>& categories);

    // entry 是库里的状态，patch 是页面上未保存的草稿；patch 覆盖 entry 的四个可编辑字段。
    void setEntry(const data::AppClassificationEntry& entry, const data::AppClassificationPatch& patch);

    qint64 appId() const { return _appId; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

signals:
    void nameEdited(qint64 appId, const QString& displayName);
    void categoryEdited(qint64 appId, const QString& categoryId);
    void colorEdited(qint64 appId, const QString& colorHex);
    void trackEdited(qint64 appId, bool enabled);

    // 快速分类：卡片空白处右键。globalPos 是屏幕坐标，
    // 宿主页拿它去弹 QuickClassificationMenu。
    void quickMenuRequested(qint64 appId, const QPoint& globalPos);

private:
    void buildUi();
    void beginRename();
    void commitRename();
    void cancelRename();
    void pickColor();
    void refreshColorButton();
    void refreshIcon();
    void refreshTrackBadge();
    // 「不计入统计」徽标与 exe 行的次要色：两张卡片各建一次，
    // 主题切换各自刷自己的。
    void refreshThemeStyles(bool dark);

    qint64 _appId = 0;
    QString _exeName;
    QString _exePath;
    QString _displayName;
    QString _categoryId;
    QString _colorHex;
    QString _effectiveColorHex;
    bool _trackEnabled = true;
    // setEntry 期间抑制信号：否则每回填一次就假报一次改动。
    bool _updating = false;

    QVector<data::CategoryToken> _categories;

    QLabel* _iconLabel = nullptr;
    QLabel* _nameLabel = nullptr;
    QLabel* _exeLabel = nullptr;
    QLabel* _excludedBadge = nullptr;
    ElaLineEdit* _nameEdit = nullptr;
    ElaIconButton* _renameButton = nullptr;
    ElaComboBox* _categoryBox = nullptr;
    QPushButton* _colorButton = nullptr;
    ElaToggleSwitch* _trackSwitch = nullptr;
};

} // namespace patina::ui

#endif // PATINA_UI_CLASSIFICATION_APP_CLASSIFICATION_CARD_H
