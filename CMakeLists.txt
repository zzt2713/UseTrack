cmake_minimum_required(VERSION 3.21)

project(usetrack-cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

if(MSVC)
    # 源码与执行字符集统一 UTF-8，界面中文字面量才不会乱码
    add_compile_options(/utf-8)
endif()

if(NOT CMAKE_PREFIX_PATH AND EXISTS "D:/Qt/6.8.3/msvc2022_64")
    set(CMAKE_PREFIX_PATH "D:/Qt/6.8.3/msvc2022_64")
endif()

find_package(Qt6 6.8.3 REQUIRED COMPONENTS Core Gui Widgets Sql)

set(ELA_ROOT "" CACHE PATH "ElaWidgetTools 安装根目录（含 include / lib / bin）")

set(_ela_root "")
foreach(_candidate IN ITEMS
        "${ELA_ROOT}"
        "D:/cppsoft/MsvcEla"
        "D:/study/code/ElaWidgetTools/Install/ElaWidgetTools")
    if(_candidate
            AND EXISTS "${_candidate}/include/ElaWindow.h"
            AND EXISTS "${_candidate}/lib/ElaWidgetTools.lib"
            AND EXISTS "${_candidate}/bin/ElaWidgetTools.dll")
        set(_ela_root "${_candidate}")
        break()
    endif()
endforeach()

if(NOT _ela_root)
    message(FATAL_ERROR "未找到 ElaWidgetTools 安装目录，请用 -DELA_ROOT=<路径> 指定")
endif()
message(STATUS "ElaWidgetTools: ${_ela_root}")

add_library(ElaWidgetTools SHARED IMPORTED)
set_target_properties(ElaWidgetTools PROPERTIES
    IMPORTED_IMPLIB "${_ela_root}/lib/ElaWidgetTools.lib"
    IMPORTED_LOCATION "${_ela_root}/bin/ElaWidgetTools.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${_ela_root}/include"
    INTERFACE_LINK_LIBRARIES "Qt6::Widgets;D3D11;DXGI"
)

# 全部产品代码合并进一个静态库，主程序独占链接它。
# 目录分层（platform / data / engine / app / ui）靠 coding-style §6 的 include 约定约束，
# 不再用「一目录一个库」的方式在链接期强制——那样每个库都会在 IDE 里占一个目标。
add_library(usetrack_core STATIC
    # platform/：收口全部 Win32 / WinRT 调用
    src/platform/win_handle.h
    src/platform/foreground_window.h
    src/platform/foreground_window.cpp
    src/platform/icon_extractor.h
    src/platform/icon_extractor.cpp
    src/platform/idle_time.h
    src/platform/idle_time.cpp
    src/platform/power_watcher.h
    src/platform/power_watcher.cpp
    src/platform/autostart.h
    src/platform/autostart.cpp
    src/platform/packaged_app.h
    src/platform/packaged_app.cpp
    src/platform/sustained_signals.h
    src/platform/sustained_signals.cpp
    src/platform/process_resources.h
    src/platform/process_resources.cpp
    src/platform/wallpaper_fetcher.h
    src/platform/wallpaper_fetcher.cpp

    # data/：独占 SQLite 访问，UI 与 engine 都只能经 repository 接口，不写 SQL
    src/data/types.h
    src/data/database.h
    src/data/database.cpp
    src/data/schema.h
    src/data/schema.cpp
    src/data/app_repository.h
    src/data/app_repository.cpp
    src/data/session_repository.h
    src/data/session_repository.cpp
    src/data/session_title_sample_repository.h
    src/data/session_title_sample_repository.cpp
    src/data/anonymous_activity_repository.h
    src/data/anonymous_activity_repository.cpp
    src/data/settings_repository.h
    src/data/settings_repository.cpp
    src/data/app_category.h
    src/data/app_category.cpp
    src/data/app_links.h
    src/data/app_links.cpp
    src/data/app_override.h
    src/data/app_override.cpp
    src/data/category_registry.h
    src/data/category_registry.cpp
    src/data/classification_repository.h
    src/data/classification_repository.cpp
    src/data/dashboard_read_model.h
    src/data/dashboard_read_model.cpp
    src/data/data_read_model.h
    src/data/data_read_model.cpp
    src/data/backup_service.h
    src/data/backup_service.cpp
    src/data/hourly_read_model.h
    src/data/hourly_read_model.cpp
    src/data/destination_read_model.h
    src/data/destination_read_model.cpp
    src/data/app_options_read_model.h
    src/data/app_options_read_model.cpp
    src/data/tool_models.h
    src/data/tool_models.cpp
    src/data/tools_repository.h
    src/data/tools_repository.cpp
    src/data/activity_reminder_checker.h
    src/data/activity_reminder_checker.cpp
    src/data/icon_cache_repository.h
    src/data/icon_cache_repository.cpp
    src/data/export_service.h
    src/data/export_service.cpp
    src/data/import_service.h
    src/data/import_service.cpp
    src/data/data_directory_migrator.h
    src/data/data_directory_migrator.cpp
    src/data/repositories.h

    # engine/：采样时序与 session 边界语义，不碰 Win32、不写 SQL
    src/engine/window_sample.h
    src/engine/window_identity.h
    src/engine/window_identity.cpp
    src/engine/tracking_policy.h
    src/engine/process_filter.h
    src/engine/process_filter.cpp
    src/engine/session_store.h
    src/engine/session_store.cpp
    src/engine/session_tracker.h
    src/engine/session_tracker.cpp
    src/engine/sustained_identity.h
    src/engine/sustained_identity.cpp
    src/engine/sustained_participation.h
    src/engine/sustained_participation.cpp
    src/engine/continuity.h
    src/engine/continuity.cpp
    src/engine/watchdog.h
    src/engine/watchdog.cpp
    src/engine/tracking_settings.h
    src/engine/tracking_settings.cpp

    # app/：采样 → engine 语义 → data 落库的编排
    src/app/sqlite_session_store.h
    src/app/sqlite_session_store.cpp
    src/app/tracking_service.h
    src/app/tracking_service.cpp
    src/app/single_instance.h
    src/app/single_instance.cpp

    # shared/：跨模块纯工具，不依赖 ui/、data/
    src/shared/app_icon_cache.h
    src/shared/app_icon_cache.cpp
    src/shared/theme_styles.h
    src/shared/theme_styles.cpp

    # ui/：只经 data/ 的读模型取数，不写 SQL、不 include platform/
    src/ui/shared/hourly_bars_widget.h
    src/ui/shared/hourly_bars_widget.cpp
    src/ui/shared/quick_classification_menu.h
    src/ui/shared/quick_classification_menu.cpp
    src/ui/destination/destination_detail_dialog.h
    src/ui/destination/destination_detail_dialog.cpp
    src/ui/dashboard/dashboard_page.h
    src/ui/dashboard/dashboard_page.cpp
    src/ui/dashboard/category_donut_widget.h
    src/ui/dashboard/category_donut_widget.cpp
    src/ui/dashboard/app_rank_list_widget.h
    src/ui/dashboard/app_rank_list_widget.cpp
    src/ui/history/history_page.h
    src/ui/history/history_page.cpp
    src/ui/history/history_timeline_widget.h
    src/ui/history/history_timeline_widget.cpp
    src/ui/history/history_timeline_zoom_dialog.h
    src/ui/history/history_timeline_zoom_dialog.cpp
    src/ui/history/history_feed_widget.h
    src/ui/history/history_feed_widget.cpp
    src/ui/history/history_day_band_widget.h
    src/ui/history/history_day_band_widget.cpp
    src/ui/data/data_page.h
    src/ui/data/data_page.cpp
    src/ui/data/trend_line_chart_widget.h
    src/ui/data/trend_line_chart_widget.cpp
    src/ui/data/activity_heatmap_widget.h
    src/ui/data/activity_heatmap_widget.cpp
    src/ui/classification/classification_page.h
    src/ui/classification/classification_page.cpp
    src/ui/classification/app_classification_card.h
    src/ui/classification/app_classification_card.cpp
    src/ui/tools/tools_types.h
    src/ui/tools/tools_types.cpp
    src/ui/tools/tools_runtime.h
    src/ui/tools/tools_runtime.cpp
    src/ui/tools/countdown_ring_widget.h
    src/ui/tools/countdown_ring_widget.cpp
    src/ui/tools/tools_page.h
    src/ui/tools/tools_page.cpp
    src/ui/settings/settings_config.h
    src/ui/settings/settings_config.cpp
    src/ui/settings/settings_page.h
    src/ui/settings/settings_page.cpp
    src/ui/about/about_page.h
    src/ui/about/about_page.cpp
)

target_include_directories(usetrack_core PUBLIC src)
target_link_libraries(usetrack_core PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Sql ElaWidgetTools
    # user32/gdi32/shell32/wtsapi32 是 Win32 采样与电源通知的系统导入库，MSVC 下必须显式链接。
    user32
    gdi32
    shell32
    wtsapi32
    # 5.1 媒体/音频探测：ole32 是 COM 基础（WASAPI + SMTC 的 WinRT 激活）；
    # version 用于 5.11 的显示名解析（GetFileVersionInfoW）。
    ole32
    version
    # 5.1 SMTC 的 WinRT 激活在 WindowsApp.lib 里（含 C++/WinRT 所需的
    # RoGetActivationFactory 等入口）。SDK 10.0.26100.0 自带。
    windowsapp
    # 壁纸下载走 WinHTTP（系统库，不引入 Qt Network，见 coding-style §2 与 §6）。
    winhttp
)

# C++/WinRT 投影头需要 SDK 的 cppwinrt 目录；SDK 10.0.26100.0 自带。
if(EXISTS "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/cppwinrt")
    target_include_directories(usetrack_core SYSTEM PUBLIC
        "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/cppwinrt")
endif()

# ---------------------------------------------------------------- 主程序

add_executable(usetrack WIN32
    src/main.cpp
    src/app/main_window.h
    src/app/main_window.cpp
    # exe 文件图标：MSVC 直接编译 .rc，资源管理器里就能看到应用图标。
    app.rc
)

target_link_libraries(usetrack PRIVATE usetrack_core)

# 关于页要显示版本号：把 CMake 的 PROJECT_VERSION 传进 main.cpp，
# 由 QCoreApplication::setApplicationVersion 落到运行时。
target_compile_definitions(usetrack PRIVATE PATINA_VERSION=\"${PROJECT_VERSION}\")

# 托盘图标：浅色/深色任务栏 × 记录中/已暂停 四个变体，运行时按主题选择
qt_add_resources(usetrack "tray_icons"
    PREFIX "/tray"
    BASE "resources/tray"
    FILES
        resources/tray/on-light-active.png
        resources/tray/on-light-paused.png
        resources/tray/on-dark-active.png
        resources/tray/on-dark-paused.png
)

# 应用图标：窗口图标与左侧导航栏头像共用
qt_add_resources(usetrack "app_icons"
    PREFIX "/icons"
    BASE "resources/icons"
    FILES
        resources/icons/app.png
)

# 运行时 ElaWidgetTools.dll 必须与 exe 同目录
add_custom_command(TARGET usetrack POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_ela_root}/bin/ElaWidgetTools.dll"
        "$<TARGET_FILE_DIR:usetrack>"
    COMMENT "复制 ElaWidgetTools.dll 到输出目录"
)

# ---------------------------------------------------------------- 开发探针

# 控制台采样探针，只用于验证 platform 层，不进主程序。
# 默认不构建：需要时用 -DPATINA_BUILD_PROBES=ON 打开。
option(PATINA_BUILD_PROBES "构建 platform 采样探针" OFF)

if(PATINA_BUILD_PROBES)
    add_executable(usetrack_foreground_probe
        src/platform/foreground_probe.cpp
    )
    target_link_libraries(usetrack_foreground_probe PRIVATE usetrack_core)
endif()
