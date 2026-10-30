#ifndef PATINA_APP_MAIN_WINDOW_H
#define PATINA_APP_MAIN_WINDOW_H

#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QSystemTrayIcon>
#include <QVector>

#include <atomic>
#include <memory>

#include "ElaWindow.h"

class ElaMenu;
class QAction;
class QWidget;

namespace patina::app
{
class TrackingService;
} // namespace patina::app

namespace patina::ui
{
class AboutPage;
class ClassificationPage;
class DashboardPage;
class DataPage;
class HistoryPage;
class SettingsPage;
class ToolsPage;
} // namespace patina::ui

// 主窗口：左侧导航 + 六个页面 + 托盘。
class MainWindow : public ElaWindow
{
    Q_OBJECT

public:
    // tracking 由 main.cpp 持有并保证活得比窗口久；它提供已打开的数据库句柄。
    explicit MainWindow(patina::app::TrackingService* tracking, QWidget* parent = nullptr);

    // 开机自启时用：只把页面与托盘备好，不显示窗口。
    // 不做这一步的话，最小化启动的那次运行里工具页迟迟不建，
    // 重启恢复与提醒都不会跑（它们挂在页面初始化流程上）。
    void prepareTrayStart();

    // 单实例激活——第二实例到来时唤醒主窗口。
    // 走 showFromTray 的路径（解除最小化、置顶、取消隐藏），封装成公有
    // 是因为 QMetaObject::invokeMethod 需要可访问的入口。
    void activateFromSecondInstance();

    // 壁纸下载在后台线程跑，线程里握着的窗口指针必须在窗口析构前收掉。
    ~MainWindow() override;

protected:
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void initWindow();
    void initUserCard();
    void initContent();
    void initCloseBehavior();
    void initTray();

    void addPlaceholderPage(const QString& title, ElaIconType::IconName icon);
    // 下面七对：add*Page 只建外壳（ElaScrollPage + 内容宿主 + 导航节点，一次生命周期内
    // 不动），create*Content 建页面本体并挂进宿主。收托盘只拆内容，外壳留着——
    // 摘导航节点会让 Ela 的 _pSelectedNode 悬空、_navigationTargetIndex 陈旧，
    // 重建时必崩或切页被拦下（白屏）。
    void addDashboardPage();
    void addHistoryPage();
    void addDataPage();
    void addClassificationPage();
    void addToolsPage();
    void addSettingsPage();
    void addAboutPage();
    void createDashboardContent();
    void createHistoryContent();
    void createDataContent();
    void createClassificationContent();
    void createToolsContent();
    void createSettingsContent();
    void createAboutContent();
    // 建一页外壳（ElaScrollPage + 空内容宿主 + 导航节点），返回内容宿主；失败返回 nullptr。
    // 只调一次：摘导航节点会让 Ela 内部缓存的选中节点悬空（崩溃根因）。
    QWidget* addPageShell(const QString& title, ElaIconType::IconName icon,
                          QString* pageKeyOut);
    // 工具状态活在页面之外（ToolsRuntime 单例）：托盘启动时页面不建，
    // 但绑定数据库与提醒连接必须照做，否则重启恢复和到点通知都不会跑。
    void initToolsRuntime();
    void onPageChanged(QString pageKey);
    // 整窗进/出托盘时统一改页面活动态：收托盘要放掉页面缓存。
    void setPagesActive(bool active);
    // 收托盘把页面本体拆掉，外壳与导航节点留着（摘节点会让 Ela 内部
    // 缓存的选中节点悬空、导航目标索引陈旧），回前台再按原页重建内容。
    void destroyPages();
    void rebuildPages();
    // 设置页只发信号：注册表读写归 platform/，由这里转手（ui/ 不 include platform/）。
    void applyAutostart(bool enabled);
    // —— 三面板信号的消费端（ui/ 不 include platform/，行为在 app/ 层落地）——
    // 追踪面板保存后 / 托盘菜单切换后：同步到 TrackingService（库已写好）。
    void applyTrackingPause(bool paused);
    // 同上：标题录制开关（立刻更新读缓存，关闭时清当前段标题）。
    void applyTitleRecording(bool enabled);
    // 引擎状态 → 托盘菜单文案/图标（双向同步的「引擎 → 菜单」方向）。
    void syncTrayMenuState();
    // 外观面板：三值主题模式换算成 ElaTheme 的 Light/Dark + QPalette。
    // "system" 挂在 QStyleHints::colorSchemeChanged 上跟随。
    void applyThemeMode(const QString& mode);
    // 常驻面板：托盘显隐与关闭/最小化行为。
    void applyTrayIconVisible(bool visible);
    void applyCloseBehavior(bool closeToTray);
    void applyMinimizeBehavior(bool minimizeToTray);

    // —— 壁纸（外观面板发起）——
    // 开关/来源变了：写库 + 按需立刻换图或撤图。
    void applyWallpaperSettings(bool enabled, const QString& source);
    // 下载 → 存盘 → 解码 → 贴图。网络在后台线程，结果回主线程落 UI。
    void refreshWallpaper(const QString& source);
    // 把盘上的壁纸贴到窗口（主题两档同一张）；没有文件或解码失败就撤掉壁纸。
    void applyWallpaperToWindow();
    // 撤掉壁纸：先切回 Normal 再清 pixmap（Ela 的 Pixmap 分支会直接解引用空图）。
    void clearWallpaper();
    // 透明度（20~100）：只重算贴到窗口的那张图，不重新下载。
    void applyWallpaperOpacity(int percent);

    // 整库被替换过（恢复备份 / 清理历史）：让各页面丢掉旧数据重查。
    void reloadPagesAfterDataChange();
    QIcon makeTrayIcon() const;
    bool isDarkTaskbar() const;
    void refreshTrayIcon();
    // 工具状态变了就换托盘图标与提示文字（进度环也画在图标上）。
    void refreshTrayProgress();

    void hideToTray();
    void showFromTray();
    void quitApplication();
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);

    patina::app::TrackingService* _tracking = nullptr;
    // 页面本体：收托盘时 delete 并置空，回前台由 create*Content 重建。
    patina::ui::DashboardPage* _dashboardPage = nullptr;
    patina::ui::HistoryPage* _historyPage = nullptr;
    patina::ui::DataPage* _dataPage = nullptr;
    patina::ui::ClassificationPage* _classificationPage = nullptr;
    patina::ui::ToolsPage* _toolsPage = nullptr;
    patina::ui::SettingsPage* _settingsPage = nullptr;
    patina::ui::AboutPage* _aboutPage = nullptr;
    // 内容宿主：挂在外壳里，本体被拆后它还在，重建时往这里放。
    QWidget* _dashboardHost = nullptr;
    QWidget* _historyHost = nullptr;
    QWidget* _dataHost = nullptr;
    QWidget* _classificationHost = nullptr;
    QWidget* _toolsHost = nullptr;
    QWidget* _settingsHost = nullptr;
    QWidget* _aboutHost = nullptr;
    QSystemTrayIcon* _trayIcon = nullptr;
    ElaMenu* _trayMenu = nullptr;
    // 这两项的文案、图标、可用态随引擎状态变，必须留引用。
    QAction* _trayPauseAction = nullptr;
    QAction* _trayTitleAction = nullptr;
    QString _firstPageKey;
    // 各页外壳（ElaScrollPage），顺序即导航顺序，索引用于记住停在第几页。
    QVector<QWidget*> _pageShells;
    // 收托盘拆内容时记下原来停在第几页，重建后按位置切回去。
    int _activePageIndex = 0;
    bool _pagesDestroyed = false;
    // navigationNodeClicked 只连一次：initContent 在首次显示时跑。
    bool _navigationConnected = false;
    QString _dashboardPageKey;
    QString _historyPageKey;
    QString _dataPageKey;
    QString _classificationPageKey;
    QString _toolsPageKey;
    QString _settingsPageKey;
    QString _aboutPageKey;
    // 当前显示的页面：恢复备份 / 清理历史后要靠它判断该让哪一页重查。
    QString _activePageKey;
    bool _contentInitialized = false;
    // 工具状态与会话通知的连接只挂一次（托盘启动路径可能先跑一遍）。
    bool _toolsRuntimeBound = false;
    bool _darkTaskbar = false;
    // —— 常驻行为的运行时镜像（设置页写库后经信号同步到这里）——
    bool _trayIconVisible = true;
    bool _closeToTray = true;
    bool _minimizeToTray = false;
    // 主题模式的运行时镜像（"light"|"dark"|"system"）：system 时监听系统色。
    QString _themeModeRuntime = QStringLiteral("system");
    bool _themeSystemConnected = false;

    // —— 壁纸 ——
    // 窗口底图按主题分两档，但壁纸只有一张，两档设同一个 pixmap。
    bool _wallpaperEnabled = false;
    QString _wallpaperSource = QStringLiteral("loliapi");
    // 正在下载时置位：避免连点「换一张」开出一串线程。
    bool _wallpaperLoading = false;
    // 透明度百分数（20~100）：贴图时按它调 alpha，不改变盘上的原图。
    int _wallpaperOpacity = 100;
    // 降透明度时留一份解码后的原图，拖滑块重合成时不用每次重解 JPEG。
    // 100% 时置空，这一份位图只在真的调过透明度后才占内存。
    QPixmap _wallpaperOriginal;
    // 后台线程只读这个原子量决定要不要把结果丢回来（窗口在下载途中被销毁）。
    // 线程 detach 出去，不 join：网络最长 25s 超时，退出时卡住主线程不可接受。
    std::shared_ptr<std::atomic_bool> _wallpaperAlive;
};

#endif // PATINA_APP_MAIN_WINDOW_H
