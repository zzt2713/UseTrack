# coding-style

本文件是 usetrack-cpp 的编码约束。改代码、加功能、重构都按这里的规矩来。

## 1. 技术栈

- 语言：C++17
- Qt：6.8.3（`D:\Qt\6.8.3\msvc2022_64`），模块只允许 **Core / Gui / Widgets / Sql**
- UI 库：ElaWidgetTools 
- 构建：CMake + MSVC（x64）
- 数据库：SQLite，经 `QSqlDatabase`

### ElaWidgetTools 使用规则

- CMake 接入：`find_package(ElaWidgetTools REQUIRED PATHS "D:/cppsoft/MsvcEla/lib/Cmake")`，链接目标 `ElaWidgetTools`；头文件经 `ElaWidgetTools_INCLUDE_DIRS` 引入
- 运行时需要 `ElaWidgetTools.dll`（位于 `D:\cppsoft\MsvcEla\bin`）与 exe 同目录或在 PATH 中
- 主窗口基于 `ElaWindow`（自带无边框标题栏、导航栏、主题切换），页面容器用 `ElaScrollPage` / `ElaCentralStackedWidget`
- 控件优先用 Ela 提供的：`ElaPushButton`、`ElaToggleSwitch`、`ElaComboBox`、`ElaContentDialog`、`ElaColorPicker`、`ElaStatusBar` 等；Ela 没有的再用 Qt 原生控件
- 图标用 `ElaIcon`（内置图标集），应用图标等特殊图标仍自取
- **不修改** Ela 库本身；需要定制外观时在子类/样式表层做
- 主题跟随 `ElaTheme`（Light/Dark），不自建深浅色体系

## 2. 能力边界

以下能力**一律不实现**，任何目录、类、配置项都不得为其预留空间：

| 不做 | 原因 |
|---|---|
| 一切联网能力（HTTP/WebSocket/同步/上传） | 完全离线，除壁纸外 |
| WebDAV / 远程备份 / 定时备份 | 联网；本地备份只做手动导出/恢复 |
| 国际化框架（i18n/翻译文件/语言切换） | 界面只保留w |
| Widget 小组件 / 多窗口权限体系 | 单主窗口 + 托盘足够 |
| 质量门禁脚本体系（mutation test、风格债 checker 等） | 过度工程 |

**禁止**：出现 `QNetworkAccessManager`、`QTcpSocket`、`QWebEngine*`、`Qml` 相关 include 或 CMake 依赖。

## 3. 内存红线（最高优先级）

目标：托盘常驻空闲 ≤ **30 MB** 工作集；主窗口打开 ≤ **50 MB**。超出即视为缺陷。

1. **不用 QML / QtQuick**。整个 UI 用 Qt Widgets，页面是 `QWidget`。
2. 页面懒加载：启动只创建托盘 + 追踪引擎；首个页面在主窗口首次打开时才创建。
3. 页面隐藏时释放重资源（图表位图、大列表缓存）；能重建的不常驻。
4. 应用图标缓存设上限（LRU，≤ 256 项）；不缓存全尺寸原图，只存缩放后 pixmap。
5. SQLite 单连接（或极小连接池），读查询用 `LIMIT/keyset`，不一次 `SELECT` 全表进内存。
6. 追踪引擎的状态数据用紧凑结构，不为历史明细在常驻内存建索引——历史按需查库。
7. 定时器粒度合理：采样定时器 1s 级，UI 刷新按脏标记合并，不做每秒全量重绘。
8. 不留后台常驻的隐藏窗口、多余 `QTimer`、无用单例缓存。

## 4. 界面与语言

- **UI 文案只写中文**，直接用中文字符串 + `tr()` 包裹（为将来可能的文本修正留口，但不建翻译文件）。
- 代码标识符（类名、函数名、变量名）用英文。
- 注释用中文。
- UI 风格基于Fluent 风格，克制、低打扰；深浅色跟随 `ElaTheme`（系统切换自动响应）。**重要界面改版前先在 `preview/` 出 HTML 预览稿**（多方案、报数量、等选择）。

## 5. 目录结构与所有权

```
usetrack-cpp/
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   ├── app/            # 应用壳：主窗口、托盘、自启、设置页入口。保持薄，不沉淀业务。
│   ├── engine/         # 追踪引擎：前台采样、session 生命周期、AFK/锁屏/休眠边界。产品主链。
│   ├── platform/       # Win32 收口：窗口枚举、电源通知、图标提取、注册表自启。所有 Win32 调用只准出现在这里。
│   ├── data/           # SQLite：schema、migration、repository、读模型聚合。UI 不准直接写 SQL。
│   ├── ui/             # 页面 widgets，按功能分目录：
│   │   ├── dashboard/      # 今日概览
│   │   ├── history/        # 历史时间轴
│   │   ├── data/           # 趋势/热力图
│   │   ├── classification/ # 分类管理
│   │   ├── destination/    # 详情弹窗
│   │   ├── tools/          # 提醒/计时/番茄钟
│   │   ├── settings/       # 设置（含数据目录、备份导出/恢复、清理）
│   │   ├── about/          # 关于
│   │   └── shared/         # 跨页面复用控件（分类右键菜单、小时柱状图等）
│   └── shared/         # 跨模块纯工具（时间格式化等），不得依赖 ui/、data/
```

边界规则：

- `ui/*` 不 include `platform/*`，不写 SQL；需要数据一律经 `data/` 的接口。
- `platform/*` 不 include `ui/*`；Win32 回调通过信号/纯虚接口上抛。
- `engine/` 拥有采样时序与 session 边界语义，是核心难点，语义见第 6 节。
- 新代码落在真正拥有它的层；不确定时先问，不顺手乱放。

## 6. 必须保留的边界语义（不许"简化"掉）

这些语义是被真实问题锤打出来的，改动时**照抄行为**，只简化实现结构：

1. 无操作（AFK）不计入有效活动；AFK 结束回到活动要正确闭合/开新段。
2. 锁屏、睡眠、休眠：唤醒后不把离开时长串进活动段。
3. 异常退出（崩溃/断电）后重启：上一条 session 必须被正确封口，不留悬挂段。
4. 长时间离开后回前台：开新 session，不续接旧段。
5. 低交互场景（视频、会议）不误判为 AFK。
6. 写事实与写派生状态在同一 SQLite 事务内。
7. 任何破坏性操作（清理历史、恢复备份）前先校验目标路径与数据完整性。

具体判定参数（AFK 阈值、采样间隔等）改动前先核对现有实现，不凭感觉定。

## 7. 工程习惯

- 每个功能先定可验证目标，循环验证到通过；不做无验证的"完成了"。
- 精准修改：不顺手重构没坏的代码；自己产生的孤儿代码自己清。
- 不确定就问，不隐藏困惑。
- Git：只提交源代码（不含 build 产物、.db、preview 临时脚本）；提交前展示变更摘要；提交信息用简洁英文；不自动 push。
- 数据库 schema 有版本号和 migration；migration 只前进不回退。

## 8. 注释

不要写多余的注释，只写简洁的执行逻辑具体的大类工作。

## 9.Casting

仅使用 C++ 风格的类型转换：`static_cast<T>(val)`, `dynamic_cast<T*>(ptr)`, `reinterpret_cast<T>(ptr)`。禁止 C 风格转换。

## 10.Formatting

- 单行 `if`、`else`、`for`、`while` 也必须使用大括号
- `switch` 中的 `case`、`default` 分支体必须显式添加大括号
- 函数定义必须展开，函数签名和大括号分别换行
- 使用项目根目录的 `.clang-format` 进行格式化（LLVM-based, 4-space indent, Allman braces）

## 11.Comments

- 代码标识符（类名、方法名、变量名）：英文

- 文档注释和用户可见信息：中文

- 不要写解释代码做什么的注释 — 起好名字就够了。只在 WHY 不明显时才加注释（隐藏的约束、微妙的不变量、变通方案）

- 内部结构体的每个字段必须有注释，说明数据身份、业务含义或有效条件

  
