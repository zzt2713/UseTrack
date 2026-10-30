#ifndef PATINA_APP_TRACKING_SERVICE_H
#define PATINA_APP_TRACKING_SERVICE_H

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <optional>
#include <thread>

#include "engine/window_sample.h"
#include "platform/foreground_window.h"

class QTimer;

namespace patina::data
{
class Database;
class ActivityReminderChecker;
} // namespace patina::data

namespace patina::engine
{
class SessionTracker;
class TrackingSettingsCache;
class RuntimeHealthState;
} // namespace patina::engine

namespace patina::platform
{
class PowerWatcher;
enum class PowerSignal;
struct SustainedSignalsSnapshot;
} // namespace patina::platform

namespace patina::app
{

class SqliteSessionStore;

// 追踪链路的编排者：采样 → session 语义 → 落库。
//
// 这是 1e 里唯一把三个模块接起来的地方，本身不含业务语义：
//   - 采样时序归 platform/（sampleForegroundWindow + idleMillis）
//   - 边界语义归 engine/（SessionTracker）
//   - 存取归 data/（经 SqliteSessionStore 接缝）
//
// 后台静默运行：不创建任何 Qt 窗口，托盘图标由 MainWindow 单独负责。
// 唯一创建的 Win32 对象是 PowerWatcher 那个不可见顶层窗口——电源广播只发给
// 顶层窗口，这是 Windows 的限制，不是这里多此一举。
//
// 线程：采样在 GUI 线程跑（QTimer 驱动）；媒体/音频探测、看门狗
// 各自有独立 std::thread，通过原子健康时间戳与互斥锁和主线程协同。
class TrackingService : public QObject
{
    Q_OBJECT

public:
    explicit TrackingService(QObject* parent = nullptr);
    ~TrackingService() override;

    TrackingService(const TrackingService&) = delete;
    TrackingService& operator=(const TrackingService&) = delete;

    // 打开库、跑启动自愈、起采样定时器与电源监听。
    // 失败时返回 false 并把原因写进 errorOut；此时服务处于停止状态。
    bool start(QString* errorOut = nullptr);

    // 停止采样与电源监听。已落库的数据不动：未闭合段留给下次启动的
    // recoverAtStartup 封口（启动封口本来就要处理这种情况）。
    void stop();

    bool isRunning() const;

    // 已打开的数据库句柄，供 ui/ 的读模型使用。未 start 或 start 失败时为 nullptr。
    // 只读语义由调用方（ui/ 只用读模型）保证，这里不做强制。
    data::Database* database() const { return _database.get(); }

    // 数据库文件的完整路径，供设置页与排查使用。
    QString databasePath() const { return _databasePath; }

    // 覆写数据库路径（默认在 AppDataLocation 下）。测试与 --db 参数用。
    void setDatabasePath(const QString& path) { _databasePath = path; }

    // 追踪暂停/恢复。暂停时封口当前段（reason tracking-paused-sealed），
    // 暂停期间不产生新段；重启后从 settings 读回状态保持。
    bool setPaused(bool paused, QString* errorOut = nullptr);
    bool togglePaused(QString* errorOut = nullptr);
    bool isPaused() const;

    // 标题录制总开关。写库 + 立即更新读缓存；关闭时把当前未闭合段的
    // 标题清空并闭合标题样本。
    bool setTitleRecordingEnabled(bool enabled, QString* errorOut = nullptr);
    // 读缓存（非 const：TrackingSettingsCache 的读路径带 TTL 刷新）。
    bool isTitleRecordingEnabled();

    // 健康时间戳快照（诊断面板用）。
    std::optional<qint64> lastHeartbeatMs() const;
    std::optional<qint64> lastSuccessfulSampleMs() const;
    std::optional<qint64> lastWatchdogSealMs() const;

signals:    // 采样结果变化时发出，供 UI 观察（当前无页面消费，保留接口）。
    void sampleHandled(const QString& outcome);

    // 出现非致命错误（采样失败、写库失败等），服务会继续跑。
    void trackingError(const QString& message);

    // 暂停状态变化时发出，托盘/设置页消费。
    void trackingPausedChanged(bool paused);

    // 标题录制开关变化（托盘菜单与设置页双向同步）。
    void titleRecordingChanged(bool enabled);

    // 活动提醒规则触发（某应用/分类今日用量到限）。标题与正文已拼好，
    // MainWindow 负责弹托盘系统通知。
    void activityReminderRaised(const QString& title, const QString& body);

private:
    void onTick();
    void onPowerSignal(platform::PowerSignal signal);

    QString resolveDatabasePath() const;

    // 旧文件名（patina.db）的数据在新文件名下不存在时，复制一份过来。
    static void migrateLegacyDatabase(const QString& path);

    // 活动提醒规则的节流时间戳（采样循环里每分钟查一次就够）。
    qint64 _lastActivityReminderCheckMs = 0;
    std::unique_ptr<data::ActivityReminderChecker> _activityReminderChecker;

    // 前台探测，带降级。失败时返回缓存窗口 + fromCachedWindow=true，
    // 缓存超过 60s 则硬降级为「无窗口」。
    struct ProbeResult
    {
        bool ok = false;           // 真探测成功（非降级）
        bool degraded = false;     // 用了缓存
        bool hardDegraded = false; // 缓存也过期，视为无窗口
        // 真探测拿到的窗口。onTick 直接用它，不再重复采样一次。
        platform::ForegroundWindow window;
    };
    ProbeResult probeForegroundWindow(qint64 nowMs);

    // display_name 解析。缓存内 10 分钟有效。
    QString resolveDisplayName(const QString& exePath, const QString& exeName);

    // 看门狗线程侧。只把封口请求投递回 GUI 线程，不碰数据库。
    // 线程不能直接用 Database：QSqlDatabase 绑定创建它的线程，
    // 跨线程调 isOpen() 会返回 false（Qt 同时打 "requested database
    // does not belong to the calling thread"）。
    void requestWatchdogSeal(qint64 targetMs);

    // GUI 线程侧执行封口。与主采样共用 _sealMutex。
    void sealStaleSessionOnGuiThread(qint64 targetMs);

    std::unique_ptr<data::Database> _database;
    std::unique_ptr<SqliteSessionStore> _store;
    std::unique_ptr<engine::TrackingSettingsCache> _settingsCache;
    std::unique_ptr<engine::SessionTracker> _tracker;
    std::unique_ptr<engine::RuntimeHealthState> _health;
    std::unique_ptr<platform::PowerWatcher> _powerWatcher;
    QTimer* _timer = nullptr;

    QString _databasePath;
    bool _running = false;

    // 探针是否成功启动（失败不致命：sustained 永远不可用）。
    bool _sustainedSignalsStarted = false;

    // 看门狗线程。
    std::unique_ptr<std::thread> _watchdogThread;
    std::atomic_bool _watchdogRunning{false};

    // 主采样与看门狗封口的互斥。SQLite 单连接 + tracker 都不是线程
    // 安全的，两边必须串行。封口一律在 GUI 线程执行（看门狗只投递请求），
    // 所以这把锁实际只保护「采样循环 vs GUI 线程投递进来的封口」。
    QMutex _sealMutex;

    // 已投递但还没执行的看门狗封口时刻（毫秒）。看门狗每 8 秒卡顿只
    // 需要封一次，重复投递在 GUI 线程侧按这个值去重。
    std::atomic<qint64> _pendingWatchdogSealMs{0};

    // 探测降级状态。
    std::optional<engine::WindowSample> _lastGoodSample;
    qint64 _lastGoodSampleAtMs = 0;
    qint64 _fallbackStartedAtMs = 0;
    qint64 _lastRecoveryAttemptMs = 0;
    bool _hardDegraded = false;

    // 显示名缓存：exePath → displayName。
    QHash<QString, QString> _displayNameCache;
};

} // namespace patina::app

#endif // PATINA_APP_TRACKING_SERVICE_H
