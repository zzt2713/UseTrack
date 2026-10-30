#include "app/tracking_service.h"

#include "app/sqlite_session_store.h"
#include "data/activity_reminder_checker.h"
#include "data/database.h"
#include "data/dashboard_read_model.h"
#include "data/icon_cache_repository.h"
#include "engine/session_tracker.h"
#include "engine/tracking_settings.h"
#include "engine/watchdog.h"
#include "engine/window_sample.h"
#include "platform/foreground_window.h"
#include "platform/icon_extractor.h"
#include "platform/idle_time.h"
#include "platform/packaged_app.h"
#include "platform/power_watcher.h"
#include "platform/sustained_signals.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>

#include <windows.h>

#include <optional>
#include <vector>

namespace patina::app
{
namespace
{

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

// platform 的电源信号映射到 engine 的边界语义。
// 两层类型刻意不同（platform 不 include engine），映射只能在这里做。
engine::PowerEvent toEngineEvent(platform::PowerSignal signal)
{
    switch (signal) {
    case platform::PowerSignal::Lock:
        return engine::PowerEvent::Lock;
    case platform::PowerSignal::Unlock:
        return engine::PowerEvent::Unlock;
    case platform::PowerSignal::Suspend:
        return engine::PowerEvent::Suspend;
    case platform::PowerSignal::Resume:
        return engine::PowerEvent::Resume;
    }
    return engine::PowerEvent::Resume;
}

const char* describePowerSignal(platform::PowerSignal signal)
{
    switch (signal) {
    case platform::PowerSignal::Lock:
        return "lock";
    case platform::PowerSignal::Unlock:
        return "unlock";
    case platform::PowerSignal::Suspend:
        return "suspend";
    case platform::PowerSignal::Resume:
        return "resume";
    }
    return "unknown";
}

// 探测降级常量：连续失败 10 秒后降低探测频率，冷却 30 秒再试，60 秒仍失败就长期降级。
constexpr qint64 kProbeRecoveryAfterMs = 10'000;
constexpr qint64 kProbeRecoveryCooldownMs = 30'000;
constexpr qint64 kProbeHardDegradedAfterMs = 60'000;
// 单线程架构下无法中途取消同步探测，改为事后比对耗时：
// 超过这个值就把本帧判为探测失败。
constexpr qint64 kProbeTimeoutMs = 3'000;

constexpr qint64 kDisplayNameCacheTtlMs = 10 * 60 * 1000;

// 活动提醒规则的检查间隔：规则按「今日用量」触发，分钟级粒度足够。
constexpr qint64 kActivityReminderCheckIntervalMs = 60 * 1000;

// 从 exe 的版本资源里取显示名：FileDescription → ProductName → CompanyName，
// 拿不到退回 exe 名。
QString displayNameFromVersionResource(const QString& exePath)
{
    const std::wstring widePath = exePath.toStdWString();
    DWORD handle = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(widePath.c_str(), &handle);
    if (size == 0) {
        return QString();
    }

    std::vector<char> buffer(size);
    if (!::GetFileVersionInfoW(widePath.c_str(), 0, size, buffer.data())) {
        return QString();
    }

    // 依次尝试 FileDescription / ProductName / CompanyName。
    // 子块语言代码页随构建变化，常见 040904b0（英文 Unicode）与 080404b0
    //（简中 Unicode），两种都试一遍；还可以从 \\VarFileInfo\\Translation
    // 精确读取，但试表已经覆盖了绝大多数场景。
    static const wchar_t* const keys[] = {
        L"\\StringFileInfo\\040904b0\\FileDescription",
        L"\\StringFileInfo\\040904b0\\ProductName",
        L"\\StringFileInfo\\040904b0\\CompanyName",
        L"\\StringFileInfo\\080404b0\\FileDescription",
        L"\\StringFileInfo\\080404b0\\ProductName",
    };
    for (const wchar_t* key : keys) {
        void* value = nullptr;
        UINT valueLength = 0;
        if (::VerQueryValueW(buffer.data(), key, &value, &valueLength) && value != nullptr
            && valueLength > 0) {
            const QString text =
                QString::fromWCharArray(static_cast<const wchar_t*>(value),
                                        static_cast<int>(valueLength));
            if (!text.trimmed().isEmpty()) {
                return text.trimmed();
            }
        }
    }
    return QString();
}

} // namespace

TrackingService::TrackingService(QObject* parent)
    : QObject(parent)
    , _health(std::make_unique<engine::RuntimeHealthState>())
    , _sealMutex()
{
}

TrackingService::~TrackingService()
{
    stop();
}

bool TrackingService::isRunning() const
{
    return _running && _timer != nullptr && _timer->isActive();
}

QString TrackingService::resolveDatabasePath() const
{
    if (!_databasePath.isEmpty()) {
        return _databasePath;
    }

    // AppDataLocation 在 Windows 上展开成 %APPDATA%/<组织>/<应用>，
    // main.cpp 已设好 applicationName / organizationName。
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (directory.isEmpty()) {
        return QString();
    }
    return directory + QStringLiteral("/usetrack.db");
}

// 旧版本把数据存在 patina.db。改名之后旧库还在原地，直接换成新文件名等于
// 让用户的所有历史凭空消失，所以首次启动时把旧库整份复制过来（旧文件留着，
// 不删——万一复制失败或用户想回滚，原数据还在）。
//
// 只在「新库还不存在」时做：新库一旦建立就说明已经迁移过或用户是全新安装，
// 这时旧文件再出现（比如用户手动拷回来）不该覆盖正在用的库。
void TrackingService::migrateLegacyDatabase(const QString& path)
{
    const QFileInfo info(path);
    if (info.exists() && info.size() > 0) {
        return;
    }

    // 两个位置都要看：
    //   1. 与目标同目录——用户改过数据目录、或程序名没变时的情形；
    //   2. 旧的应用数据目录 %APPDATA%/Patina/Patina——改名同时换了
    //      organizationName / applicationName，默认路径整个搬了家。
    QStringList candidates;
    candidates.append(info.absoluteDir().absoluteFilePath(QStringLiteral("patina.db")));
    const QString roaming =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (!roaming.isEmpty()) {
        candidates.append(roaming + QStringLiteral("/Patina/Patina/patina.db"));
    }

    QString legacyPath;
    for (const QString& candidate : candidates) {
        const QFileInfo legacy(candidate);
        if (legacy.exists() && legacy.size() > 0) {
            legacyPath = candidate;
            break;
        }
    }
    if (legacyPath.isEmpty()) {
        return;
    }

    // WAL 模式下的库，数据可能还在 -wal 里，只拷主文件会丢最近的记录。
    if (!QFile::copy(legacyPath, path)) {
        qWarning("[tracking] 旧数据库复制失败，将新建空库：%s", qUtf8Printable(legacyPath));
        return;
    }
    const QString legacyWal = legacyPath + QStringLiteral("-wal");
    if (QFileInfo::exists(legacyWal)) {
        QFile::copy(legacyWal, path + QStringLiteral("-wal"));
    }
    qInfo("[tracking] 已把旧数据库复制到新文件名：%s", qUtf8Printable(path));
}

bool TrackingService::start(QString* errorOut)
{
    if (_running) {
        return true;
    }

    const QString path = resolveDatabasePath();
    if (path.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = tr("无法确定数据库路径");
        }
        return false;
    }

    // 目录不存在就建：首次运行与换了数据目录后都要走到这里。
    const QFileInfo info(path);
    const QDir directory = info.absoluteDir();
    if (!directory.exists() && !QDir().mkpath(directory.absolutePath())) {
        if (errorOut != nullptr) {
            *errorOut = tr("无法创建数据目录：%1").arg(directory.absolutePath());
        }
        return false;
    }

    migrateLegacyDatabase(path);

    _database = std::make_unique<data::Database>();
    const data::Status opened = _database->openAndMigrate(path);
    if (!opened.ok()) {
        _database.reset();
        if (errorOut != nullptr) {
            *errorOut = opened.error;
        }
        return false;
    }
    _databasePath = path;

    _store = std::make_unique<SqliteSessionStore>(*_database);
    _settingsCache = std::make_unique<engine::TrackingSettingsCache>(*_store);
    _tracker = std::make_unique<engine::SessionTracker>(*_store, _settingsCache.get(),
                                                        engine::TrackingPolicy{});
    // 活动提醒规则接在采样循环里。规则不存在时查询是零行，开销可忽略。
    _activityReminderChecker = std::make_unique<data::ActivityReminderChecker>(*_database);

    // 启动时先把库里的暂停键读出来——重启后保持暂停状态。
    // 读失败 → 安全暂停，但这次启动允许在下次轮询重试。
    if (_settingsCache->trackingPaused(nowMs())) {
        _tracker->applyPausedHint(true);
    }

    // 先把上次异常退出留下的悬挂段封口，再开始采样。
    QMutexLocker sealLocker(&_sealMutex);
    const data::Result<engine::StartupReport> recovered = _tracker->recoverAtStartup(nowMs());
    if (!recovered.ok()) {
        sealLocker.unlock();
        _tracker.reset();
        _settingsCache.reset();
        _store.reset();
        _database.reset();
        if (errorOut != nullptr) {
            *errorOut = recovered.error;
        }
        return false;
    }
    if (recovered.value.sealedOpenSession) {
        qInfo("[tracking] 已封口上次残留的未闭合段，结束于 %lld（时间戳采信：%s）",
              static_cast<long long>(recovered.value.sealedEndMs),
              recovered.value.hadLastSample && !recovered.value.normalizedClock ? "是" : "否");
    }
    sealLocker.unlock();

    // 锁屏/挂起事件。监听窗口不可见，不占任务栏。
    _powerWatcher = std::make_unique<platform::PowerWatcher>();
    QString powerError;
    if (!_powerWatcher->start(
            [this](platform::PowerSignal signal) { onPowerSignal(signal); }, &powerError)) {
        // 电源监听失败不致命：采样停顿（stallSealAfterMs）仍能兜住锁屏/休眠，
        // 只是封口会晚一个停顿窗口。降级继续跑，并明确报出来。
        qWarning("[tracking] 电源监听启动失败，锁屏只能靠采样停顿兜底：%s",
                 qUtf8Printable(powerError));
        emit trackingError(powerError);
        _powerWatcher.reset();
    }

    // 媒体/音频信号探测。失败不致命，低交互场景退化为普通 AFK 判定。
    platform::startSustainedSignals();
    _sustainedSignalsStarted = true;

    // 看门狗线程。独立于 GUI 事件循环，GUI 卡死时仍能发现停顿；
    // 但封口动作必须回到 GUI 线程（Database 单线程约束），这里只投递。
    _watchdogRunning.store(true);
    _watchdogThread = std::make_unique<std::thread>([this]() {
        engine::runWatchdogLoop(
            _watchdogRunning, *_health,
            [this](qint64 targetMs) { requestWatchdogSeal(targetMs); });
    });

    if (_timer == nullptr) {
        _timer = new QTimer(this);
        _timer->setTimerType(Qt::CoarseTimer);
        connect(_timer, &QTimer::timeout, this, &TrackingService::onTick);
    }
    _timer->start(static_cast<int>(_tracker->policy().sampleIntervalMs));

    _lastActivityReminderCheckMs = 0;

    // 图标持久化桥接。内存 LRU 未命中时读 icon_cache 表，
    // 提取成功（或失败负缓存）写回。调用方都在 GUI 线程（采样 QTimer 与
    // UI 绘制），与这条连接同线程，符合 Database 的单线程约束。
    // 库关掉时解绑（stop/换库路径都会走到）。
    data::Database* database = _database.get();
    platform::setIconPersistence(
        [database](const QString& exeName) -> QImage {
            if (database == nullptr || !database->isOpen()) {
                return QImage();
            }
            const data::IconCacheRepository repository(*database);
            const data::Result<QImage> icon = repository.loadIcon(exeName);
            return icon.ok() ? icon.value : QImage();
        },
        [database](const QString& exeName, const QImage& image) {
            if (database == nullptr || !database->isOpen()) {
                return;
            }
            data::IconCacheRepository repository(*database);
            repository.saveIcon(exeName, image, QDateTime::currentMSecsSinceEpoch());
        });

    _running = true;
    return true;
}

void TrackingService::stop()
{
    if (_timer != nullptr) {
        _timer->stop();
    }
    if (_powerWatcher) {
        _powerWatcher->stop();
        _powerWatcher.reset();
    }
    if (_sustainedSignalsStarted) {
        platform::stopSustainedSignals();
        _sustainedSignalsStarted = false;
    }
    if (_watchdogThread) {
        _watchdogRunning.store(false);
        _watchdogThread->join();
        _watchdogThread.reset();
    }
    // 已投递但还没执行的封口请求会落到下一拍事件循环，那时 _tracker 已被
    // 下面几行 reset 掉——靠 sealStaleSessionOnGuiThread 里的 `!_tracker`
    // 分支兜住，不会 use-after-free。这里清掉去重标记，免得下次 start()
    // 恰好撞上同一个 targetMs 被误判成「已投递过」。
    _pendingWatchdogSealMs.store(0);
    // 销毁 _tracker/_store 前必须持 _sealMutex：看门狗线程可能正拿着这把锁
    // 在 sealOpenSessionPublic 里用它们，不锁就是 use-after-free。
    // 看门狗已经 join 了，理论上不会再进来——但 PowerWatcher 的窗口过程
    // 走消息泵，销毁时机微妙，锁一下是零成本的保险。
    QMutexLocker sealLocker(&_sealMutex);
    // 未闭合段故意留着：下次启动的 recoverAtStartup 按最后成功采样封口，
    // 这是正常路径，不是遗漏。
    _activityReminderChecker.reset();
    platform::setIconPersistence({}, {});
    _tracker.reset();
    _settingsCache.reset();
    _store.reset();
    _database.reset();
    sealLocker.unlock();
    _running = false;
}

void TrackingService::onTick()
{
    if (!_tracker) {
        return;
    }

    const qint64 sampleAtMs = nowMs();
    _health->noteHeartbeat(sampleAtMs);

    // 探测 + 降级。探不到就用缓存窗口，缓存过期就硬降级为无窗口。
    const ProbeResult probe = probeForegroundWindow(sampleAtMs);

    engine::WindowSample sample;
    sample.sampledAtMs = sampleAtMs;
    if (probe.hardDegraded) {
        sample.probeOk = true; // 采样流程本身没失败，只是没有可跟踪对象
        sample.valid = false;
    } else if (probe.degraded) {
        // 用上次成功的缓存窗口：内容有效但「最后成功采样」不推进。
        sample = *_lastGoodSample;
        sample.fromCachedWindow = true;
        sample.sampledAtMs = sampleAtMs;
        sample.idleMs = platform::idleMillis();
        sample.probeOk = true;
    } else {
        // 复用 probeForegroundWindow 已经拿到的窗口：同一帧只采样一次，
        // 避免两次采样之间用户切窗口导致 probe 与 sample 不一致。
        const platform::ForegroundWindow& window = probe.window;
        sample.probeOk = true;
        sample.valid = !window.isEmpty();
        sample.hwnd = window.hwnd;
        sample.rootOwnerHwnd = window.rootOwnerHwnd;
        sample.processId = window.processId;
        sample.windowClass = window.windowClass;
        sample.title = window.title;
        sample.exeName = window.exeName;
        sample.exePath = window.exePath;
        sample.appUserModelId = window.appUserModelId;
        sample.idleMs = platform::idleMillis();
        // display_name 解析（缓存内 10 分钟有效）。
        sample.displayName = resolveDisplayName(window.exePath, window.exeName);
        if (sample.valid && !sample.fromCachedWindow) {
            _lastGoodSample = sample;
            _lastGoodSampleAtMs = sampleAtMs;
            _hardDegraded = false;
            _fallbackStartedAtMs = 0;
        }
    }

    // 读探测线程的最新快照（互斥锁在 platform 内部，读取便宜）。
    // 局部变量不能叫 signals —— Qt 把 signals 定义成 public 的宏。
    const platform::SustainedSignalsSnapshot probeSnapshot = platform::currentSustainedSignals();
    engine::SustainedSignal sustainedMedia;
    sustainedMedia.available = probeSnapshot.systemMedia.available;
    sustainedMedia.active = probeSnapshot.systemMedia.active;
    sustainedMedia.source = engine::SustainedSignalSource::SystemMedia;
    sustainedMedia.sourceAppId = probeSnapshot.systemMedia.sourceAppId;
    sustainedMedia.exeName = probeSnapshot.systemMedia.exeName;
    sustainedMedia.exePath = probeSnapshot.systemMedia.exePath;

    engine::SustainedSignal sustainedAudio;
    sustainedAudio.available = probeSnapshot.audioSession.available;
    sustainedAudio.active = probeSnapshot.audioSession.active;
    sustainedAudio.source = engine::SustainedSignalSource::AudioSession;
    sustainedAudio.sourceAppId = probeSnapshot.audioSession.sourceAppId;
    sustainedAudio.exeName = probeSnapshot.audioSession.exeName;
    sustainedAudio.exePath = probeSnapshot.audioSession.exePath;

    // 主采样与看门狗共用一把锁；看门狗排队等锁时若主循环恢复，
    // 复核逻辑会放弃封口。
    QMutexLocker sealLocker(&_sealMutex);

    const data::Result<engine::SampleOutcome> outcome =
        _tracker->handleSample(sample, sustainedMedia, sustainedAudio);
    if (outcome.ok() && !sample.fromCachedWindow && sample.valid) {
        _health->noteSuccessfulSample(sampleAtMs);
    }
    sealLocker.unlock();

    if (!outcome.ok()) {
        emit trackingError(outcome.error);
        qWarning("[tracking] %s", qUtf8Printable(outcome.error));
        return;
    }

    // 活动提醒规则检查。分钟级节流，规则不存在时只花一次零行查询。
    if (_activityReminderChecker
        && sampleAtMs - _lastActivityReminderCheckMs >= kActivityReminderCheckIntervalMs) {
        _lastActivityReminderCheckMs = sampleAtMs;
        const qint64 dayStartMs = data::localDayStartMs(sampleAtMs);
        const QString dateKey =
            QDateTime::fromMSecsSinceEpoch(dayStartMs).date().toString(QStringLiteral("yyyy-MM-dd"));
        const data::Result<QVector<data::ActivityReminderHit>> hits =
            _activityReminderChecker->fireDueRules(dateKey, dayStartMs, sampleAtMs);
        if (hits.ok()) {
            for (const data::ActivityReminderHit& hit : hits.value) {
                const QString label = hit.label.isEmpty() ? QStringLiteral("应用") : hit.label;
                emit activityReminderRaised(
                    QStringLiteral("使用提醒"),
                    QStringLiteral("%1今日已使用 %2 分钟：%3")
                        .arg(label,
                             QString::number(hit.limitMs / 60000),
                             hit.message.isEmpty() ? QStringLiteral("该休息一下了") : hit.message));
            }
        } else {
            qWarning("[tracking] 活动提醒检查失败：%s", qUtf8Printable(hits.error));
        }
    }

    emit sampleHandled(QString::fromLatin1(engine::describeOutcome(outcome.value)));
}

TrackingService::ProbeResult TrackingService::probeForegroundWindow(qint64 nowMs)
{
    ProbeResult result;

    // 硬降级状态下先做有界恢复尝试（10 秒后开始，30 秒冷却）。
    // 只有真正到期的那一帧才往下走真探测，其余帧直接判无窗口。
    const bool recoveryAttempt = _hardDegraded
        && nowMs - _lastGoodSampleAtMs >= kProbeHardDegradedAfterMs
        && nowMs - _lastRecoveryAttemptMs >= kProbeRecoveryCooldownMs;
    if (_hardDegraded && !recoveryAttempt) {
        result.hardDegraded = true;
        return result;
    }
    if (recoveryAttempt) {
        _lastRecoveryAttemptMs = nowMs;
    }

    // 真探测：sampleForegroundWindow 是纯同步 Win32 调用（GetForegroundWindow /
    // OpenProcess / GetWindowTextW 等），没有会长时间阻塞的调用，单次延迟通常
    // < 10ms。单线程 QTimer 架构下无法中途取消，改为「事后量耗时」：
    // 超过 3 秒的这一帧按探测失败处理，与超时语义一致（失败帧不推进时间戳、
    // 进入降级计数）。
    QElapsedTimer elapsed;
    elapsed.start();
    const platform::ForegroundWindow window = platform::sampleForegroundWindow();
    const qint64 elapsedMs = elapsed.elapsed();
    if (elapsedMs > kProbeTimeoutMs) {
        qWarning("[tracking] 前台探测耗时 %lld ms，超过 %lld ms 上限，本帧按失败处理",
                 static_cast<long long>(elapsedMs), static_cast<long long>(kProbeTimeoutMs));
    } else if (!window.isEmpty()) {
        result.ok = true;
        result.window = window;
        return result;
    }

    // 探测失败：进入 fallback。
    if (_fallbackStartedAtMs == 0) {
        _fallbackStartedAtMs = nowMs;
    }
    if (nowMs - _fallbackStartedAtMs >= kProbeHardDegradedAfterMs) {
        _hardDegraded = true;
        result.hardDegraded = true;
        return result;
    }

    // 用缓存窗口（10 秒后开始做有界恢复尝试，30 秒冷却）。
    if (_lastGoodSample.has_value() && nowMs - _lastGoodSampleAtMs >= kProbeRecoveryAfterMs
        && nowMs - _lastRecoveryAttemptMs >= kProbeRecoveryCooldownMs) {
        _lastRecoveryAttemptMs = nowMs;
    }
    result.degraded = _lastGoodSample.has_value();
    if (!result.degraded) {
        result.hardDegraded = true;
    }
    return result;
}

QString TrackingService::resolveDisplayName(const QString& exePath, const QString& exeName)
{
    if (exePath.trimmed().isEmpty() || exeName.trimmed().isEmpty()) {
        return QString();
    }

    const auto cached = _displayNameCache.constFind(exePath);
    if (cached != _displayNameCache.constEnd()) {
        return *cached;
    }

    QString displayName = displayNameFromVersionResource(exePath);
    if (displayName.isEmpty()) {
        // 拿不到退回 exe 名（去掉 .exe）。
        displayName = exeName;
        if (displayName.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
            displayName.chop(4);
        }
    }
    // 版本资源字符串尾部常带 \0 或零宽字符（截图中名字后的「□」）：
    // 截到第一个控制字符，再剔除零宽与 BOM 类字符。
    for (int i = 0; i < displayName.size(); ++i) {
        const QChar ch = displayName.at(i);
        if (ch.category() == QChar::Other_Control || ch.unicode() == 0) {
            displayName.truncate(i);
            break;
        }
    }
    displayName.remove(QChar(0xFEFF));   // BOM
    displayName.remove(QChar(0x200B));   // zero-width space
    displayName = displayName.trimmed();
    _displayNameCache.insert(exePath, displayName);
    return displayName;
}

void TrackingService::requestWatchdogSeal(qint64 targetMs)
{
    // 看门狗线程：这里绝对不能碰 _tracker/_store/_database。
    // QSqlDatabase 绑定创建它的线程，从别的线程调 isOpen() 会拿到无效连接，
    // 表现为「数据库未打开」+ Qt 的 "requested database does not belong to
    // the calling thread"。所以只投递，真正的封口在 GUI 线程做。
    //
    // 卡顿期间看门狗每秒都会命中一次，同一个 targetMs 只投一次：
    // GUI 冻住 60 秒时排 60 个 lambda 没意义，而且它们读的是同一个时刻。
    if (_pendingWatchdogSealMs.exchange(targetMs) == targetMs) {
        return;
    }
    QMetaObject::invokeMethod(
        this, [this, targetMs]() { sealStaleSessionOnGuiThread(targetMs); },
        Qt::QueuedConnection);
}

void TrackingService::sealStaleSessionOnGuiThread(qint64 targetMs)
{
    // GUI 线程。锁内复核 last_successful == target，不等说明排队期间
    // 主循环恢复了，放弃本次封口。
    // emit 放到锁外：Qt 直连信号会同步进 UI，锁内发有死锁风险。
    bool didSeal = false;
    {
        QMutexLocker locker(&_sealMutex);
        if (!_tracker) {
            return;
        }
        const std::optional<qint64> current = _health->lastSuccessfulSampleMs();
        if (!current.has_value() || *current != targetMs) {
            return;
        }
        const data::Result<bool> sealed = _tracker->sealOpenSessionPublic(targetMs);
        // 匿名段同源：看门狗封口时它也可能开着，一并收在同一个时刻。
        const data::Status anonymousSealed = _tracker->sealAnonymousPublic(targetMs);
        if (!anonymousSealed.ok()) {
            qWarning("[tracking] 看门狗封口匿名段失败：%s",
                     qUtf8Printable(anonymousSealed.error));
        }
        if (sealed.ok() && sealed.value) {
            _health->noteWatchdogSeal(targetMs);
            didSeal = true;
        }
    }
    if (didSeal) {
        qInfo("[tracking] 看门狗封口了停顿期间的活动段，结束于 %lld",
              static_cast<long long>(targetMs));
        emit sampleHandled(QStringLiteral("watchdog-sealed"));
    }
}

bool TrackingService::setPaused(bool paused, QString* errorOut)
{
    if (!_tracker) {
        if (errorOut != nullptr) {
            *errorOut = tr("追踪服务未启动");
        }
        return false;
    }

    bool sealOk = false;
    {
        QMutexLocker locker(&_sealMutex);
        const data::Result<bool> result = _tracker->setPaused(paused, nowMs());
        if (!result.ok()) {
            if (errorOut != nullptr) {
                *errorOut = result.error;
            }
            return false;
        }
        sealOk = result.value;
    }

    // 状态落库，重启后保持。失败时回滚内存标记，保持两边一致。
    const data::Status persisted = _store->saveSettingValue(
        engine::trackingPausedKey(), paused ? QStringLiteral("1") : QStringLiteral("0"));
    if (!persisted.ok()) {
        QMutexLocker locker(&_sealMutex);
        _tracker->setPaused(!paused, nowMs());
        if (errorOut != nullptr) {
            *errorOut = persisted.error;
        }
        return false;
    }

    _settingsCache->setTrackingPausedHint(paused);
    if (sealOk) {
        qInfo("[tracking] 追踪已%s，封口了当前活动段", paused ? "暂停" : "恢复");
    }
    emit trackingPausedChanged(paused);
    return true;
}

bool TrackingService::togglePaused(QString* errorOut)
{
    return setPaused(!isPaused(), errorOut);
}

bool TrackingService::isPaused() const
{
    return _tracker != nullptr && _tracker->isPaused();
}

bool TrackingService::setTitleRecordingEnabled(bool enabled, QString* errorOut)
{
    if (!_store || !_settingsCache) {
        if (errorOut != nullptr) {
            *errorOut = tr("追踪服务未启动");
        }
        return false;
    }

    const data::Status persisted = _store->saveSettingValue(
        engine::titleRecordingEnabledKey(), enabled ? QStringLiteral("1") : QStringLiteral("0"));
    if (!persisted.ok()) {
        if (errorOut != nullptr) {
            *errorOut = persisted.error;
        }
        return false;
    }
    _settingsCache->setTitleRecordingHint(enabled);

    // 关闭时把当前段的标题抹掉（与写入串行，避免和采样撞同一段）。
    if (!enabled) {
        QMutexLocker locker(&_sealMutex);
        const data::Result<bool> cleared = _store->clearOpenTitle(nowMs());
        if (!cleared.ok()) {
            qWarning("[tracking] 清除当前段标题失败：%s", qUtf8Printable(cleared.error));
        }
    }

    emit titleRecordingChanged(enabled);
    return true;
}

bool TrackingService::isTitleRecordingEnabled()
{
    // 未启动时按默认值 true（与库播种一致）。
    return _settingsCache != nullptr ? _settingsCache->titleRecordingEnabled(nowMs()) : true;
}

std::optional<qint64> TrackingService::lastHeartbeatMs() const
{
    return _health != nullptr ? _health->lastHeartbeatMs() : std::nullopt;
}

std::optional<qint64> TrackingService::lastSuccessfulSampleMs() const
{
    return _health != nullptr ? _health->lastSuccessfulSampleMs() : std::nullopt;
}

std::optional<qint64> TrackingService::lastWatchdogSealMs() const
{
    return _health != nullptr ? _health->lastWatchdogSealMs() : std::nullopt;
}

void TrackingService::onPowerSignal(platform::PowerSignal signal)
{
    if (!_tracker) {
        return;
    }

    QMutexLocker locker(&_sealMutex);
    const data::Status status = _tracker->handlePowerEvent(toEngineEvent(signal), nowMs());
    locker.unlock();
    if (!status.ok()) {
        emit trackingError(status.error);
        qWarning("[tracking] 电源事件 %s 处理失败：%s",
                 describePowerSignal(signal),
                 qUtf8Printable(status.error));
    }
}

} // namespace patina::app
