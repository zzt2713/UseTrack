# UseTrack

Windows 本地应用使用时长追踪工具。常驻托盘，自动记录前台应用切换，把一天的时间流向摊开给你看。

完全离线，不联网、不上传、不收集任何数据——所有记录都躺在你本机的 SQLite 里。

## 功能

**今日概览** — 当前在用什么、今天用了多久、各应用和分类的时长分布、小时活动图、与昨日对比。

**历史回看** — 按天翻页，24 小时时间轴一条色带铺满全天（每个时刻只显示占时最多的那个应用），下方是活动流明细，右侧是当日分布图例。双击时间轴可以放大到 1–24 小时的视口看分应用明细轨。

**数据统计** — 7 天 / 30 天 / 90 天 / 自定义区间的趋势曲线，支持按应用或按分类看，可自选要对比的应用；还有全年热力图。

**分类管理** — 给应用改显示名、配色、归到自定义分类；把几个 exe 关联成「同一个应用」，统计时合并到父应用头上；把不想看到的应用藏起来。

**小工具** — 提醒、倒计时、番茄钟。

**设置** — 数据目录迁移、CSV / Markdown / SQLite 三种格式导出、外部数据导入（带预览和批次回滚）、历史清理、备份恢复。

## 技术栈

| | |
|---|---|
| 语言 | C++17 |
| UI | Qt 6.8 Widgets + [ElaWidgetTools](https://github.com/Liniyous/ElaWidgetTools) |
| 构建 | CMake + MSVC (x64) |
| 存储 | SQLite（经 `QSqlDatabase`） |

除 ElaWidgetTools 外无第三方依赖。所有图表用 `QPainter` 自绘。

## 架构

```
src/
├── main.cpp
├── app/        应用壳：主窗口、托盘、单实例、追踪服务编排
├── engine/     追踪引擎：采样时序、session 生命周期、AFK/锁屏/休眠边界
├── platform/   Win32 收口：窗口枚举、电源通知、图标提取、注册表自启
├── data/       SQLite：schema/migration、repository、读模型聚合
├── ui/         页面 widgets，按功能分目录
└── shared/     跨模块纯工具
```

几条硬边界：

- 所有 Win32 调用只出现在 `platform/`，其余层不碰 `<windows.h>` 业务 API
- `ui/` 不写 SQL、不 include `platform/`，数据一律经 `data/` 的接口
- `engine/` 拥有采样时序与 session 边界语义，是整套东西的核心

## 几个实现上值得说的点

**Session 边界语义** — AFK 不计入有效活动；锁屏、睡眠唤醒后不把离开时长串进活动段；崩溃重启后上一条 session 必须被正确封口；长时间离开再回前台要开新段而不是续接；看视频、开会这类低交互场景不能误判成 AFK。这七条都有对应的实测验证。

**时间轴** — 24 小时视口下，每分钟只画占时最长的那个应用，再按合并阈值把相邻同应用分钟段连起来。这样任何时刻只有一个颜色，不会出现多行重叠、每块不足 3px 的情况。放大对话框里才切成分应用明细轨。

**单实例** — 命名管道 + OVERLAPPED I/O。第二个实例启动时把「激活」消息写进管道再退出，主实例收到后把窗口提到前台。

**内存** — 托盘常驻工作集目标 ≤ 30 MB，主窗口打开 ≤ 50 MB。页面懒加载、图标 LRU 缓存、读模型查询全部带行数上限、UI 刷新按脏标记合并。

**数据兼容** — 导出格式与外部同类工具的数据双向兼容：CSV 表头认中英文两套列名，时间戳认 RFC3339 和 `yyyy-MM-dd HH:mm:ss` 两种，`.db` 认反规范化和规范化两种表结构。

## 构建

需要 Qt 6.8.3 (MSVC 2022 64-bit) 与 ElaWidgetTools 预编译库。

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="<Qt 路径>" -DELA_ROOT="<ElaWidgetTools 根目录>"
cmake --build build --config Release
```

`ElaWidgetTools.dll` 会在构建后自动拷到输出目录。

## 数据位置

默认在 `%APPDATA%\UseTrack\`：

```
config.json     数据目录等设置
usetrack.db     活动记录
wallpaper/      壁纸缓存
```

数据目录可在设置页改到别处，会实际搬迁库文件并校验行数。

## 许可

未定。
