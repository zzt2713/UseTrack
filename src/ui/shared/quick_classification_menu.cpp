#include "ui/shared/quick_classification_menu.h"

#include "ElaInputDialog.h"
#include "ElaMenu.h"
#include "ElaTheme.h"

#include <QAction>
#include <QHash>
#include <QList>
#include <QPoint>
#include <QStringList>

#include "data/app_category.h"
#include "data/app_links.h"
#include "data/category_registry.h"

namespace patina::ui
{

QuickClassificationMenu::QuickClassificationMenu(::patina::data::Database* database,
                                                 QWidget* parent)
    : QWidget(parent), _database(database)
{
    // 不参与布局，也不抢焦点：两个 show* 的宿主都可能是自绘控件，
    // this 只是给对话框/菜单一个父对象保证层级正确。
    setFixedSize(0, 0);
}

QuickClassificationMenu::~QuickClassificationMenu() = default;

void QuickClassificationMenu::setHandlers(RenameHandler onRename, CategoryHandler onCategory)
{
    _onRename = std::move(onRename);
    _onCategory = std::move(onCategory);
}

void QuickClassificationMenu::setAppLinkHandler(AppLinkHandler onAppLink)
{
    _onAppLink = std::move(onAppLink);
}

void QuickClassificationMenu::setCaptureTitleHandler(CaptureTitleHandler onCaptureTitle)
{
    _onCaptureTitle = std::move(onCaptureTitle);
}

void QuickClassificationMenu::setHiddenHandler(HiddenHandler onHidden)
{
    _onHidden = std::move(onHidden);
}

void QuickClassificationMenu::showRename(const QString& exeName, const QString& currentName,
                                         const QPoint& globalPos)
{
    Q_UNUSED(globalPos)
    if (exeName.trimmed().isEmpty() || !_onRename) {
        return;
    }
    _pendingExeName = exeName;

    QWidget* top = window();
    bool accepted = false;
    // 标题「重命名」，初值是当前展示名，确定才提交。
    const QString text = ElaInputDialog::getText(
        top, tr("重命名"), tr("为「%1」设置显示名称").arg(currentName), tr("显示名称"),
        currentName, &accepted);
    if (!accepted) {
        return;
    }

    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed == currentName.trimmed()) {
        return;
    }
    _onRename(_pendingExeName, trimmed);
}

bool QuickClassificationMenu::showCategoryMenu(const QString& exeName,
                                               const QString& currentCategoryId,
                                               const QPoint& globalPos,
                                               const QString& currentParentExeName,
                                               bool captureTitle,
                                               bool hidden)
{
    if (exeName.trimmed().isEmpty() || !_onCategory) {
        return false;
    }
    _pendingExeName = exeName;

    // 按用户要求用 ElaMenu（注意：库的展开动画在尺寸对不上时可能文字错位、
    // hover 底色切深色后可能不刷新——出现这两条就是这个原因，届时退回
    // QMenu + applyMenuTheme）。堆上 + DeleteOnClose：ElaMenu 的展开动画
    // 要对象活得比 exec() 返回久一点，栈对象在动画中析构会闪。
    auto* menu = new ElaMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    // 菜单结构：重命名在上，分类在下。
    QAction* renameAction = menu->addAction(tr("重命名…"));

    menu->addSeparator();

    // 候选走注册表（分类可增删改名）。数据库不可用或读失败时退回内置表。
    const QString effective = currentCategoryId.trimmed().isEmpty()
        ? QStringLiteral("other")
        : currentCategoryId.trimmed();

    QStringList labels;
    QStringList ids;
    if (_database != nullptr && _database->isOpen()) {
        const auto registry = ::patina::data::loadCategoryRegistry(*_database);
        if (registry.ok()) {
            for (const auto& entry : registry.value) {
                ids.append(entry.id);
                labels.append(entry.label);
            }
        }
    }
    if (ids.isEmpty()) {
        const QVector<::patina::data::CategoryToken> tokens = ::patina::data::categoryTokens();
        for (const auto& token : tokens) {
            ids.append(token.id);
            labels.append(token.label);
        }
    }

    for (int index = 0; index < ids.size(); ++index) {
        QAction* action = menu->addAction(labels.at(index));
        action->setData(ids.at(index));
        action->setCheckable(true);
        action->setChecked(ids.at(index) == effective);
    }

    // 关联应用：把当前应用并到另一个应用名下（或解除）。
    // 候选是「父应用的候选」——只列还没有关联到别人名下的应用，
    // 避免用户在这里造出一条链（链会让统计归并键不唯一）。
    QAction* unlinkAction = nullptr;
    QHash<QString, QAction*> linkActions;
    if (_onAppLink && _database != nullptr && _database->isOpen()) {
        const ::patina::data::Result<::patina::data::AppLinks> loadedLinks =
            ::patina::data::loadAppLinks(*_database);
        const ::patina::data::AppLinks links = loadedLinks.ok()
            ? loadedLinks.value
            : ::patina::data::AppLinks{};
        const QString selfKey = exeName.trimmed().toLower();
        const QString currentParent = currentParentExeName.trimmed().toLower();

        menu->addSeparator();
        ElaMenu* linkMenu = menu->addMenu(tr("关联应用"));
        if (!currentParent.isEmpty()) {
            unlinkAction = linkMenu->addAction(tr("解除关联（当前：%1）").arg(currentParent));
        }

        const ::patina::data::Result<QHash<QString, ::patina::data::AppIdentity>> loadedIdentities =
            ::patina::data::loadAppIdentities(*_database);
        if (loadedIdentities.ok()) {
            QStringList parents;
            for (auto it = loadedIdentities.value.cbegin();
                 it != loadedIdentities.value.cend(); ++it) {
                // 自己不能当自己的父；已经是别人成员的应用不能当父。
                if (it.key() == selfKey || links.contains(it.key())) {
                    continue;
                }
                parents.append(it.key());
            }
            parents.sort();
            for (const QString& parentKey : parents) {
                const ::patina::data::AppIdentity identity = loadedIdentities.value.value(parentKey);
                const QString label = identity.displayName.trimmed().isEmpty()
                    ? identity.exeName
                    : identity.displayName.trimmed();
                QAction* action = linkMenu->addAction(label);
                action->setCheckable(true);
                action->setChecked(parentKey == currentParent);
                action->setData(parentKey);
                linkActions.insert(parentKey, action);
            }
        }
        if (linkMenu->isEmpty()) {
            // 没有任何可选的父应用：把子菜单摘掉，别留一个空菜单。
            menu->removeAction(linkMenu->menuAction());
            delete linkMenu;
            linkMenu = nullptr;
            if (unlinkAction == nullptr) {
                // 连「解除」都没有，就把上面那条分隔线也去掉。
                const QList<QAction*> actions = menu->actions();
                if (!actions.isEmpty() && actions.last()->isSeparator()) {
                    menu->removeAction(actions.last());
                }
            }
        }
    }

    // 按应用的标题录制开关。没有专门的应用弹窗，就近放到这里——
    // 右键任意应用就能关掉它的
    // 标题记录，不用先去分类页找到卡片。只在宿主给了回调时出现。
    QAction* captureTitleAction = nullptr;
    if (_onCaptureTitle) {
        menu->addSeparator();
        captureTitleAction = menu->addAction(tr("记录窗口标题"));
        captureTitleAction->setCheckable(true);
        captureTitleAction->setChecked(captureTitle);
        captureTitleAction->setToolTip(tr("关掉后这个应用只记时长，不记标题"));
    }

    // 「不在列表显示」：与 track_enabled（不计入统计）不同——隐藏后连列表、
    // 图例、排行、提醒规则统计里都不出现，历史行仍在库里，取消隐藏即恢复。
    // 放在菜单最后，与上面两个开关同一档。
    QAction* hiddenAction = nullptr;
    if (_onHidden) {
        menu->addSeparator();
        hiddenAction = menu->addAction(tr("不在列表显示"));
        hiddenAction->setCheckable(true);
        hiddenAction->setChecked(hidden);
        hiddenAction->setToolTip(tr("勾上后所有页面都不再显示这个应用，记录本身不受影响"));
    }

    QAction* selected = menu->exec(globalPos);
    if (selected == nullptr) {
        return false;
    }
    if (selected == renameAction) {
        // 展示名宿主自己解析过，这里直接回给宿主，由它调 showRename。
        return true;
    }
    if (captureTitleAction != nullptr && selected == captureTitleAction) {
        // 勾选状态已经由 QAction 自己翻转了，取反后再交出去。
        _onCaptureTitle(_pendingExeName, !captureTitle);
        return false;
    }
    if (hiddenAction != nullptr && selected == hiddenAction) {
        // 勾上「不在列表显示」= 隐藏，取消勾选 = 恢复显示。
        _onHidden(_pendingExeName, !hidden);
        return false;
    }
    if (unlinkAction != nullptr && selected == unlinkAction) {
        _onAppLink(_pendingExeName, QString());
        return false;
    }
    if (!linkActions.isEmpty() && linkActions.values().contains(selected)) {
        _onAppLink(_pendingExeName, selected->data().toString());
        return false;
    }
    const QString categoryId = selected->data().toString();
    if (categoryId == effective) {
        return false;
    }
    // 选「其他」= 清空覆盖项：回调的 categoryId 传 null QString 区分
    // 「清空」（other）与「未指定」。空串在 saveQuickOverride 语义里是「保留原值」，
    // 所以清空必须传 null QString。
    _onCategory(_pendingExeName, categoryId == QStringLiteral("other")
                                     ? QString(QString())  // null：清空覆盖
                                     : categoryId);
    return false;
}

} // namespace patina::ui
