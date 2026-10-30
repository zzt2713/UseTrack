#include <QApplication>
#include <QColor>
#include <QMetaObject>
#include <QPalette>
#include <QPixmapCache>
#include <QStringList>
#include <QStyleHints>

#include "ElaTheme.h"
#include "ElaApplication.h"

#include "app/main_window.h"
#include "app/single_instance.h"
#include "app/tracking_service.h"
#include "data/database.h"
#include "data/settings_repository.h"
#include "ui/settings/settings_config.h"

namespace
{

// Ela 主题只负责它自己绘制的那些控件。QScrollArea 的视口、QLineEdit 和
// QComboBox 里由 Qt 原生绘制的部分（下拉文字、输入框文字、按钮文字）仍然按
// QApplication 的调色板取色。
//
// 不同步的话，深色主题下就会出现两种割裂：滚动区视口刷成白底（调色板 Base 是白的），
// 下拉框文字画成黑色压在深色底上。所以调色板必须跟着 Ela 主题一起切。
void applyPaletteForTheme(ElaThemeType::ThemeMode mode)
{
    const bool dark = mode == ElaThemeType::ThemeMode::Dark;

    QPalette palette;
    if (dark)
    {
        const QColor window(0x1C, 0x1F, 0x25);
        const QColor base(0x23, 0x27, 0x2E);
        const QColor text(0xE9, 0xEC, 0xF1);
        const QColor disabled(0x6B, 0x72, 0x7E);

        palette.setColor(QPalette::Window, window);
        palette.setColor(QPalette::WindowText, text);
        palette.setColor(QPalette::Base, base);
        palette.setColor(QPalette::AlternateBase, window);
        palette.setColor(QPalette::Text, text);
        palette.setColor(QPalette::Button, base);
        palette.setColor(QPalette::ButtonText, text);
        palette.setColor(QPalette::ToolTipBase, window);
        palette.setColor(QPalette::ToolTipText, text);
        palette.setColor(QPalette::PlaceholderText, disabled);
        palette.setColor(QPalette::Highlight, QColor(0x7C, 0x8A, 0xF0));
        palette.setColor(QPalette::HighlightedText, QColor(0x14, 0x17, 0x1C));
        palette.setColor(QPalette::Mid, QColor(0x3A, 0x40, 0x4A));
        palette.setColor(QPalette::Midlight, QColor(0x2B, 0x30, 0x38));
        palette.setColor(QPalette::Dark, QColor(0x11, 0x13, 0x17));

        palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
        palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    }
    else
    {
        const QColor window(0xF5, 0xF6, 0xF8);
        const QColor base(0xFF, 0xFF, 0xFF);
        const QColor text(0x1B, 0x1D, 0x22);
        const QColor disabled(0x8F, 0x98, 0xA8);

        palette.setColor(QPalette::Window, window);
        palette.setColor(QPalette::WindowText, text);
        palette.setColor(QPalette::Base, base);
        palette.setColor(QPalette::AlternateBase, window);
        palette.setColor(QPalette::Text, text);
        palette.setColor(QPalette::Button, base);
        palette.setColor(QPalette::ButtonText, text);
        palette.setColor(QPalette::ToolTipBase, base);
        palette.setColor(QPalette::ToolTipText, text);
        palette.setColor(QPalette::PlaceholderText, disabled);
        palette.setColor(QPalette::Highlight, QColor(0x4F, 0x5F, 0xD7));
        palette.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));
        palette.setColor(QPalette::Mid, QColor(0xD2, 0xD7, 0xE0));
        palette.setColor(QPalette::Midlight, QColor(0xE4, 0xE7, 0xEC));
        palette.setColor(QPalette::Dark, QColor(0xC6, 0xCB, 0xD4));

        palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
        palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    }

    QApplication::setPalette(palette);
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    QApplication::setApplicationName(QStringLiteral("UseTrack"));
    QApplication::setOrganizationName(QStringLiteral("UseTrack"));
    // 版本号来自 CMake 的 PROJECT_VERSION（关于页显示用）；未定义时留空，
    // 关于页会回落到「版本未知」。
#ifdef PATINA_VERSION
    QApplication::setApplicationVersion(QStringLiteral(PATINA_VERSION));
#endif

    QApplication::setQuitOnLastWindowClosed(false);

    // Qt 的全局位图缓存默认 10MB，本项目图标一律走 app_icon_cache 的
    // 256 项 LRU，这个全局池只会白占工作集，压低到 1MB。
    QPixmapCache::setCacheLimit(1024);

    // 单实例。MainWindow 还没构造，激活 handler 先记一个空指针，
    // 主窗口构造好后通过 setActivationTarget 补上；第二实例到来时走
    // QMetaObject::invokeMethod 投回 GUI 线程调 activateFromSecondInstance。
    MainWindow* windowForActivate = nullptr;
    QString instanceError;
    if (!patina::app::SingleInstanceLock::acquire(
            [&windowForActivate]() {
                if (windowForActivate != nullptr) {
                    QMetaObject::invokeMethod(windowForActivate,
                                              &MainWindow::activateFromSecondInstance,
                                              Qt::QueuedConnection);
                }
            },
            &instanceError)) {
        if (instanceError.isEmpty()) {
            qInfo("[single-instance] 已有实例在运行，已通知其激活主窗口，本进程退出");
        } else {
            qWarning("[single-instance] %s，本进程退出", qUtf8Printable(instanceError));
        }
        return 0;
    }
    if (!instanceError.isEmpty()) {
        // acquire 成功但留了 errorOut：单实例保护降级，应用照常启动。
        qWarning("[single-instance] %s", qUtf8Printable(instanceError));
    }

    eApp->init();

    // 调色板跟着 Ela 主题切：Ela 只管自己绘制的控件，剩下的原生绘制靠这份调色板。
    applyPaletteForTheme(eTheme->getThemeMode());
    QObject::connect(eTheme, &ElaTheme::themeModeChanged, &app, [](ElaThemeType::ThemeMode mode) {
        applyPaletteForTheme(mode);
    });

    patina::app::TrackingService tracking;
    const QString configuredPath = patina::ui::SettingsConfig::databasePath();
    if (!configuredPath.isEmpty()) {
        tracking.setDatabasePath(configuredPath);
    }
    QString trackingError;
    if (!tracking.start(&trackingError)) {
        qCritical("[tracking] 启动失败：%s", qUtf8Printable(trackingError));
    }
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     &tracking, &patina::app::TrackingService::stop);

    // 主题模式启动即生效（默认 system = 跟随系统深浅色）。
    // 库已开（tracking.start 成功时），读 theme_mode 摆好首帧主题；
    // 运行时改动由 MainWindow::applyThemeMode 接管（同键同值）。
    if (tracking.database() != nullptr && tracking.database()->isOpen()) {
        patina::data::SettingsRepository settings(*tracking.database());
        const auto themeMode = settings.value(QStringLiteral("theme_mode"));
        bool themeDark = QGuiApplication::styleHints()->colorScheme()
            == Qt::ColorScheme::Dark;
        if (themeMode.ok() && themeMode.value.has_value()) {
            const QString mode = themeMode.value->trimmed();
            if (mode == QStringLiteral("dark")) {
                themeDark = true;
            } else if (mode == QStringLiteral("light")) {
                themeDark = false;
            }
        }
        eTheme->setThemeMode(themeDark ? ElaThemeType::ThemeMode::Dark
                                       : ElaThemeType::ThemeMode::Light);
        applyPaletteForTheme(eTheme->getThemeMode());
    }

    MainWindow window(&tracking);
    windowForActivate = &window;

    // 启动最小化只作用于开机自启场景：手动启动恒弹窗，
    // 「启动时最小化」管的是登录自启别弹窗烦人。
    // --autostart 且设置了 start_minimized → 托盘启动；否则正常弹窗。
    const bool autostartLaunch = QApplication::arguments().contains(QStringLiteral("--autostart"));
    bool startInTray = autostartLaunch;
    if (autostartLaunch && tracking.database() != nullptr
        && tracking.database()->isOpen()) {
        patina::data::SettingsRepository settings(*tracking.database());
        const auto minimized = settings.value(QStringLiteral("start_minimized"));
        // 键缺失按默认 true（自启时不弹窗）。
        startInTray = !minimized.ok() || !minimized.value.has_value()
            || minimized.value->trimmed() == QStringLiteral("1");
    }
    if (startInTray) {
        window.prepareTrayStart();
    } else {
        window.show();
        window.moveToCenter();
    }

    const int exitCode = app.exec();
    patina::app::SingleInstanceLock::release();
    return exitCode;
}
