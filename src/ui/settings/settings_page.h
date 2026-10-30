#ifndef PATINA_UI_SETTINGS_SETTINGS_PAGE_H
#define PATINA_UI_SETTINGS_SETTINGS_PAGE_H

// ui/ 只经 data/ 的对外入口取数，不写 SQL、不 include platform/。
// 开机自启在 platform/ 里，所以本页不直接调它：界面只发信号，
// 由 app/ 的 MainWindow 去读写注册表，再通过 setAutostartState() 回灌状态。
#include "data/repositories.h"

#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

class QEvent;
class QLabel;
class QPushButton;
class QVBoxLayout;
class ElaComboBox;
class ElaPivot;
class ElaPushButton;
class ElaRadioButton;
class ElaSlider;
class ElaToggleButton;
class ElaToggleSwitch;

namespace patina::ui
{

// 设置页：三面板（追踪 / 外观 / 常驻）+ 既有存储、备份、导入导出。
//
// 编辑模型分两套：
//   - 三面板走「攒草稿 → 点保存」：追踪、外观、常驻三个面板的每个字段都记进
//     _draft，保存时一次落 settings 表，取消则整组回滚到 _saved。三个面板共用
//     同一根保存条，所以「改了外观却按了取消」必须一起作废。
//   - 存储卡保持「即改即存」：备份、恢复、清理本来就是一次性动作。
//
// 布局：三面板在上，左右两列的存储/备份卡在下。
class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPage(data::Database* database, QWidget* parent = nullptr);
    ~SettingsPage() override;

    // 页面可见性由 MainWindow 驱动：切走时释放列表类控件，切回来重查一次。
    void setActive(bool active);

    // 开机自启的当前状态由 MainWindow 从注册表读出来回灌（见文件头的说明）。
    // 回灌时不会再触发 autostartRequested，避免自激。
    void setAutostartState(bool enabled);

    // —— 托盘菜单是这两个开关的第二个入口 ——
    // 托盘拨过之后引擎已经把值写进库，这里只把界面拉到同一状态（不写库、不发信号）。
    void setTrackingPaused(bool paused);
    void setTitleRecordingEnabled(bool enabled);

    // —— 诊断面板的取数来源 ——
    // ui/ 不 include platform/，进程工作集与句柄数由 app/ 层取好后塞进来；
    // 图表缓存的字节数与看门狗时间戳同理（引擎对象不在 ui/ 手里）。
    struct DiagnosticsSnapshot
    {
        bool processValid = false;
        qint64 workingSetBytes = 0;
        qint64 privateBytes = 0;
        int handleCount = 0;
        qint64 chartCacheBytes = 0;
        qint64 databaseFileBytes = 0;  // 主库 + -wal（只报主文件会让人以为数据丢了）
        bool healthValid = false;
        qint64 lastHeartbeatMs = 0;
        qint64 lastSuccessfulSampleMs = 0;
        qint64 lastWatchdogSealMs = 0;
    };
    void setDiagnosticsSource(std::function<DiagnosticsSnapshot()> source);

    // —— 壁纸（即时生效，不走三面板草稿）——
    // 开关与来源都在 MainWindow 手里落地；界面只回灌显示状态。
    // statusText 为空表示「没有额外说明」，非空时按 isError 选颜色显示。
    // busy 为真时禁用「换一张」，避免下载中重复点（下载最长 25 秒）。
    void setWallpaperState(bool enabled, const QString& source, const QString& statusText,
                           bool isError, bool busy = false);
    // 壁纸透明度（20~100，百分数）。即时生效，与开关走同一套「即改即存」。
    void setWallpaperOpacity(int percent);

signals:
    // 用户拨动了开机自启开关。state 是期望值，真正写没写成由 setAutostartState() 回灌。
    void autostartRequested(bool enabled);

    // 数据被整库替换过（恢复备份）：其它页面缓存的元信息（应用名、分类覆盖项、
    // 工具状态）都过期了，由 MainWindow 负责让它们重新加载。
    void databaseReplaced();

    // —— 三面板的即时/保存后信号，MainWindow 消费 ——

    // 追踪面板「保存」成功后发出：MainWindow 转 TrackingService::setPaused。
    // paused 是保存后的目标值。
    void trackingPauseChanged(bool paused);

    // 标题录制开关保存后发出：MainWindow 转 TrackingService::setTitleRecordingEnabled，
    // 让读缓存与托盘菜单立刻跟上（关掉时还要清掉当前段的标题）。
    void titleRecordingChanged(bool enabled);

    // 外观面板点选主题模式后发出（"light"|"dark"|"system"）：
    // MainWindow 负责把三值换算成 ElaTheme 的 Light/Dark 并落 QPalette。
    void themeModePicked(const QString& mode);

    // 常驻面板（窗口行为）：MainWindow 更新托盘/窗口行为。
    void trayIconVisibleChanged(bool visible);
    void closeBehaviorChanged(bool closeToTray);
    void minimizeBehaviorChanged(bool minimizeToTray);
    // 启动最小化只影响下次启动（main.cpp 读库），不发即时信号。

    // —— 壁纸 ——
    // 开关或来源变了：MainWindow 立即落库并按新状态贴图/撤图。
    void wallpaperSettingsChanged(bool enabled, const QString& source);
    // 点了「换一张」：MainWindow 去下载、存盘、重新贴图，完成后用 setWallpaperState 回灌。
    void wallpaperRefreshRequested(const QString& source);
    // 透明度滑块（20~100）：立即重新贴图并落库，不需要点保存。
    void wallpaperOpacityChanged(int percent);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildUi();
    // —— 三面板 ——
    QWidget* buildTrackingPanel();
    QWidget* buildAppearancePanel();
    // 壁纸区块拼进外观面板（即时生效，与主题模式分开渲染）。
    QWidget* buildWallpaperBlock(QWidget* parent);
    QWidget* buildResidentPanel();
    QWidget* buildSaveBar();
    void loadThreePanelSettings();
    void applyThreePanelDraft();
    void saveThreePanelDraft();
    void discardThreePanelDraft();
    void refreshSaveBar();
    void refreshThemeModeControl();
    void refreshBehaviorSegments();
    // 分段组互斥：只把目标按钮设为选中（带值守卫，防库的 toggle 信号递归）。
    static void setSegmentChecked(ElaToggleButton* selected,
                                  QVector<ElaToggleButton*> group);
    // 分段组信号接线：互斥 + 空选拒绝（点击已选中项摆回去），激活时执行 onActivated。
    void bindSegment(ElaToggleButton* button, QVector<ElaToggleButton*> group,
                     const std::function<void()>& onActivated);
    QWidget* buildStorageCard();
    // 资源诊断。只读、手动刷新，不起定时器。
    QWidget* buildDiagnosticsCard();
    QWidget* buildRecentBackupCard();
    QWidget* buildStartupCard();
    QWidget* buildExportCard();
    QWidget* buildRestoreCard();
    QWidget* buildCleanupCard();
    QWidget* buildDataImportCard();

    void refreshAll();
    void refreshStorage();
    void refreshDiagnostics();
    void refreshRecentBackups();
    void refreshExportPath();
    void refreshRestoreChecklist();
    void refreshCleanupPreview();
    void refreshImportBatches();

    void onOpenDataDirectory();
    void onChooseDataDirectory();
    void onExportBackup();
    void onUseDefaultBackupPath();
    void onPickBackupFile();
    void onConfirmRestore();
    void onCleanup();

    // —— 数据导出（CSV/Markdown/SQLite）——
    void onExportData();
    // —— 导入（预览 → 提交 → 批次管理）——
    void onPickImportFile();
    void onConfirmImport();
    void onImportBatchSelected(int row);

    // 清理范围的截止时间：30 天 / 90 天 / 全部（全部用一个足够大的值）。
    qint64 cleanupCutoffMs() const;
    // 当前清理档位的中文说明（「30 天前」/「90 天前」/「全部」），预览文案用。
    QString cleanupRangeLabel() const;
    void setStatus(const QString& message, bool error);
    void selectBackup(const QString& path);

    // 说明文字、校验记号、状态行这些 QLabel 只能靠样式表上色。颜色不写死在
    // 各处的 setStyleSheet 里，而是给标签挂一个 role 属性，由这里统一按当前
    // 主题刷 —— 否则切到深色主题后压在白底上的深灰字会糊成一片。
    void applyThemeColors(bool dark);
    // 拖放区那条虚线边框同样要跟着主题走。
    void refreshDropAreaStyle(bool dark);

    data::Database* _database = nullptr;
    bool _active = false;
    // 回灌控件状态时挡住信号：否则 setIsToggled / setCurrentIndex 会触发一轮写操作。
    bool _loading = false;

    // —— 三面板的草稿 ——
    // 追踪面板（保存生效；键名与 engine/tracking_settings.h 的读侧一致）。
    bool _draftTrackingPaused = false;
    bool _draftTitleRecording = true;
    int _draftMergeGapSecs = 180;   // timeline_merge_gap_secs（兼 AFK 阈值）
    int _draftIdleTimeoutSecs = 900; // idle_timeout_secs
    // 外观面板：主题模式（"light"|"dark"|"system"）。Ela 只有两档明暗，
    // 「跟随系统」由 MainWindow 监听 colorSchemeChanged 换算。
    QString _draftThemeMode = QStringLiteral("system");
    bool _draftTrayIconVisible = true;
    bool _draftCloseToTray = true;      // close_behavior：tray | exit
    bool _draftMinimizeToTray = false;  // minimize_behavior：taskbar | tray
    bool _draftStartMinimized = true;   // start_minimized
    // 保存过的值：dirty 比对基准。
    bool _savedTrackingPaused = false;
    bool _savedTitleRecording = true;
    int _savedMergeGapSecs = 180;
    int _savedIdleTimeoutSecs = 900;
    QString _savedThemeMode = QStringLiteral("system");
    bool _savedTrayIconVisible = true;
    bool _savedCloseToTray = true;
    bool _savedMinimizeToTray = false;
    bool _savedStartMinimized = true;

    QString _exportPath;
    QString _selectedBackupPath;
    data::BackupInspection _inspection;

    // —— 导出与导入的界面状态 ——
    QString _dataExportFormat;       // "csv" | "markdown" | "sqlite"
    QString _importFilePath;
    QString _importFingerprint;
    data::ImportPreview _importPreview;
    QVector<data::ImportBatchSummary> _importBatches;

    QLabel* _statusLabel = nullptr;
    QLabel* _dataDirLabel = nullptr;
    QLabel* _databaseSizeLabel = nullptr;
    QLabel* _backupDirSizeLabel = nullptr;
    QLabel* _sessionCountLabel = nullptr;
    QLabel* _dataDirHint = nullptr;
    QLabel* _autostartHint = nullptr;
    QLabel* _exportPathLabel = nullptr;
    QLabel* _recentEmptyLabel = nullptr;
    QLabel* _dropHintLabel = nullptr;
    QLabel* _restoreMessageLabel = nullptr;
    QLabel* _cleanupPreviewLabel = nullptr;
    QLabel* _importPreviewLabel = nullptr;
    QLabel* _importEmptyLabel = nullptr;

    // —— 诊断面板 ——
    std::function<DiagnosticsSnapshot()> _diagnosticsSource;
    QLabel* _workingSetLabel = nullptr;
    QLabel* _privateBytesLabel = nullptr;
    QLabel* _handleCountLabel = nullptr;
    QLabel* _iconCacheLabel = nullptr;
    QLabel* _chartCacheLabel = nullptr;
    QLabel* _databaseFileLabel = nullptr;
    QLabel* _healthHeartbeatLabel = nullptr;
    QLabel* _healthSampleLabel = nullptr;
    QLabel* _healthSealLabel = nullptr;
    QVBoxLayout* _importBatchLayout = nullptr;

    // —— 三面板控件 ——
    // 分段按钮一律 ElaToggleButton：库内置的互斥选中按钮，自带按压缩放/hover
    // 渐变与主题适配（ElaPushButton + checkable 会被它的按压动画压扁）。
    QLabel* _dirtyLabel = nullptr;
    ElaPushButton* _saveButton = nullptr;
    ElaPushButton* _cancelButton = nullptr;
    ElaToggleSwitch* _trackingPausedSwitch = nullptr;
    ElaToggleSwitch* _titleRecordingSwitch = nullptr;
    ElaToggleSwitch* _trayIconSwitch = nullptr;
    ElaToggleSwitch* _startMinimizedSwitch = nullptr;
    ElaPushButton* _mergeGapMinus = nullptr;
    ElaPushButton* _mergeGapPlus = nullptr;
    QLabel* _mergeGapValue = nullptr;
    ElaPushButton* _idleMinus = nullptr;
    ElaPushButton* _idlePlus = nullptr;
    QLabel* _idleValue = nullptr;
    // 主题分段（浅/深/跟随系统）：toggled 即当前模式。
    ElaToggleButton* _themeLightButton = nullptr;
    ElaToggleButton* _themeDarkButton = nullptr;
    ElaToggleButton* _themeSystemButton = nullptr;
    ElaToggleButton* _closeTrayButton = nullptr;
    ElaToggleButton* _closeExitButton = nullptr;
    ElaToggleButton* _minTaskbarButton = nullptr;
    ElaToggleButton* _minTrayButton = nullptr;

    // —— 壁纸（外观面板内，即时生效不走草稿）——
    ElaToggleSwitch* _wallpaperSwitch = nullptr;
    ElaRadioButton* _wallpaperLoliApiButton = nullptr;
    ElaRadioButton* _wallpaperUapisButton = nullptr;
    ElaPushButton* _wallpaperRefreshButton = nullptr;
    QLabel* _wallpaperStatusLabel = nullptr;
    // 壁纸透明度：滑块按 20%~100% 取值，0 会让壁纸等于没开，所以不放开。
    ElaSlider* _wallpaperOpacitySlider = nullptr;
    QLabel* _wallpaperOpacityValueLabel = nullptr;
    // 当前来源（"loliapi"|"uapis"），换源时用它判断要不要顺手刷新。
    QString _wallpaperSource = QStringLiteral("loliapi");

    // 既有存储卡（自启开关已迁入常驻面板，原 buildStartupCard 弃用；
    // 但 _autostartSwitch/_autostartHint 仍由 setAutostartState 回灌指向新面板的控件）。
    ElaToggleSwitch* _autostartSwitch = nullptr;
    ElaPivot* _cleanupPivot = nullptr;
    ElaComboBox* _dataExportFormatBox = nullptr;

    QVBoxLayout* _recentLayout = nullptr;

    struct CheckRow
    {
        QLabel* mark = nullptr;
        QLabel* text = nullptr;
    };
    QVector<CheckRow> _restoreChecks;

    QWidget* _dropArea = nullptr;
    QPushButton* _confirmRestoreButton = nullptr;
    QPushButton* _cleanupButton = nullptr;
    QPushButton* _exportButton = nullptr;
};

} // namespace patina::ui

#endif // PATINA_UI_SETTINGS_SETTINGS_PAGE_H
