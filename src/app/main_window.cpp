#include "main_window.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStyleHints>
#include <QVBoxLayout>
#include <QWidget>

#include <QEvent>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <thread>

#include "ElaMenu.h"
#include "ElaScrollPage.h"
#include "ElaText.h"
#include "ElaTheme.h"

#include "app/tracking_service.h"
#include "data/repositories.h"
#include "platform/autostart.h"
#include "platform/process_resources.h"
#include "platform/wallpaper_fetcher.h"
#include "shared/app_icon_cache.h"
#include "ui/about/about_page.h"
#include "ui/classification/classification_page.h"
#include "ui/dashboard/dashboard_page.h"
#include "ui/data/data_page.h"
#include "ui/history/history_page.h"
#include "ui/settings/settings_config.h"
#include "ui/settings/settings_page.h"
#include "ui/tools/tools_page.h"
#include "ui/tools/tools_runtime.h"

namespace
{

// 到点通知在托盘气泡里停留的时长。
constexpr int kTrayMessageMsec = 6000;
// 托盘图标是 32px 的目标尺寸（Windows 会按 DPI 缩放），进度环画在这个坐标系里。
constexpr int kTrayIconSize = 32;
constexpr qreal kTrayArcWidth = 4.0;
constexpr qreal kTrayArcMargin = 2.0;
// 壁纸透明度区间，与设置页滑块一致：0% 等于没开壁纸，不放开。
constexpr int kWallpaperOpacityMin = 20;
constexpr int kWallpaperOpacityFull = 100;
constexpr int kWallpaperOpacityDefault = 100;

} // namespace

MainWindow::MainWindow(patina::app::TrackingService* tracking, QWidget* parent)
    : ElaWindow(parent), _tracking(tracking)
{
    _wallpaperAlive = std::make_shared<std::atomic_bool>(true);
    initWindow();
    initUserCard();
    initCloseBehavior();
    initTray();
}

MainWindow::~MainWindow()
{
    // 后台下载线程可能还活着：先让它下次醒来就放弃回填，别碰已析构的 this。
    if (_wallpaperAlive) {
        _wallpaperAlive->store(false);
    }
}

void MainWindow::initWindow()
{
    // 关闭按钮交给 initCloseBehavior 分派（收托盘或退出），不从窗口层直接关。
    setIsDefaultClosed(false);
    // 单窗口应用，禁止页面弹出新窗口。
    setIsAllowPageOpenInNewWindow(false);

    setWindowTitle(tr("UseTrack"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));
    _darkTaskbar = isDarkTaskbar();
    // 左侧导航栏 Ela 默认 300px 偏宽，收窄到 240px；库对小于 180px 的值会强制回 180。
    setNavigationBarWidth(50);
    setMinimumSize(900, 600);
    setNavigationBarDisplayMode(ElaNavigationType::Compact);
    // 切页动画关掉：Ela 默认的 Popup 模式会延迟 180ms 再取目标页并 grab 整个
    // 中央堆栈，收托盘拆页面树时那个延迟回调会拿到空页直接崩（ElaWidgetTools
    // 0xf537a 处的空指针解引用）。本应用切页要的是立刻生效，不是过场。
    setStackSwitchMode(ElaWindowType::StackSwitchMode::None);
    resize(1100, 720);
}

void MainWindow::initUserCard()
{
    setUserInfoCardVisible(false);
}

void MainWindow::initContent()
{
    // 工具状态与提醒连接先就位：托盘启动路径可能已经跑过一次，
    // initToolsRuntime 内部有一次性守卫。
    initToolsRuntime();

    // 先建外壳（ElaScrollPage + 内容宿主 + 导航节点），一次生命周期内不再动；
    // 收托盘只拆里面的页面本体，回前台再逐页重建。
    addDashboardPage();
    addHistoryPage();
    addDataPage();
    addClassificationPage();
    addToolsPage();
    addSettingsPage();
    addAboutPage();

    // 内容本体：首次进入时建；收托盘拆掉后由 rebuildPages 走同一批函数重建。
    createDashboardContent();
    createHistoryContent();
    createDataContent();
    createClassificationContent();
    createToolsContent();
    createSettingsContent();
    createAboutContent();

    // initContent 只在首次显示时跑一次，导航信号也一并只连一次。
    if (_navigationConnected) {
        return;
    }
    _navigationConnected = true;

    // 页面可见性驱动刷新：切走就停表并释放列表，切回来再查一次库。
    connect(this, &ElaWindow::navigationNodeClicked, this,
            [this](ElaNavigationType::NavigationNodeType nodeType, const QString& nodeKey) {
                if (nodeType == ElaNavigationType::PageNode) {
                    onPageChanged(nodeKey);
                }
            });
}

void MainWindow::initCloseBehavior()
{
    connect(this, &ElaWindow::closeButtonClicked, this, [this]() {
        // 关闭行为二选一、直接执行：
        // 「到托盘」直接收托盘，「退出」直接退出，都不再弹确认框。
        if (_closeToTray) {
            hideToTray();
        } else {
            quitApplication();
        }
    });
}

void MainWindow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::WindowStateChange && _minimizeToTray) {
        // 最小化行为=「到托盘」时，任务栏最小化改走收托盘。
        // isMinimized 判断放在状态变更后（WindowStateChange 时新状态已生效）。
        if (isMinimized()) {
            // 先还原窗口状态再隐藏：直接 hide() 会把 minimized 状态带进托盘，
            // 从托盘唤回时窗口会以最小化形态出现。
            setWindowState(windowState() & ~Qt::WindowMinimized);
            hideToTray();
        }
    }
    ElaWindow::changeEvent(event);
}

QWidget* MainWindow::addPageShell(const QString& title, ElaIconType::IconName icon,
                                  QString* pageKeyOut)
{
    auto* page = new ElaScrollPage();

    // 内容宿主：页面本体拆掉后它还留在外壳里，下次重建直接往这儿放。
    // 外壳与导航节点整个生命周期只建一次——摘节点会让 Ela 内部缓存的选中节点
    // 悬空、导航目标索引陈旧，重建时崩在 getRow() 或切页被拦下（白屏）。
    auto* host = new QWidget(page);
    host->setWindowTitle(title);
    // 宿主只是页面本体的容器：跟 ElaScrollPage 自己的容器一样透明，
    // 否则会多出一层底色盖在滚动区上。
    host->setObjectName(QStringLiteral("ElaPageContentHost"));
    host->setStyleSheet(
        QStringLiteral("#ElaPageContentHost{background-color:transparent;}"));
    // 纵向跟着外壳长：里面只有页面本体，不需要自己的首选高度。
    host->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);

    page->setTitleVisible(false);
    page->addCentralWidget(host);

    if (addPageNode(title, page, icon) != ElaNavigationType::Success) {
        delete page;
        return nullptr;
    }

    // pageKey 由导航栏内部生成，只能从页面属性反查。
    const QString pageKey = page->property("ElaPageKey").toString();
    if (pageKeyOut != nullptr) {
        *pageKeyOut = pageKey;
    }
    _pageShells.append(page);
    if (_firstPageKey.isEmpty()) {
        _firstPageKey = pageKey;
    }
    return host;
}

void MainWindow::addDashboardPage()
{
    if (_dashboardHost != nullptr) {
        return;
    }
    _dashboardHost = addPageShell(tr("今日概览"), ElaIconType::House, &_dashboardPageKey);
}

void MainWindow::createDashboardContent()
{
    if (_dashboardHost == nullptr || _dashboardPage != nullptr) {
        return;
    }
    // 追踪没起来时数据库句柄为空：页面照样建，显示「数据库未就绪」而不是空白。
    patina::data::Database* database = _tracking != nullptr ? _tracking->database() : nullptr;

    _dashboardPage = new patina::ui::DashboardPage(database, _dashboardHost);
    _dashboardHost->layout()->addWidget(_dashboardPage);
}

void MainWindow::onPageChanged(QString pageKey)
{
    _activePageKey = pageKey;
    // 记下当前在第几页：收托盘会拆掉内容，回前台按位置切回去。
    for (int i = 0; i < _pageShells.size(); ++i) {
        if (_pageShells.at(i)->property("ElaPageKey").toString() == pageKey) {
            _activePageIndex = i;
            break;
        }
    }
    if (_dashboardPage != nullptr) {
        _dashboardPage->setActive(!_dashboardPageKey.isEmpty() && pageKey == _dashboardPageKey);
    }
    if (_historyPage != nullptr) {
        _historyPage->setActive(!_historyPageKey.isEmpty() && pageKey == _historyPageKey);
    }
    if (_dataPage != nullptr) {
        _dataPage->setActive(!_dataPageKey.isEmpty() && pageKey == _dataPageKey);
    }
    if (_classificationPage != nullptr) {
        _classificationPage->setActive(!_classificationPageKey.isEmpty()
                                       && pageKey == _classificationPageKey);
    }
    if (_toolsPage != nullptr) {
        _toolsPage->setActive(!_toolsPageKey.isEmpty() && pageKey == _toolsPageKey);
    }
    if (_settingsPage != nullptr) {
        _settingsPage->setActive(!_settingsPageKey.isEmpty() && pageKey == _settingsPageKey);
    }
    if (_aboutPage != nullptr) {
        _aboutPage->setActive(!_aboutPageKey.isEmpty() && pageKey == _aboutPageKey);
    }
}

void MainWindow::setPagesActive(bool active)
{
    const auto apply = [active](auto* page) {
        if (page != nullptr) {
            page->setActive(active);
        }
    };
    // active=false 时各页的 setActive 会清掉图表/列表缓存并停表。
    apply(_dashboardPage);
    apply(_historyPage);
    apply(_dataPage);
    apply(_classificationPage);
    apply(_toolsPage);
    apply(_settingsPage);
    apply(_aboutPage);
}

void MainWindow::addHistoryPage()
{
    if (_historyHost != nullptr) {
        return;
    }
    _historyHost = addPageShell(tr("历史"), ElaIconType::Clock, &_historyPageKey);
}

void MainWindow::createHistoryContent()
{
    if (_historyHost == nullptr || _historyPage != nullptr) {
        return;
    }
    patina::data::Database* database = _tracking != nullptr ? _tracking->database() : nullptr;

    _historyPage = new patina::ui::HistoryPage(database, _historyHost);
    _historyHost->layout()->addWidget(_historyPage);
}

void MainWindow::addDataPage()
{
    if (_dataHost != nullptr) {
        return;
    }
    _dataHost = addPageShell(tr("数据"), ElaIconType::ChartLine, &_dataPageKey);
}

void MainWindow::createDataContent()
{
    if (_dataHost == nullptr || _dataPage != nullptr) {
        return;
    }
    patina::data::Database* database = _tracking != nullptr ? _tracking->database() : nullptr;

    _dataPage = new patina::ui::DataPage(database, _dataHost);
    _dataHost->layout()->addWidget(_dataPage);
}

void MainWindow::addClassificationPage()
{
    if (_classificationHost != nullptr) {
        return;
    }
    _classificationHost =
        addPageShell(tr("分类"), ElaIconType::Tags, &_classificationPageKey);
}

void MainWindow::createClassificationContent()
{
    if (_classificationHost == nullptr || _classificationPage != nullptr) {
        return;
    }
    patina::data::Database* database = _tracking != nullptr ? _tracking->database() : nullptr;

    _classificationPage = new patina::ui::ClassificationPage(database, _classificationHost);
    _classificationHost->layout()->addWidget(_classificationPage);
}

void MainWindow::addToolsPage()
{
    if (_toolsHost != nullptr) {
        return;
    }
    _toolsHost = addPageShell(tr("工具"), ElaIconType::ScrewdriverWrench, &_toolsPageKey);
}

void MainWindow::createToolsContent()
{
    if (_toolsHost == nullptr || _toolsPage != nullptr) {
        return;
    }
    patina::data::Database* database = _tracking != nullptr ? _tracking->database() : nullptr;

    _toolsPage = new patina::ui::ToolsPage(database, _toolsHost);
    _toolsHost->layout()->addWidget(_toolsPage);
}

void MainWindow::initToolsRuntime()
{
    patina::data::Database* database = _tracking != nullptr ? _tracking->database() : nullptr;

    // 工具状态在页面之外（ToolsRuntime 单例），先把它接到同一个数据库上：
    // 页面还没建的时候，重启恢复就要跑一遍。
    patina::ui::ToolsRuntime& runtime = patina::ui::ToolsRuntime::instance();
    // attach 对同一个库是幂等的，但下面的 connect 用 lambda 不去重，函数体只能进一次。
    if (_toolsRuntimeBound) {
        return;
    }
    _toolsRuntimeBound = true;
    runtime.attach(database);

    // 状态一变就更新托盘；到点则弹系统通知。
    // 这两条连接挂在这里而不是工具页里：页面可能一直没被打开，
    // 但最小化到托盘时提醒照样得弹出来。
    connect(&runtime, &patina::ui::ToolsRuntime::changed, this, &MainWindow::refreshTrayProgress);
    connect(&runtime, &patina::ui::ToolsRuntime::alertRaised, this,
            [this](const patina::ui::ToolAlert& alert) {
                if (_trayIcon == nullptr) {
                    return;
                }
                _trayIcon->showMessage(alert.title, alert.body, QSystemTrayIcon::Information,
                                       kTrayMessageMsec);
            });

    // 活动提醒规则（引擎侧触发）也走托盘系统通知。
    if (_tracking != nullptr) {
        connect(_tracking, &patina::app::TrackingService::activityReminderRaised, this,
                [this](const QString& title, const QString& body) {
                    if (_trayIcon == nullptr) {
                        return;
                    }
                    _trayIcon->showMessage(title, body, QSystemTrayIcon::Information,
                                           kTrayMessageMsec);
                });
    }

    refreshTrayProgress();
}

void MainWindow::prepareTrayStart()
{
    // 自启启动时不显示窗口，也不建页面（托盘常驻期间只有托盘与引擎）。
    // 工具状态活在页面之外，绑定与提醒连接必须照做，否则重启恢复不会跑。
    initToolsRuntime();

    // 构造窗口时 Ela 骨架（导航栏、标题栏、堆叠容器）和字体已经在工作集里了。
    // 这条路径没有 hideToTray 兜底，启动完直接还一次，托盘常驻才是真的低。
    patina::platform::trimWorkingSet();
}

void MainWindow::addSettingsPage()
{
    if (_settingsHost != nullptr) {
        return;
    }
    _settingsHost = addPageShell(tr("设置"), ElaIconType::Gear, &_settingsPageKey);
}

void MainWindow::createSettingsContent()
{
    if (_settingsHost == nullptr || _settingsPage != nullptr) {
        return;
    }
    patina::data::Database* database = _tracking != nullptr ? _tracking->database() : nullptr;

    _settingsPage = new patina::ui::SettingsPage(database, _settingsHost);
    _settingsHost->layout()->addWidget(_settingsPage);

    // 开机自启：界面只发信号，注册表读写留在这个层（platform/ 由 app/ 调用）。
    connect(_settingsPage, &patina::ui::SettingsPage::autostartRequested, this,
            &MainWindow::applyAutostart);
    connect(_settingsPage, &patina::ui::SettingsPage::databaseReplaced, this,
            &MainWindow::reloadPagesAfterDataChange);

    // —— 三面板信号接线 ——
    connect(_settingsPage, &patina::ui::SettingsPage::trackingPauseChanged, this,
            &MainWindow::applyTrackingPause);
    connect(_settingsPage, &patina::ui::SettingsPage::titleRecordingChanged, this,
            &MainWindow::applyTitleRecording);
    connect(_settingsPage, &patina::ui::SettingsPage::themeModePicked, this,
            &MainWindow::applyThemeMode);
    connect(_settingsPage, &patina::ui::SettingsPage::trayIconVisibleChanged, this,
            &MainWindow::applyTrayIconVisible);
    connect(_settingsPage, &patina::ui::SettingsPage::closeBehaviorChanged, this,
            &MainWindow::applyCloseBehavior);
    connect(_settingsPage, &patina::ui::SettingsPage::minimizeBehaviorChanged, this,
            &MainWindow::applyMinimizeBehavior);

    // 壁纸：开关与来源即时生效，不走三面板的保存条。
    connect(_settingsPage, &patina::ui::SettingsPage::wallpaperSettingsChanged, this,
            &MainWindow::applyWallpaperSettings);
    connect(_settingsPage, &patina::ui::SettingsPage::wallpaperRefreshRequested, this,
            &MainWindow::refreshWallpaper);
    connect(_settingsPage, &patina::ui::SettingsPage::wallpaperOpacityChanged, this,
            &MainWindow::applyWallpaperOpacity);

    // 首次进来就把注册表的真实状态读出来，而不是假设「没注册」。
    _settingsPage->setAutostartState(patina::platform::Autostart::isEnabled());

    // 诊断面板的取数来源。Win32 读数只在这里出现（ui/ 不 include platform/），
    // 图表缓存与看门狗时间戳也一并在这里汇齐；面板只在用户点刷新时回调。
    _settingsPage->setDiagnosticsSource([this]() {
        patina::ui::SettingsPage::DiagnosticsSnapshot snapshot;
        const patina::platform::ProcessResources resources =
            patina::platform::currentProcessResources();
        snapshot.processValid = resources.valid;
        snapshot.workingSetBytes = resources.workingSetBytes;
        snapshot.privateBytes = resources.privateBytes;
        snapshot.handleCount = resources.handleCount;
        if (_dataPage != nullptr) {
            snapshot.chartCacheBytes = _dataPage->cachedBytes();
        }
        if (_tracking != nullptr) {
            const QString path = _tracking->databasePath();
            if (!path.isEmpty() && path != QStringLiteral(":memory:")) {
                snapshot.databaseFileBytes = QFileInfo(path).size()
                    + QFileInfo(path + QStringLiteral("-wal")).size();
            }
            const auto heartbeat = _tracking->lastHeartbeatMs();
            const auto sample = _tracking->lastSuccessfulSampleMs();
            const auto seal = _tracking->lastWatchdogSealMs();
            snapshot.healthValid = heartbeat.has_value();
            snapshot.lastHeartbeatMs = heartbeat.value_or(0);
            snapshot.lastSuccessfulSampleMs = sample.value_or(0);
            snapshot.lastWatchdogSealMs = seal.value_or(0);
        }
        return snapshot;
    });

    // 常驻/主题的运行时镜像从库初始化（与设置页 loadThreePanelSettings 同键）。
    if (_tracking != nullptr && _tracking->database() != nullptr
        && _tracking->database()->isOpen()) {
        patina::data::SettingsRepository repository(*_tracking->database());
        const auto readBool = [&repository](const char* key, bool defaultValue) {
            const auto result = repository.value(QString::fromLatin1(key));
            return (result.ok() && result.value.has_value())
                ? result.value->trimmed() == QStringLiteral("1")
                : defaultValue;
        };
        const auto readText = [&repository](const char* key, const QString& defaultValue) {
            const auto result = repository.value(QString::fromLatin1(key));
            return (result.ok() && result.value.has_value()
                    && !result.value->trimmed().isEmpty())
                ? result.value->trimmed()
                : defaultValue;
        };
        _trayIconVisible = readBool("show_tray_icon", true);
        _closeToTray =
            readText("close_behavior", QStringLiteral("tray")) == QStringLiteral("tray");
        _minimizeToTray =
            readText("minimize_behavior", QStringLiteral("taskbar")) == QStringLiteral("tray");
        applyTrayIconVisible(_trayIconVisible);
        applyCloseBehavior(_closeToTray);
        applyMinimizeBehavior(_minimizeToTray);
        const QString themeMode = readText("theme_mode", QStringLiteral("system"));
        // 启动时把主题也摆到位（设置页面板的选中态由它自己从库装）。
        applyThemeMode(themeMode);

        // 壁纸同样从库恢复：设置了就贴图，没设就保持素窗口。
        _wallpaperSource =
            readText("wallpaper_source", QStringLiteral("loliapi")) == QStringLiteral("uapis")
                ? QStringLiteral("uapis")
                : QStringLiteral("loliapi");
        _wallpaperEnabled = readBool("wallpaper_enabled", false);
        // 透明度读不到或写坏了都回 100%：宁可给原图，也不把窗口糊成一片。
        bool opacityOk = false;
        const int opacity = readText("wallpaper_opacity", QString()).toInt(&opacityOk);
        _wallpaperOpacity = opacityOk
            ? std::clamp(opacity, kWallpaperOpacityMin, kWallpaperOpacityFull)
            : kWallpaperOpacityDefault;
        if (_wallpaperEnabled) {
            applyWallpaperToWindow();
        }
    }
}

void MainWindow::addAboutPage()
{
    if (_aboutHost != nullptr) {
        return;
    }
    _aboutHost = addPageShell(tr("关于"), ElaIconType::CircleInfo, &_aboutPageKey);
}

void MainWindow::createAboutContent()
{
    if (_aboutHost == nullptr || _aboutPage != nullptr) {
        return;
    }
    _aboutPage = new patina::ui::AboutPage(_aboutHost);
    _aboutHost->layout()->addWidget(_aboutPage);
}

// ---------------- 三面板消费端 ----------------

void MainWindow::applyTrackingPause(bool paused)
{
    if (_tracking == nullptr) {
        return;
    }
    // 库已由调用方写好；setPaused 会封当前段并重写同一键（幂等），
    // 传 false 时恢复采样。失败时引擎状态没变，菜单摆回原样。
    QString error;
    if (!_tracking->setPaused(paused, &error)) {
        qWarning("[tracking] 同步追踪暂停状态失败：%s", qUtf8Printable(error));
        syncTrayMenuState();
        return;
    }
    // 菜单、图标、设置页都由 initTray 里那条 trackingPausedChanged 负责回灌。
}

void MainWindow::applyTitleRecording(bool enabled)
{
    if (_tracking == nullptr) {
        return;
    }
    // 库已写好；再走一次服务是为了立刻更新读缓存并（关闭时）清掉当前段的
    // 标题，否则要等缓存 TTL 过期才生效。
    QString error;
    if (!_tracking->setTitleRecordingEnabled(enabled, &error)) {
        qWarning("[tracking] 同步标题录制开关失败：%s", qUtf8Printable(error));
        syncTrayMenuState();
        return;
    }
    // 菜单与设置页由 titleRecordingChanged 回灌。
}

void MainWindow::applyThemeMode(const QString& mode)
{
    _themeModeRuntime = mode;
    // 三值 → ElaTheme 两档。「跟随系统」由 styleHints 的 colorScheme 决定，
    // 并挂在 colorSchemeChanged 上持续跟随（initTray 里已有同款监听换托盘色）。
    const bool dark = mode == QStringLiteral("dark")
        || (mode == QStringLiteral("system")
            && QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
    eTheme->setThemeMode(dark ? ElaThemeType::ThemeMode::Dark : ElaThemeType::ThemeMode::Light);

    // 半透明壁纸是「按窗口底色合成」出来的，底色随主题变，切主题就得重合成。
    // 100% 时不走这里（合成结果与原图相同，重贴没有意义）。
    if (_wallpaperEnabled && _wallpaperOpacity < kWallpaperOpacityFull && isVisible()) {
        applyWallpaperToWindow();
    }

    // 系统主题联动只挂一次：重复 connect 会在多次切到 system 时叠加。
    if (_themeSystemConnected) {
        return;
    }
    _themeSystemConnected = true;
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
            [this](Qt::ColorScheme scheme) {
                if (_themeModeRuntime != QStringLiteral("system")) {
                    return;
                }
                eTheme->setThemeMode(scheme == Qt::ColorScheme::Dark
                                         ? ElaThemeType::ThemeMode::Dark
                                         : ElaThemeType::ThemeMode::Light);
                if (_wallpaperEnabled && _wallpaperOpacity < kWallpaperOpacityFull
                    && isVisible()) {
                    applyWallpaperToWindow();
                }
            });
}

void MainWindow::applyTrayIconVisible(bool visible)
{
    _trayIconVisible = visible;
    if (_trayIcon != nullptr) {
        if (visible) {
            _trayIcon->show();
        } else {
            _trayIcon->hide();
        }
    }
}

void MainWindow::applyCloseBehavior(bool closeToTray)
{
    _closeToTray = closeToTray;
}

void MainWindow::applyMinimizeBehavior(bool minimizeToTray)
{
    _minimizeToTray = minimizeToTray;
}

// ---------------- 壁纸 ----------------

namespace
{

// 两个公开接口站，返回的都是随机横图。只下图片本身，不带任何参数上报。
const char* const kWallpaperSourceLoliApi = "loliapi";
const char* const kWallpaperSourceUapis = "uapis";

QString wallpaperUrlForSource(const QString& source)
{
    if (source == QLatin1String(kWallpaperSourceUapis)) {
        return QStringLiteral("https://uapis.cn/api/v1/random/image?category=acg&type=pc");
    }
    if (source == QLatin1String(kWallpaperSourceLoliApi)) {
        return QStringLiteral("https://www.loliapi.com/acg/pc/");
    }
    return QString();
}

// 按文件头判断格式：接口站有时给 JPEG，有时给 PNG/WebP，
// 只信 Content-Type 会把 webp 存成 .jpg，QImage 那边就是空图。
QString wallpaperExtensionFromBytes(const QByteArray& data, const QString& contentType)
{
    if (data.startsWith("\x89PNG\r\n\x1a\n")) {
        return QStringLiteral("png");
    }
    if (data.startsWith("\xFF\xD8\xFF")) {
        return QStringLiteral("jpg");
    }
    if (data.size() >= 12 && data.startsWith("RIFF") && data.mid(8, 4) == "WEBP") {
        return QStringLiteral("webp");
    }
    if (contentType.contains(QLatin1String("png"))) {
        return QStringLiteral("png");
    }
    if (contentType.contains(QLatin1String("webp"))) {
        return QStringLiteral("webp");
    }
    return QStringLiteral("jpg");
}

} // namespace

void MainWindow::applyWallpaperSettings(bool enabled, const QString& source)
{
    _wallpaperEnabled = enabled;
    if (!source.isEmpty()) {
        _wallpaperSource = source;
    }

    if (_tracking != nullptr && _tracking->database() != nullptr
        && _tracking->database()->isOpen()) {
        patina::data::SettingsRepository repository(*_tracking->database());
        repository.setValue(QStringLiteral("wallpaper_enabled"),
                            enabled ? QStringLiteral("1") : QStringLiteral("0"));
        repository.setValue(QStringLiteral("wallpaper_source"), _wallpaperSource);
    }

    if (!enabled) {
        clearWallpaper();
        if (_settingsPage != nullptr) {
            _settingsPage->setWallpaperState(false, _wallpaperSource, QString(), false);
        }
        return;
    }

    // 开着但盘上还没有图（首次打开开关就走这里）：去下一张。
    if (patina::ui::SettingsConfig::wallpaperFilePath().isEmpty()) {
        refreshWallpaper(_wallpaperSource);
        return;
    }
    applyWallpaperToWindow();
    if (_settingsPage != nullptr) {
        _settingsPage->setWallpaperState(true, _wallpaperSource, QString(), false);
    }
}

void MainWindow::refreshWallpaper(const QString& source)
{
    if (_wallpaperLoading) {
        return;
    }
    const QString url = wallpaperUrlForSource(source);
    if (url.isEmpty()) {
        if (_settingsPage != nullptr) {
            _settingsPage->setWallpaperState(_wallpaperEnabled, source,
                                             tr("未知的壁纸来源"), true);
        }
        return;
    }
    _wallpaperLoading = true;
    if (_settingsPage != nullptr) {
        _settingsPage->setWallpaperState(_wallpaperEnabled, source, tr("正在下载…"), false, true);
    }
    const std::shared_ptr<std::atomic_bool> alive = _wallpaperAlive;
    QPointer<MainWindow> self(this);

    // 网络最长 25 秒超时：绝不能在 UI 线程上等，否则整个窗口在下载期间僵死。
    // 线程 detach：结果排队回主线程，窗口没了就靠 alive 直接丢弃。
    std::thread([alive, self, url, source]() {
        const patina::platform::WallpaperResponse response =
            patina::platform::fetchWallpaper(url);
        if (!alive->load()) {
            return;
        }
        // 用 qApp 当投递目标：它在窗口之后才析构，工作线程不必去碰 QPointer。
        QMetaObject::invokeMethod(
            qApp,
            [alive, self, response, source]() {
                if (!alive->load() || self.isNull()) {
                    return;
                }
                MainWindow* window = self.data();
                window->_wallpaperLoading = false;
                if (!response.ok) {
                    if (window->_settingsPage != nullptr) {
                        window->_settingsPage->setWallpaperState(
                            window->_wallpaperEnabled, window->_wallpaperSource,
                            tr("换图失败：%1").arg(response.error), true);
                    }
                    return;
                }

                // 先落盘再贴图：贴图失败时盘上那份还能下次启动复用。
                const QString directory = patina::ui::SettingsConfig::wallpaperDirectory();
                if (!directory.isEmpty()) {
                    QDir().mkpath(directory);
                    const QString extension =
                        wallpaperExtensionFromBytes(response.data, response.contentType);
                    const QString target =
                        directory + QStringLiteral("/wallpaper.") + extension;
                    // 「不堆积」：写新文件前把目录里其它 wallpaper.* 删掉。
                    const QStringList stale = QDir(directory).entryList(
                        {QStringLiteral("wallpaper.*")}, QDir::Files);
                    for (const QString& name : stale) {
                        const QString path = directory + QLatin1Char('/') + name;
                        if (path != target) {
                            QFile::remove(path);
                        }
                    }
                    QFile file(target);
                    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                        if (window->_settingsPage != nullptr) {
                            window->_settingsPage->setWallpaperState(
                                window->_wallpaperEnabled, source,
                                tr("壁纸保存失败：%1").arg(file.errorString()), true);
                        }
                        return;
                    }
                    file.write(response.data);
                    file.close();
                }

                // 下载期间用户可能已经关了壁纸或把窗口收进托盘：那时位图
                // 刚被 clearWallpaper 放掉，这里不能再贴回去。文件已经落盘，
                // 回到前台 / 重新打开开关时会从盘上重新读。
                // 盘上换了新图，半透明用的原图缓存也跟着作废，否则还是旧图。
                window->_wallpaperOriginal = QPixmap();
                if (window->_wallpaperEnabled && window->isVisible()) {
                    window->applyWallpaperToWindow();
                }
                if (window->_settingsPage != nullptr) {
                    window->_settingsPage->setWallpaperState(
                        window->_wallpaperEnabled, source,
                        window->_wallpaperEnabled ? tr("已更新") : QString(), false);
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void MainWindow::applyWallpaperToWindow()
{
    const QString path = patina::ui::SettingsConfig::wallpaperFilePath();
    if (path.isEmpty()) {
        // 文件不在：按「没壁纸」处理，别让窗口停在空 Pixmap 分支。
        clearWallpaper();
        return;
    }

    // 100% 时不缓存原图：直接用文件解出来的那张，省一份 8MB 的常驻位图。
    // 只有调到半透明才留着原图，免得每次拖滑块都重解一遍 JPEG。
    QPixmap pixmap;
    if (_wallpaperOpacity >= kWallpaperOpacityFull) {
        _wallpaperOriginal = QPixmap();
        pixmap = QPixmap(path);
    } else {
        if (_wallpaperOriginal.isNull()) {
            _wallpaperOriginal = QPixmap(path);
        }
        pixmap = _wallpaperOriginal;
    }
    if (pixmap.isNull()) {
        clearWallpaper();
        return;
    }

    if (_wallpaperOpacity < kWallpaperOpacityFull) {
        // Ela 只是把整张图直接铺到窗口上，带 alpha 的图会和没绘制的区域混出脏色；
        // 先按当前主题的窗口底色合成成一张不透明图，降透明度才是可预期的效果。
        // 只按当前主题合成一张（两档共用一个 pixmap 句柄），切主题时由
        // applyThemeMode 重新走这里；两档各存一张会让常驻位图翻倍。
        const QColor base = eTheme->getThemeColor(eTheme->getThemeMode(),
                                                  ElaThemeType::ThemeColor::WindowBase);
        QPixmap composited(pixmap.size());
        composited.fill(base);
        QPainter painter(&composited);
        painter.setOpacity(_wallpaperOpacity / 100.0);
        painter.drawPixmap(0, 0, pixmap);
        painter.end();
        pixmap = composited;
    }

    // 顺序要紧：Ela 的 Pixmap 分支会直接解引用 pixmap 并算宽高，
    // 空图会走到除零；所以先塞非空图，再切模式。
    setWindowPixmap(ElaThemeType::ThemeMode::Light, pixmap);
    setWindowPixmap(ElaThemeType::ThemeMode::Dark, pixmap);
    setIsCentralStackedWidgetTransparent(true);
    setWindowPaintMode(ElaWindowType::PaintMode::Pixmap);
}

void MainWindow::applyWallpaperOpacity(int percent)
{
    const int clamped = std::clamp(percent, kWallpaperOpacityMin, kWallpaperOpacityFull);
    if (_wallpaperOpacity == clamped) {
        return;
    }
    _wallpaperOpacity = clamped;

    if (_tracking != nullptr && _tracking->database() != nullptr
        && _tracking->database()->isOpen()) {
        patina::data::SettingsRepository repository(*_tracking->database());
        repository.setValue(QStringLiteral("wallpaper_opacity"), QString::number(clamped));
    }

    // 只重算贴图，不重新下载；收在托盘里的时候什么都不做，回前台再贴。
    if (_wallpaperEnabled && isVisible()) {
        applyWallpaperToWindow();
    }
}

void MainWindow::clearWallpaper()
{
    // 反过来的顺序同样要紧：先切回 Normal，paintEvent 就不再碰那两个 pixmap。
    setWindowPaintMode(ElaWindowType::PaintMode::Normal);
    setIsCentralStackedWidgetTransparent(false);
    setWindowPixmap(ElaThemeType::ThemeMode::Light, QPixmap());
    setWindowPixmap(ElaThemeType::ThemeMode::Dark, QPixmap());
    // 半透明用的原图缓存也跟着放掉：托盘态不该留这份位图。
    _wallpaperOriginal = QPixmap();
}

void MainWindow::applyAutostart(bool enabled)
{
    const bool ok = patina::platform::Autostart::setEnabled(enabled);
    // 无论成败都以注册表的实际状态回灌：写失败了开关不能停在用户拨到的位置。
    if (_settingsPage != nullptr) {
        _settingsPage->setAutostartState(patina::platform::Autostart::isEnabled());
    }
    if (!ok) {
        qWarning("[settings] 开机自启设置失败：%s",
                 qUtf8Printable(patina::platform::Autostart::lastError()));
    }
}

void MainWindow::reloadPagesAfterDataChange()
{
    // 整库换过之后，各页面手里缓存的应用名、分类覆盖项、活动段全都过期了。
    // 只刷新正在显示的那一页：借一次 setActive 往返触发重查。
    // 其余页面没显示，下次切过去时 setActive(true) 会自己重查。
    const auto refreshIfActive = [this](const QString& pageKey, auto* page) {
        if (page == nullptr || pageKey.isEmpty() || pageKey != _activePageKey) {
            return;
        }
        page->setActive(false);
        page->setActive(true);
    };
    refreshIfActive(_dashboardPageKey, _dashboardPage);
    refreshIfActive(_historyPageKey, _historyPage);
    refreshIfActive(_dataPageKey, _dataPage);
    refreshIfActive(_classificationPageKey, _classificationPage);

    // 工具状态存在 settings 表里，恢复备份会把它整份换掉：断一次再接上，重新从库里读。
    patina::ui::ToolsRuntime& runtime = patina::ui::ToolsRuntime::instance();
    runtime.attach(nullptr);
    runtime.attach(_tracking != nullptr ? _tracking->database() : nullptr);
    refreshTrayProgress();
}

void MainWindow::addPlaceholderPage(const QString& title, ElaIconType::IconName icon)
{
    auto* page = new ElaScrollPage();

    // addCentralWidget 用 windowTitle 生成面包屑，页面标题在这里设置。
    auto* container = new QWidget(page);
    container->setWindowTitle(title);

    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 12, 24, 12);
    layout->setSpacing(8);

    layout->addWidget(new ElaText(title, 20, container));
    layout->addWidget(new ElaText(tr("该页面尚未实现。"), 14, container));
    layout->addStretch();

    page->addCentralWidget(container);

    if (addPageNode(title, page, icon) == ElaNavigationType::Success) {
        // pageKey 由导航栏内部生成，只能从页面属性反查。
        if (_firstPageKey.isEmpty()) {
            _firstPageKey = page->property("ElaPageKey").toString();
        }
    } else {
        delete page;
    }
}

QIcon MainWindow::makeTrayIcon() const
{
    // 四个变体随深浅色任务栏切换；暂停态（工具计时器暂停 或 追踪被暂停）用 paused 版本。
    const QString suffix = _darkTaskbar ? QStringLiteral("dark") : QStringLiteral("light");
    const patina::ui::ToolsRuntime& runtime = patina::ui::ToolsRuntime::instance();
    const qreal progress = runtime.trayProgress();
    const bool paused = runtime.timer().status == patina::ui::TimerStatus::Paused
        || runtime.pomodoro().status == patina::ui::PomodoroStatus::Paused
        || (_tracking != nullptr && _tracking->isPaused());
    const QString state = paused ? QStringLiteral("paused") : QStringLiteral("active");

    QPixmap pixmap(QStringLiteral(":/tray/on-%1-%2.png").arg(suffix, state));
    if (pixmap.isNull()) {
        pixmap = QPixmap(QStringLiteral(":/tray/on-%1-active.png").arg(suffix));
    }
    if (progress < 0.0 || pixmap.isNull()) {
        return QIcon(pixmap);
    }

    // 有进度时在图标外圈画一段弧：托盘图标就那么大，环是最省地方的表达。
    // 在缩放后的 pixmap 上画，边框跟着 DPI 走，不会在小图上糊成一团。
    pixmap = pixmap.scaled(kTrayIconSize, kTrayIconSize, Qt::KeepAspectRatio,
                           Qt::SmoothTransformation);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const qreal side = std::min(pixmap.width(), pixmap.height());
    const QRectF ringRect(kTrayArcMargin, kTrayArcMargin, side - kTrayArcMargin * 2.0,
                          side - kTrayArcMargin * 2.0);

    QPen trackPen(QColor(128, 128, 128, 90), kTrayArcWidth);
    trackPen.setCapStyle(Qt::FlatCap);
    painter.setPen(trackPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(ringRect);

    QColor accent(79, 95, 215);
    if (_darkTaskbar) {
        accent = QColor(124, 138, 240);
    }
    if (paused) {
        accent.setAlphaF(0.45);
    }
    QPen progressPen(accent, kTrayArcWidth);
    progressPen.setCapStyle(Qt::RoundCap);
    painter.setPen(progressPen);
    const int spanDegrees = static_cast<int>(std::lround(-(progress * 360.0)));
    painter.drawArc(ringRect, 90 * 16, spanDegrees * 16);
    painter.end();

    return QIcon(pixmap);
}

bool MainWindow::isDarkTaskbar() const
{
    // 任务栏明暗跟随系统主题，Qt 6.5+ 直接暴露，不用自己读注册表。
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

void MainWindow::refreshTrayIcon()
{
    _darkTaskbar = isDarkTaskbar();
    if (_trayIcon) {
        _trayIcon->setIcon(makeTrayIcon());
    }
}

void MainWindow::refreshTrayProgress()
{
    if (_trayIcon == nullptr) {
        return;
    }
    // 图标（进度环）与悬停文字一起换：工具状态一变就走到这里。
    _trayIcon->setIcon(makeTrayIcon());

    const patina::ui::ToolsRuntime& runtime = patina::ui::ToolsRuntime::instance();
    const QString text = runtime.trayText();
    _trayIcon->setToolTip(text.isEmpty() ? tr("UseTrack") : text);
}

void MainWindow::initTray()
{
    _trayMenu = new ElaMenu(this);

    // 顺序：打开主界面 → 暂停追踪 → 记录标题 → 退出。
    // 两项固定项不做 checkable：靠文案与图标表达当前态就够了，勾选标记会和 Ela 的图标列打架。
    QAction* showAction = _trayMenu->addElaIconAction(ElaIconType::WindowRestore,
                                                      tr("显示主窗口"));
    _trayPauseAction = _trayMenu->addElaIconAction(ElaIconType::CirclePause, tr("暂停追踪"));
    _trayTitleAction = _trayMenu->addElaIconAction(ElaIconType::Eye, tr("屏蔽标题"));
    QAction* quitAction = _trayMenu->addElaIconAction(ElaIconType::PowerOff, tr("退出"));

    connect(showAction, &QAction::triggered, this, &MainWindow::showFromTray);
    // 两个 toggle 都按「当前态取反」走，与设置页保存后走同一对落地函数。
    connect(_trayPauseAction, &QAction::triggered, this, [this]() {
        applyTrackingPause(!(_tracking != nullptr && _tracking->isPaused()));
    });
    connect(_trayTitleAction, &QAction::triggered, this, [this]() {
        applyTitleRecording(!(_tracking == nullptr || _tracking->isTitleRecordingEnabled()));
    });
    connect(quitAction, &QAction::triggered, this, &MainWindow::quitApplication);

    _darkTaskbar = isDarkTaskbar();
    _trayIcon = new QSystemTrayIcon(this);
    _trayIcon->setIcon(makeTrayIcon());
    _trayIcon->setToolTip(tr("UseTrack"));
    _trayIcon->setContextMenu(_trayMenu);
    connect(_trayIcon, &QSystemTrayIcon::activated, this, &MainWindow::onTrayActivated);
    _trayIcon->show();

    // 菜单每次弹出前按引擎当前状态刷新：托盘可能是唯一的入口（窗口关着也常驻）。
    connect(_trayMenu, &QMenu::aboutToShow, this, &MainWindow::syncTrayMenuState);
    syncTrayMenuState();

    // 引擎侧变化（设置页保存、托盘切换、热键、电源事件）回灌菜单与设置页，
    // 双向同步的另一半。设置页那两条带幂等守卫，自己拨的那次不会重复触发。
    if (_tracking != nullptr) {
        connect(_tracking, &patina::app::TrackingService::trackingPausedChanged, this,
                [this](bool paused) {
                    syncTrayMenuState();
                    refreshTrayIcon();
                    if (_settingsPage != nullptr) {
                        _settingsPage->setTrackingPaused(paused);
                    }
                });
        connect(_tracking, &patina::app::TrackingService::titleRecordingChanged, this,
                [this](bool enabled) {
                    syncTrayMenuState();
                    if (_settingsPage != nullptr) {
                        _settingsPage->setTitleRecordingEnabled(enabled);
                    }
                });
    }

    // 系统切换深浅色时同步换图标。
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
            this, [this](Qt::ColorScheme) { refreshTrayIcon(); });
}

void MainWindow::syncTrayMenuState()
{
    if (_trayPauseAction == nullptr || _trayTitleAction == nullptr) {
        return;
    }

    const bool paused = _tracking != nullptr && _tracking->isPaused();
    // 文案与图标都按「当前态」算好再和现有值比：菜单每次弹出前都会调这里，
    // 状态没变时不该白标脏一次菜单（update() 会触发重排）。
    const QString pauseText = paused ? tr("恢复追踪") : tr("暂停追踪");
    const QChar pauseIcon = QChar(static_cast<ushort>(paused ? ElaIconType::CirclePlay
                                                             : ElaIconType::CirclePause));
    const bool pauseChanged = _trayPauseAction->text() != pauseText
        || _trayPauseAction->property("ElaIconType").toChar() != pauseIcon;
    if (pauseChanged) {
        _trayPauseAction->setProperty("ElaIconType", pauseIcon);
        _trayPauseAction->setText(pauseText);
    }

    const bool enabled = _tracking == nullptr || _tracking->isTitleRecordingEnabled();
    // 按「点下去会发生什么」命名——开着时给「屏蔽标题」。
    const QString titleText = enabled ? tr("屏蔽标题") : tr("记录标题");
    const QChar titleIcon = QChar(static_cast<ushort>(enabled ? ElaIconType::Eye
                                                              : ElaIconType::EyeSlash));
    const bool titleChanged = _trayTitleAction->text() != titleText
        || _trayTitleAction->property("ElaIconType").toChar() != titleIcon;
    if (titleChanged) {
        _trayTitleAction->setProperty("ElaIconType", titleIcon);
        _trayTitleAction->setText(titleText);
    }

    if ((pauseChanged || titleChanged) && _trayMenu != nullptr) {
        _trayMenu->update();
    }
}

void MainWindow::showEvent(QShowEvent* event)
{
    ElaWindow::showEvent(event);

    // 页面懒加载：主窗口首次打开时才创建页面，托盘常驻阶段不占内存。
    if (!_contentInitialized) {
        _contentInitialized = true;
        initContent();
        // 导航栏不会自动选中首项，这里手动切到第一个页面。
        if (!_firstPageKey.isEmpty()) {
            navigation(_firstPageKey);
            // navigation() 不保证回发 navigationNodeClicked，这里显式激活首屏，
            // 否则今日概览页会一直停在「正在读取数据…」。
            onPageChanged(_firstPageKey);
        }
    }
}

void MainWindow::hideToTray()
{
    // 只 hide() 的话页面控件、样式、字体、图表位图还全在内存里，
    // 实测托盘常驻仍有 60MB+。这里把各页的内容本体拆掉（外壳与导航节点留着，
    // 摘节点会让 Ela 内部状态悬空），回前台再按当前页重建。
    // 不改 _activePageIndex：回到前台时还要靠它恢复原来那一页。
    if (_contentInitialized && !_pagesDestroyed) {
        destroyPages();
    }
    // 控件删了，两层图标缓存里还挂着位图（各 256 项封顶）。托盘态不进主窗，
    // 这些图短时间内用不上，先清掉；回前台重建页面时按需重新取一次。
    patina::shared::clearAppIconCache();

    // 壁纸是一整张 1920 级位图，托盘态同样先放掉；回前台按盘上那份重新贴。
    // 顺序由 clearWallpaper 保证（先切 Normal 再清 pixmap）。
    if (_wallpaperEnabled) {
        clearWallpaper();
    }

    hide();

    // delete 只把页标成可复用，物理页仍留在进程工作集里，读数不会降。
    // 拆完页面树显式还一次，把托盘态的常驻压到 20MB 以内。
    patina::platform::trimWorkingSet();
}

void MainWindow::destroyPages()
{
    _pagesDestroyed = true;

    // 页面自身的 hideEvent/析构会停表并清缓存；这里先显式退到非活动态，
    // 免得页面在析构前又触发一次查库。
    setPagesActive(false);

    // 只删页面本体，不碰外壳与导航节点：摘导航节点会让 Ela 内部缓存的
    // 选中节点悬空（崩在 _resetNodeSelected → getRow()），导航目标索引也会
    // 停在旧值上导致重建后切页被拦下（白屏）。外壳里剩下的只是空的宿主 widget。
    // 删之前先从宿主布局里摘掉，免得布局里留下失效的 item。
    // 泛型 lambda：各页面指针推导出的类型不同（DashboardPage* 等），
    // 收成 QWidget** 会丢派生类型，delete 后置空也写不回原变量。
    const auto dropPage = [](QWidget* host, auto** page) {
        if (*page == nullptr) {
            return;
        }
        if (host != nullptr && host->layout() != nullptr) {
            host->layout()->removeWidget(*page);
        }
        delete *page;
        *page = nullptr;
    };
    dropPage(_dashboardHost, &_dashboardPage);
    dropPage(_historyHost, &_historyPage);
    dropPage(_dataHost, &_dataPage);
    dropPage(_classificationHost, &_classificationPage);
    dropPage(_toolsHost, &_toolsPage);
    dropPage(_settingsHost, &_settingsPage);
    dropPage(_aboutHost, &_aboutPage);
}

void MainWindow::showFromTray()
{
    // 先把内容重建好再显示，否则会闪一帧空宿主。
    // rebuildPages 内部会切回原页并激活，下面不必再补一次。
    const bool rebuilt = _contentInitialized && _pagesDestroyed;
    if (rebuilt) {
        rebuildPages();
    }

    showNormal();
    raise();
    activateWindow();

    // 壁纸在收托盘时被放掉了，回前台从盘上那一份重新贴（不重新下载）。
    if (_wallpaperEnabled) {
        applyWallpaperToWindow();
    }

    // 没拆过（例如开机自启直接唤出）就只把当前页重新激活（重查一次库）。
    if (!rebuilt && _contentInitialized && !_activePageKey.isEmpty()) {
        onPageChanged(_activePageKey);
    }
}

void MainWindow::rebuildPages()
{
    _pagesDestroyed = false;

    // 外壳还在，内容按导航顺序逐页建回来；建完再激活原来那一页。
    // create*Content 各自带「已存在就返回」的守卫，重复调用安全。
    createDashboardContent();
    createHistoryContent();
    createDataContent();
    createClassificationContent();
    createToolsContent();
    createSettingsContent();
    createAboutContent();

    // 页面顺序固定，按销毁前的位置还原；越界就退回第一页。
    if (_activePageIndex < 0 || _activePageIndex >= _pageShells.size()) {
        _activePageIndex = 0;
    }
    if (_pageShells.isEmpty()) {
        return;
    }
    const QString pageKey = _pageShells.at(_activePageIndex)->property("ElaPageKey").toString();
    if (pageKey.isEmpty()) {
        return;
    }
    navigation(pageKey);
    // navigation() 不保证回发 navigationNodeClicked，这里显式激活。
    onPageChanged(pageKey);
}

void MainWindow::activateFromSecondInstance()
{
    showFromTray();
}

void MainWindow::quitApplication()
{
    _trayIcon->hide();
    QApplication::quit();
}

void MainWindow::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        showFromTray();
    }
}
