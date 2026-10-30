#ifndef PATINA_UI_SHARED_QUICK_CLASSIFICATION_MENU_H
#define PATINA_UI_SHARED_QUICK_CLASSIFICATION_MENU_H

// ui/ 只经 data/ 的对外入口取数，不写 SQL、不 include platform/。
#include "data/repositories.h"

#include <functional>
#include <QWidget>

namespace patina::data
{
class Database;
}

namespace patina::ui
{

// 快速分类入口。
//
// 一个弹出菜单：重命名 + 分类子菜单。写走 data/ 的 ClassificationRepository，
// 菜单本身不碰 SQL、不持有数据库之外的状态。
//
// 两个宿主页的刷新路径不同
// （仪表盘整页重查库，分类页走草稿），所以保存动作留给宿主页面，
// 菜单只负责收集选择：
//   - showRename()：ElaInputDialog，确定后交 onRename 回调；
//   - showCategoryMenu()：QMenu 子菜单，点分类交 onCategory 回调。
// 回调签名带 exeName，宿主自己找行/卡片，避免菜单持有行指针
// （排行列表每次刷新整批重建，指针活不过一次刷新）。
class QuickClassificationMenu : public QWidget
{
    Q_OBJECT

public:
    // categorySelected 的第二个参数：空串 = 未分类（other）。
    using RenameHandler = std::function<void(const QString& exeName, const QString& displayName)>;
    using CategoryHandler = std::function<void(const QString& exeName, const QString& categoryId)>;
    // 关联应用：parentExeName 为空 = 解除关联。
    using AppLinkHandler = std::function<void(const QString& exeName,
                                              const QString& parentExeName)>;
    // 按应用的标题录制开关。captureTitle=false 表示这个应用不记窗口标题。
    using CaptureTitleHandler = std::function<void(const QString& exeName, bool captureTitle)>;
    // 「不在列表显示」：hidden=true 表示所有页面都不再出现这个应用。
    using HiddenHandler = std::function<void(const QString& exeName, bool hidden)>;

    // database 只用来读分类候选（分类可增删改名），可为空——
    // 空时退回内置分类表。不持有所有权。
    explicit QuickClassificationMenu(::patina::data::Database* database,
                                     QWidget* parent = nullptr);
    ~QuickClassificationMenu() override;

    // 两个回调必须都给：菜单不保存任何状态，选择只能交给宿主。
    void setHandlers(RenameHandler onRename, CategoryHandler onCategory);

    // 关联应用回调。不设时菜单里不出现「关联应用」子菜单
    // （宿主没打算处理这个动作，就不该给用户点）。
    void setAppLinkHandler(AppLinkHandler onAppLink);

    // 标题录制开关回调。不设时菜单里不出现「不记录窗口标题」项。
    void setCaptureTitleHandler(CaptureTitleHandler onCaptureTitle);

    // 「不在列表显示」回调。不设时菜单里不出现这一项。
    void setHiddenHandler(HiddenHandler onHidden);

    // 在 globalPos 弹出重命名对话框。exeName 是应用标识（落库键），
    // currentName 是展示名（输入框初值，空时宿主已解析好）。
    void showRename(const QString& exeName, const QString& currentName, const QPoint& globalPos);

    // 在 globalPos 弹出分类子菜单（顶部带「重命名…」项）。
    // 返回 true 表示用户点了「重命名…」，宿主应接着调 showRename()；
    // 返回 false 表示菜单关闭或已提交分类。currentCategoryId 是生效分类 id
    // （打勾用），传空串按 other 处理。
    //
    // currentParentExeName 是当前已关联的父应用（未关联传空），
    // 子菜单用它打勾；传 parentExeName 非空时菜单里会出现「关联应用」子菜单。
    //
    // captureTitle 是当前生效值（true = 记录标题），用于打勾。
    //
    // hidden 是当前「不在列表显示」状态，用于打勾。
    bool showCategoryMenu(const QString& exeName, const QString& currentCategoryId,
                          const QPoint& globalPos,
                          const QString& currentParentExeName = QString(),
                          bool captureTitle = true,
                          bool hidden = false);

private:
    ::patina::data::Database* _database = nullptr;
    RenameHandler _onRename;
    CategoryHandler _onCategory;
    AppLinkHandler _onAppLink;
    CaptureTitleHandler _onCaptureTitle;
    HiddenHandler _onHidden;
    QString _pendingExeName;
};

} // namespace patina::ui

#endif // PATINA_UI_SHARED_QUICK_CLASSIFICATION_MENU_H
