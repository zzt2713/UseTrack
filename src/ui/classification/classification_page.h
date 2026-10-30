#ifndef PATINA_UI_CLASSIFICATION_CLASSIFICATION_PAGE_H
#define PATINA_UI_CLASSIFICATION_CLASSIFICATION_PAGE_H

// ui/ 只经 data/ 的对外入口取数，不写 SQL、不 include platform/。
#include "data/repositories.h"

#include <QHash>
#include <QPoint>
#include <QVector>
#include <QWidget>

#include <functional>

class QGridLayout;
class QLabel;
class QPushButton;
class ElaIconButton;
class ElaLineEdit;
class QVBoxLayout;

namespace patina::ui
{

class AppClassificationCard;
class QuickClassificationMenu;

// 「分类管理」页（方案 B · 卡片流）。
//
// 左侧常驻分类栏（点一档只看这一档），右侧两列卡片逐应用调整。
//
// 编辑模型是「攒草稿 → 点保存」：卡片上的每次点击只改内存里的 AppClassificationPatch，
// 不碰数据库；点「保存」时把有变化的条目整批交给 ClassificationRepository，
// 由它在同一个写事务里落库，任何一条失败整批回滚。
//
// 页面隐藏时清空卡片列表，把几百个控件和它们的字体度量一起释放。
class ClassificationPage : public QWidget
{
    Q_OBJECT

public:
    explicit ClassificationPage(data::Database* database, QWidget* parent = nullptr);
    ~ClassificationPage() override;

    void setActive(bool active);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    // 左栏色点的点击转发：色点（QLabel）点击直接弹 ElaColorDialog 改分类色。
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildUi();
    void reload();
    void rebuildRail();
    void clearCards();
    void refreshHeader();
    void applyFilter();
    void onSave();
    void onDiscard();

    // ---- 分类本身的增删改名 / 配色（即时落库，走 ClassificationRepository）----
    // 应用级归属仍是「攒草稿 → 保存」；分类级操作影响面大且低频，即时生效。
    void onCreateCategory();
    void onRenameCategory(const QString& categoryId, const QString& currentLabel);
    void onDeleteCategory(const QString& categoryId, const QString& label);
    void onCategoryColor(const QString& categoryId, const QString& currentColorHex);
    // 分类级操作后统一刷新：重读注册表 + 重铺左栏与卡片（草稿保留）。
    void reloadAfterCategoryChange();

    // 分类栏计数、空态提示、脏标记都靠样式表上色，主题一换要重铺。
    // 脏标记的颜色还跟「有没有未保存改动」有关，所以按当前草稿重算一次。
    void refreshThemeStyles(bool dark);
    // 侧栏计数与空态用的次要色片段。
    QString secondaryStyle() const;

    data::AppClassificationPatch patchFor(const data::AppClassificationEntry& entry) const;

    // 单字段改动：取出当前草稿（或库里的状态），只改一个字段，再交给 updatePatch 判断是否还算「脏」。
    void applyEdit(qint64 appId, const std::function<void(data::AppClassificationPatch&)>& mutate);
    void updatePatch(const data::AppClassificationPatch& patch);

    // 快速分类：卡片右键 → 重命名 / 改分类。分类页是「攒草稿 → 保存」模型，
    // 菜单的选择只记进草稿，落库仍走页面统一的「保存」按钮，不直写数据库。
    void openCardQuickMenu(qint64 appId, const QPoint& globalPos);
    // exeName（COLLATE NOCASE）→ 库条目；快速分类回调按 exeName 找条目。
    const data::AppClassificationEntry* entryByExeName(const QString& exeName) const;
    // 关联应用：把 exeName 并到 parentExeName 名下（空 = 解除）。
    // 关联不是分类草稿的一部分（它只改统计归并口径，不动 apps 表），
    // 所以这条直接落库，与「攒草稿 → 保存」的模型互不影响。
    void saveAppLink(const QString& exeName, const QString& parentExeName);
    // 按应用标题录制：写 settings 的 __app_override 键，同样不进草稿。
    void saveAppCaptureTitle(const QString& exeName, bool captureTitle);
    // 「不在列表显示」：隐藏后卡片从列表消失，所以这里直接落库（不进草稿），
    // 否则用户勾了隐藏却还看得见卡片，得再点一次保存才生效，很容易以为没生效。
    void saveAppHidden(const QString& exeName, bool hidden);
    // 被隐藏的应用在分类页也不出现，需要一个找回入口：
    // 页头「已隐藏 N 个」按钮弹出清单，点一下就恢复显示。
    void showHiddenAppsMenu();
    QuickClassificationMenu* _quickClassification = nullptr;

    data::Database* _database = nullptr;
    bool _active = false;
    bool _loading = false;
    // 已经成功读过一次库：用来区分「还没加载」和「加载了但库里没应用」。
    bool _loaded = false;

    // 库里的状态，以及页面上的草稿覆盖（只存被改过的应用）。
    QVector<data::AppClassificationEntry> _entries;
    QHash<qint64, data::AppClassificationPatch> _patches;
    // 生效分类（注册表），带 builtin/deletable 标记供左栏操作按钮显隐。
    QVector<data::CategoryEntry> _categoryEntries;
    QVector<data::CategoryToken> _categories;

    // 「全部」用空串表示，其余是分类 id。
    QString _categoryFilter;
    QString _searchText;

    QLabel* _summaryLabel = nullptr;
    QLabel* _dirtyLabel = nullptr;
    QLabel* _countLabel = nullptr;
    ElaLineEdit* _searchEdit = nullptr;
    QPushButton* _saveButton = nullptr;
    QPushButton* _discardButton = nullptr;
    // 有隐藏应用时才出现的找回入口（见 showHiddenAppsMenu）。
    QPushButton* _hiddenButton = nullptr;
    QPushButton* _addCategoryButton = nullptr;
    // 双列/单列切换（ElaIconButton 透明图标按钮，图标随模式变）。
    ElaIconButton* _layoutButton = nullptr;
    void refreshLayoutButton();
    QVBoxLayout* _railLayout = nullptr;
    QGridLayout* _cardsLayout = nullptr;
    QWidget* _cardsHost = nullptr;
    QVector<AppClassificationCard*> _cards;
};

} // namespace patina::ui

#endif // PATINA_UI_CLASSIFICATION_CLASSIFICATION_PAGE_H
