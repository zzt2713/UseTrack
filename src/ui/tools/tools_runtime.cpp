#include "ui/tools/tools_runtime.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <optional>

namespace patina::ui
{

namespace
{

// 页面可见时每秒刷一次：数字要跟着跳。
constexpr int kVisibleTickMs = 1000;
// 页面不可见时放慢到 10 秒：只要不漏掉到点判定就够。
// 到点本身靠 updateTickTimer 里的短唤醒兜住，不依赖这个粒度。
constexpr int kHiddenTickMs = 10000;
// 落盘节流：秒表每秒都在变，但没必要每秒写一次 SQLite。
constexpr qint64 kPersistIntervalMs = 20000;
constexpr qint64 kMinuteMs = 60 * 1000;

QString newReminderId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString phaseLabel(PomodoroPhase phase)
{
    switch (phase) {
    case PomodoroPhase::ShortBreak:
        return QStringLiteral("短休息");
    case PomodoroPhase::LongBreak:
        return QStringLiteral("长休息");
    case PomodoroPhase::Focus:
        break;
    }
    return QStringLiteral("专注");
}

// —— 内存态 ↔ data/ 持久化行的转换 ——

data::ToolTimerRow toTimerRow(const ToolTimer& timer)
{
    data::ToolTimerRow row;
    switch (timer.mode) {
    case TimerMode::Countdown:
        row.mode = data::ToolTimerMode::Countdown;
        break;
    case TimerMode::Stopwatch:
        break;
    }
    switch (timer.status) {
    case TimerStatus::Running:
        row.status = data::ToolTimerStatus::Running;
        break;
    case TimerStatus::Paused:
        row.status = data::ToolTimerStatus::Paused;
        break;
    case TimerStatus::Completed:
        row.status = data::ToolTimerStatus::Completed;
        break;
    case TimerStatus::Idle:
        break;
    }
    row.label = timer.label;
    row.durationMs = timer.durationMs;
    row.accumulatedMs = timer.accumulatedMs;
    row.startedAtMs = timer.startedAtMs;
    row.completedAtMs = timer.completedAtMs;
    return row;
}

ToolTimer fromTimerRow(const data::ToolTimerRow& row)
{
    ToolTimer timer;
    switch (row.mode) {
    case data::ToolTimerMode::Countdown:
        timer.mode = TimerMode::Countdown;
        break;
    case data::ToolTimerMode::Stopwatch:
        break;
    }
    switch (row.status) {
    case data::ToolTimerStatus::Running:
        timer.status = TimerStatus::Running;
        break;
    case data::ToolTimerStatus::Paused:
        timer.status = TimerStatus::Paused;
        break;
    case data::ToolTimerStatus::Completed:
        timer.status = TimerStatus::Completed;
        break;
    case data::ToolTimerStatus::Idle:
        break;
    }
    timer.label = row.label;
    timer.durationMs = row.durationMs;
    timer.accumulatedMs = row.accumulatedMs;
    timer.startedAtMs = row.startedAtMs;
    timer.completedAtMs = row.completedAtMs;
    return timer;
}

data::ToolPomodoroRow toPomodoroRow(const ToolPomodoro& pomodoro)
{
    data::ToolPomodoroRow row;
    switch (pomodoro.phase) {
    case PomodoroPhase::ShortBreak:
        row.phase = data::ToolPomodoroPhase::ShortBreak;
        break;
    case PomodoroPhase::LongBreak:
        row.phase = data::ToolPomodoroPhase::LongBreak;
        break;
    case PomodoroPhase::Focus:
        break;
    }
    switch (pomodoro.status) {
    case PomodoroStatus::Running:
        row.status = data::ToolPomodoroStatus::Running;
        break;
    case PomodoroStatus::Paused:
        row.status = data::ToolPomodoroStatus::Paused;
        break;
    case PomodoroStatus::Completed:
        row.status = data::ToolPomodoroStatus::Completed;
        break;
    case PomodoroStatus::Idle:
        break;
    }
    row.cycleIndex = pomodoro.cycleIndex;
    row.completedFocusCount = pomodoro.completedFocusCount;
    row.longBreakEvery = pomodoro.longBreakEvery;
    row.focusMs = pomodoro.focusMs;
    row.shortBreakMs = pomodoro.shortBreakMs;
    row.longBreakMs = pomodoro.longBreakMs;
    row.phaseStartedAtMs = pomodoro.phaseStartedAtMs;
    row.phaseRemainingAtStartMs = pomodoro.phaseRemainingAtStartMs;
    return row;
}

ToolPomodoro fromPomodoroRow(const data::ToolPomodoroRow& row)
{
    ToolPomodoro pomodoro;
    switch (row.phase) {
    case data::ToolPomodoroPhase::ShortBreak:
        pomodoro.phase = PomodoroPhase::ShortBreak;
        break;
    case data::ToolPomodoroPhase::LongBreak:
        pomodoro.phase = PomodoroPhase::LongBreak;
        break;
    case data::ToolPomodoroPhase::Focus:
        break;
    }
    switch (row.status) {
    case data::ToolPomodoroStatus::Running:
        pomodoro.status = PomodoroStatus::Running;
        break;
    case data::ToolPomodoroStatus::Paused:
        pomodoro.status = PomodoroStatus::Paused;
        break;
    case data::ToolPomodoroStatus::Completed:
        pomodoro.status = PomodoroStatus::Completed;
        break;
    case data::ToolPomodoroStatus::Idle:
        break;
    }
    pomodoro.cycleIndex = row.cycleIndex;
    pomodoro.completedFocusCount = row.completedFocusCount;
    pomodoro.longBreakEvery = row.longBreakEvery;
    pomodoro.focusMs = row.focusMs;
    pomodoro.shortBreakMs = row.shortBreakMs;
    pomodoro.longBreakMs = row.longBreakMs;
    pomodoro.phaseStartedAtMs = row.phaseStartedAtMs;
    pomodoro.phaseRemainingAtStartMs = row.phaseRemainingAtStartMs;
    return pomodoro;
}

} // namespace

ToolsRuntime& ToolsRuntime::instance()
{
    static ToolsRuntime runtime;
    return runtime;
}

ToolsRuntime::ToolsRuntime(QObject* parent) : QObject(parent)
{
    _tickTimer = new QTimer(this);
    _tickTimer->setTimerType(Qt::CoarseTimer);
    connect(_tickTimer, &QTimer::timeout, this, &ToolsRuntime::onTick);
}

void ToolsRuntime::attach(data::Database* database)
{
    if (database == nullptr) {
        // 解除绑定：库里存着的东西不再有效（测试里库会被销毁）。
        // 不清状态、不重查，只断指针并把计时器停掉。
        _database = nullptr;
        _loaded = false;
        _tickTimer->stop();
        return;
    }
    // 同一个库重复绑定时不要重跑：重跑会把「重启恢复」再走一遍，
    // 已经触发过的提醒会再弹一次。
    if (_database == database && _loaded) {
        return;
    }
    _database = database;
    _loaded = true;
    reloadFromStore();
}

void ToolsRuntime::reloadFromStore()
{
    _reminders.clear();
    _laps.clear();
    _activityRules.clear();
    _timer = ToolTimer();
    _pomodoro = ToolPomodoro();
    _settings = ToolsSettings();

    if (_database == nullptr || !_database->isOpen()) {
        updateTickTimer();
        emit changed();
        return;
    }

    // 工具设置仍是轻量键值（状态搬进专用表，设置五项留在 settings）。
    data::SettingsRepository settingsRepo(*_database);
    const data::Result<std::optional<QString>> storedSettings =
        settingsRepo.value(QStringLiteral("tools.settings"));
    if (storedSettings.ok() && storedSettings.value.has_value()
        && !storedSettings.value->trimmed().isEmpty()) {
        // 与旧版 JSON 格式保持兼容：只取五项数值。
        const QJsonDocument document = QJsonDocument::fromJson(storedSettings.value->toUtf8());
        if (document.isObject()) {
            const QJsonObject object = document.object();
            const auto readInt = [&object](const char* key, int fallback) {
                const QJsonValue value = object.value(QLatin1String(key));
                return value.isDouble() ? static_cast<int>(value.toDouble()) : fallback;
            };
            _settings.defaultCountdownMinutes = readInt("defaultCountdownMinutes", 25);
            _settings.pomodoroFocusMinutes = readInt("pomodoroFocusMinutes", 25);
            _settings.pomodoroShortBreakMinutes = readInt("pomodoroShortBreakMinutes", 5);
            _settings.pomodoroLongBreakMinutes = readInt("pomodoroLongBreakMinutes", 15);
            _settings.pomodoroLongBreakEvery = readInt("pomodoroLongBreakEvery", 4);
        }
    }

    data::ToolsRepository repository(*_database);

    const data::Result<QVector<data::ToolReminderRow>> reminders = repository.listReminders();
    if (reminders.ok()) {
        for (const data::ToolReminderRow& row : reminders.value) {
            ToolReminder reminder;
            reminder.id = row.id;
            reminder.label = row.label;
            reminder.scheduledAtMs = row.scheduledAtMs;
            reminder.createdAtMs = row.createdAtMs;
            switch (row.status) {
            case data::ToolReminderStatus::Fired:
                reminder.status = ReminderStatus::Fired;
                break;
            case data::ToolReminderStatus::Cancelled:
                reminder.status = ReminderStatus::Cancelled;
                break;
            case data::ToolReminderStatus::Scheduled:
                reminder.status = ReminderStatus::Scheduled;
                break;
            }
            reminder.firedAtMs = row.firedAtMs.value_or(0);
            _reminders.append(reminder);
        }
    }

    const data::Result<data::ToolTimerRow> timer = repository.loadTimer();
    if (timer.ok()) {
        _timer = fromTimerRow(timer.value);
    }

    const data::Result<QVector<data::ToolLapRow>> laps = repository.listLaps();
    if (laps.ok()) {
        for (const data::ToolLapRow& row : laps.value) {
            ToolLap lap;
            lap.index = row.index;
            lap.startedAtMs = row.startedAtMs;
            lap.endedAtMs = row.endedAtMs;
            lap.durationMs = row.durationMs;
            _laps.append(lap);
        }
    }

    const data::Result<data::ToolPomodoroRow> pomodoro = repository.loadPomodoro();
    if (pomodoro.ok()) {
        _pomodoro = fromPomodoroRow(pomodoro.value);
    }

    // 每日统计在 tool_daily_stats 表里，番茄钟行里不再存 statDateKey。
    _pomodoro.statDateKey = todayDateKey();
    loadDailyStat();

    // 活动提醒规则（只取启用的）。
    reloadActivityRules();

    recoverAfterStartup(QDateTime::currentMSecsSinceEpoch());
    updateTickTimer();
    emit changed();
}

void ToolsRuntime::reloadActivityRules()
{
    _activityRules.clear();
    if (_database == nullptr || !_database->isOpen()) {
        return;
    }
    data::ToolsRepository repository(*_database);
    const data::Result<QVector<data::ToolActivityReminderRule>> rules =
        repository.listActivityRules(false);
    if (rules.ok()) {
        _activityRules = rules.value;
    }
}

// 重启后的状态恢复规则：
//
//   一次性提醒  待触发且已过点的 → 立刻补触发一次并转「已触发」。
//               未到点的继续等。取消掉的不动。
//   秒表        一律转暂停，累计时长就是最后一次落库的那个值。
//               落库时已经把运行中的那一段折进累计值了，所以这里只清起点，
//               关机期间的空档不会被算进去。
//   倒计时      保留运行状态，按剩零点判断。已过点 → 立刻置完成并发提示；
//               未过点 → 继续倒数（倒计时是绝对时刻语义，能跨关机）。
//   番茄钟      按当前阶段剩零点判断。已过点 → 记一次完成、推进阶段，
//               但下一阶段置「暂停」等手动开始；未过点 → 继续跑。
//   每日统计    按今天日期从 tool_daily_stats 读；跨天自然清零（表按日期键存）。
void ToolsRuntime::recoverAfterStartup(qint64 nowMs)
{
    rolloverDailyStat(nowMs);

    data::ToolsRepository repository(*_database);

    // 秒表：关掉的时候没在数，就不要假装它一直在数。
    if (_timer.mode == TimerMode::Stopwatch && _timer.status == TimerStatus::Running) {
        _timer.startedAtMs = 0;
        _timer.status = TimerStatus::Paused;
        persistTimer();
    }

    // 倒计时：过点了就直接完成，不用等下一次 tick。
    if (_timer.isCountdownDue(nowMs)) {
        _timer.accumulatedMs = _timer.durationMs;
        _timer.startedAtMs = 0;
        _timer.completedAtMs = nowMs;
        _timer.status = TimerStatus::Completed;
        persistTimer();

        ToolAlert alert;
        alert.kind = ToolAlert::Kind::Countdown;
        alert.title = QStringLiteral("倒计时结束");
        alert.body = _timer.label.trimmed().isEmpty() ? QStringLiteral("倒计时已走完")
                                                      : _timer.label.trimmed();
        alert.occurredAtMs = nowMs;
        emit alertRaised(alert);
    }

    // 番茄钟：过点了记完成、推进阶段，但停在暂停态等人点开始。
    if (_pomodoro.isPhaseDue(nowMs)) {
        if (_pomodoro.phase == PomodoroPhase::Focus) {
            ++_pomodoro.completedFocusToday;
            saveDailyStat();
        }
        const QString finishedPhase = phaseLabel(_pomodoro.phase);
        _pomodoro.advanceAfterCompletion();
        _pomodoro.status = PomodoroStatus::Paused;
        _pomodoro.phaseStartedAtMs = 0;
        persistPomodoro();

        ToolAlert alert;
        alert.kind = ToolAlert::Kind::Pomodoro;
        alert.title = QStringLiteral("番茄钟：%1结束").arg(finishedPhase);
        alert.body = QStringLiteral("下一阶段是%1，点开始继续").arg(phaseLabel(_pomodoro.phase));
        alert.occurredAtMs = nowMs;
        emit alertRaised(alert);
    }

    // 提醒：过点的补触发一次，按时间顺序发。逐条落库而不是整份重写：
    // 状态行在表里各归各，别的提醒不会被动到。
    for (ToolReminder& reminder : _reminders) {
        if (!reminder.isPending() || reminder.scheduledAtMs > nowMs) {
            continue;
        }
        reminder.status = ReminderStatus::Fired;
        reminder.firedAtMs = nowMs;
        persistReminder(reminder);

        ToolAlert alert;
        alert.kind = ToolAlert::Kind::Reminder;
        alert.title = QStringLiteral("提醒");
        alert.body = reminder.label.trimmed().isEmpty() ? QStringLiteral("时间到了")
                                                        : reminder.label.trimmed();
        alert.occurredAtMs = nowMs;
        emit alertRaised(alert);
    }
}

void ToolsRuntime::setUiVisible(bool visible)
{
    _uiVisible = visible;
    updateTickTimer();
}

void ToolsRuntime::updateTickTimer()
{
    // 有活干才跑表；纯待触发且还早的提醒不需要秒级唤醒。
    const bool needsTick = _uiVisible || hasActiveWork();
    if (!needsTick) {
        _tickTimer->stop();
        return;
    }
    _tickTimer->start(_uiVisible ? kVisibleTickMs : kHiddenTickMs);
}

void ToolsRuntime::onTick()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    bool dirty = rolloverDailyStat(nowMs), changedState = false;

    if (_timer.isCountdownDue(nowMs)) {
        _timer.accumulatedMs = _timer.durationMs;
        _timer.startedAtMs = 0;
        _timer.completedAtMs = nowMs;
        _timer.status = TimerStatus::Completed;
        persistTimer();
        changedState = true;

        ToolAlert alert;
        alert.kind = ToolAlert::Kind::Countdown;
        alert.title = QStringLiteral("倒计时结束");
        alert.body = _timer.label.trimmed().isEmpty() ? QStringLiteral("倒计时已走完")
                                                      : _timer.label.trimmed();
        alert.occurredAtMs = nowMs;
        emit alertRaised(alert);
    }

    // 番茄钟到点：记完成、推进阶段，下一阶段停着等人点。
    if (_pomodoro.isPhaseDue(nowMs)) {
        if (_pomodoro.phase == PomodoroPhase::Focus) {
            ++_pomodoro.completedFocusToday;
            saveDailyStat();
        }
        const QString finishedPhase = phaseLabel(_pomodoro.phase);
        _pomodoro.advanceAfterCompletion();
        _pomodoro.status = PomodoroStatus::Paused;
        _pomodoro.phaseStartedAtMs = 0;
        persistPomodoro();
        changedState = true;

        ToolAlert alert;
        alert.kind = ToolAlert::Kind::Pomodoro;
        alert.title = QStringLiteral("番茄钟：%1结束").arg(finishedPhase);
        alert.body = QStringLiteral("下一阶段是%1，点开始继续").arg(phaseLabel(_pomodoro.phase));
        alert.occurredAtMs = nowMs;
        emit alertRaised(alert);
    }

    for (ToolReminder& reminder : _reminders) {
        if (!reminder.isPending() || reminder.scheduledAtMs > nowMs) {
            continue;
        }
        reminder.status = ReminderStatus::Fired;
        reminder.firedAtMs = nowMs;
        persistReminder(reminder);
        changedState = true;

        ToolAlert alert;
        alert.kind = ToolAlert::Kind::Reminder;
        alert.title = QStringLiteral("提醒");
        alert.body = reminder.label.trimmed().isEmpty() ? QStringLiteral("时间到了")
                                                        : reminder.label.trimmed();
        alert.occurredAtMs = nowMs;
        emit alertRaised(alert);
    }

    // 运行中的秒表/倒计时每隔一段时间把累计时长写回去，
    // 这样进程被强杀也只丢最后这 20 秒。
    if (_timer.status == TimerStatus::Running
        && nowMs - _lastTimerPersistMs >= kPersistIntervalMs) {
        persistTimer();
    }

    // dirty 是跨日清零，changedState 是上面三种到点里的任意一种。
    // 两者都没有、页面又不可见时，连界面都别惊动。
    if (dirty || changedState) {
        updateTickTimer();
        emit changed();
    } else if (_uiVisible) {
        emit changed();
    }
}

bool ToolsRuntime::loadDailyStat()
{
    if (_database == nullptr || !_database->isOpen()) {
        return false;
    }
    data::ToolsRepository repository(*_database);
    const data::Result<int> count = repository.dailyFocusCount(_pomodoro.statDateKey);
    if (count.ok()) {
        _pomodoro.completedFocusToday = count.value;
        return true;
    }
    return false;
}

bool ToolsRuntime::saveDailyStat()
{
    if (_database == nullptr || !_database->isOpen()) {
        return false;
    }
    data::ToolsRepository repository(*_database);
    return repository.setDailyFocusCount(_pomodoro.statDateKey, _pomodoro.completedFocusToday).ok();
}

bool ToolsRuntime::rolloverDailyStat(qint64 nowMs)
{
    Q_UNUSED(nowMs)
    const QString today = todayDateKey();
    if (_pomodoro.statDateKey == today) {
        return false;
    }
    // 跨天了：今日完成数从 0 开始（旧值留在 tool_daily_stats 的历史日期行里），
    // completedFocusCount（算到第几轮）不清。
    _pomodoro.statDateKey = today;
    _pomodoro.completedFocusToday = 0;
    return true;
}

bool ToolsRuntime::hasPendingReminder() const
{
    return std::any_of(_reminders.cbegin(), _reminders.cend(),
                       [](const ToolReminder& reminder) { return reminder.isPending(); });
}

bool ToolsRuntime::hasRunningWork() const
{
    return _timer.status == TimerStatus::Running || _pomodoro.status == PomodoroStatus::Running;
}

bool ToolsRuntime::hasActiveWork() const
{
    return hasRunningWork() || hasPendingReminder();
}

QString ToolsRuntime::trayText() const
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    if (_pomodoro.status == PomodoroStatus::Running) {
        return QStringLiteral("UseTrack · %1 %2")
            .arg(phaseLabel(_pomodoro.phase), formatCompact(_pomodoro.remainingMsAt(nowMs)));
    }
    if (_pomodoro.status == PomodoroStatus::Paused) {
        return QStringLiteral("UseTrack · %1 已暂停").arg(phaseLabel(_pomodoro.phase));
    }
    if (_timer.status == TimerStatus::Running) {
        const qint64 shown = _timer.mode == TimerMode::Countdown ? _timer.remainingMsAt(nowMs)
                                                                 : _timer.elapsedMsAt(nowMs);
        return QStringLiteral("UseTrack · %1 %2")
            .arg(_timer.mode == TimerMode::Countdown ? QStringLiteral("倒计时")
                                                     : QStringLiteral("秒表"),
                 formatCompact(shown));
    }
    if (_timer.status == TimerStatus::Paused) {
        return QStringLiteral("UseTrack · 计时器已暂停");
    }

    // 没有在跑的东西时，退而提示最近一个待触发的提醒。
    const ToolReminder* next = nullptr;
    for (const ToolReminder& reminder : _reminders) {
        if (!reminder.isPending()) {
            continue;
        }
        if (next == nullptr || reminder.scheduledAtMs < next->scheduledAtMs) {
            next = &reminder;
        }
    }
    if (next != nullptr) {
        return QStringLiteral("UseTrack · 下次提醒 %1").arg(formatClockTime(next->scheduledAtMs));
    }
    return QString();
}

qreal ToolsRuntime::trayProgress() const
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    if (_pomodoro.status == PomodoroStatus::Running) {
        const qint64 total = std::max<qint64>(1, _pomodoro.phaseDurationMs());
        return 1.0 - static_cast<qreal>(_pomodoro.remainingMsAt(nowMs)) / static_cast<qreal>(total);
    }
    if (_timer.status == TimerStatus::Running && _timer.mode == TimerMode::Countdown) {
        const qint64 total = std::max<qint64>(1, _timer.durationMs);
        return 1.0 - static_cast<qreal>(_timer.remainingMsAt(nowMs)) / static_cast<qreal>(total);
    }
    return -1.0;
}

// —— 提醒 ——

data::Status ToolsRuntime::addReminder(const QString& label, qint64 delayMs)
{
    if (_database == nullptr || !_database->isOpen()) {
        return data::Status::failure(QStringLiteral("数据库未打开"));
    }
    const qint64 safeDelay = std::max<qint64>(kMinuteMs, delayMs);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    ToolReminder reminder;
    reminder.id = newReminderId();
    reminder.label = label.trimmed();
    reminder.scheduledAtMs = nowMs + safeDelay;
    reminder.createdAtMs = nowMs;
    reminder.status = ReminderStatus::Scheduled;

    // 先写库再进内存：库里写不进去就不给界面加行。
    const data::Status saved = persistReminder(reminder);
    if (!saved.ok()) {
        return saved;
    }
    _reminders.append(reminder);
    updateTickTimer();
    emit changed();
    return data::Status::success();
}

data::Status ToolsRuntime::cancelReminder(const QString& id)
{
    for (ToolReminder& reminder : _reminders) {
        if (reminder.id != id) {
            continue;
        }
        reminder.status = ReminderStatus::Cancelled;
        const data::Status saved = persistReminder(reminder);
        if (!saved.ok()) {
            reminder.status = ReminderStatus::Scheduled;
            return saved;
        }
        updateTickTimer();
        emit changed();
        return data::Status::success();
    }
    return data::Status::failure(QStringLiteral("提醒不存在"));
}

// —— 活动提醒规则 ——

data::Status ToolsRuntime::addActivityRule(const QString& targetKind,
                                           const QString& appName,
                                           const QString& exeName,
                                           const QString& categoryId,
                                           const QString& label,
                                           qint64 limitMs,
                                           const QString& message)
{
    if (_database == nullptr || !_database->isOpen()) {
        return data::Status::failure(QStringLiteral("数据库未打开"));
    }
    if (targetKind != QLatin1String("app") && targetKind != QLatin1String("category")) {
        return data::Status::failure(QStringLiteral("提醒目标类型非法"));
    }
    if (targetKind == QLatin1String("app") && appName.trimmed().isEmpty()) {
        return data::Status::failure(QStringLiteral("请先选择要提醒的应用"));
    }
    if (targetKind == QLatin1String("category") && categoryId.trimmed().isEmpty()) {
        return data::Status::failure(QStringLiteral("请先选择要提醒的分类"));
    }
    // 1 分钟～24 小时。
    if (limitMs < kMinuteMs || limitMs > 24 * 60 * kMinuteMs) {
        return data::Status::failure(QStringLiteral("时长上限要在 1 分钟到 24 小时之间"));
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    data::ToolActivityReminderRule rule;
    rule.targetKind = targetKind;
    rule.appName = targetKind == QLatin1String("app") ? appName.trimmed() : QString();
    rule.exeName = targetKind == QLatin1String("app") ? exeName.trimmed() : QString();
    rule.categoryId = targetKind == QLatin1String("category") ? categoryId.trimmed() : QString();
    rule.labelSnapshot = label.trimmed();
    rule.limitMs = limitMs;
    rule.message = message.trimmed();
    rule.createdAtMs = nowMs;
    rule.updatedAtMs = nowMs;

    data::ToolsRepository repository(*_database);
    const data::Result<data::ToolActivityReminderRule> created = repository.createActivityRule(rule);
    if (!created.ok()) {
        return data::Status::failure(created.error);
    }
    reloadActivityRules();
    emit changed();
    return data::Status::success();
}

data::Status ToolsRuntime::disableActivityRule(qint64 ruleId)
{
    if (_database == nullptr || !_database->isOpen()) {
        return data::Status::failure(QStringLiteral("数据库未打开"));
    }
    data::ToolsRepository repository(*_database);
    const data::Status disabled =
        repository.disableActivityRule(ruleId, QDateTime::currentMSecsSinceEpoch());
    if (!disabled.ok()) {
        return disabled;
    }
    reloadActivityRules();
    emit changed();
    return data::Status::success();
}

// —— 计时器 ——

data::Status ToolsRuntime::startTimer(TimerMode mode, qint64 durationMs, const QString& label)
{
    if (_database == nullptr || !_database->isOpen()) {
        return data::Status::failure(QStringLiteral("数据库未打开"));
    }
    if (_timer.isActive()) {
        return data::Status::failure(QStringLiteral("计时器正在使用中，先重置"));
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    _timer = ToolTimer();
    _timer.mode = mode;
    _timer.label = label.trimmed();
    _timer.durationMs = mode == TimerMode::Countdown ? std::max<qint64>(1000, durationMs) : 0;
    _timer.accumulatedMs = 0;
    _timer.startedAtMs = nowMs;
    _timer.status = TimerStatus::Running;
    _laps.clear();
    _lastTimerPersistMs = nowMs;

    data::ToolsRepository repository(*_database);
    const data::Status saved = repository.saveTimer(toTimerRow(_timer));
    if (!saved.ok()) {
        _timer = ToolTimer();
        return saved;
    }
    repository.clearLaps();
    updateTickTimer();
    emit changed();
    return data::Status::success();
}

data::Status ToolsRuntime::pauseTimer()
{
    if (_timer.status != TimerStatus::Running) {
        return data::Status::failure(QStringLiteral("计时器没有在跑"));
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    _timer.accumulatedMs = _timer.elapsedMsAt(nowMs);
    _timer.startedAtMs = 0;
    _timer.status = TimerStatus::Paused;

    const data::Status saved = persistTimer();
    if (!saved.ok()) {
        // 回滚：accumulatedMs 已经被折成当前值，起点拨到现在就是「从这一刻继续跑」。
        _timer.status = TimerStatus::Running;
        _timer.startedAtMs = nowMs;
        return saved;
    }
    updateTickTimer();
    emit changed();
    return data::Status::success();
}

data::Status ToolsRuntime::resumeTimer()
{
    if (_timer.status != TimerStatus::Paused) {
        return data::Status::failure(QStringLiteral("计时器没有暂停"));
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    _timer.startedAtMs = nowMs;
    _timer.status = TimerStatus::Running;

    const data::Status saved = persistTimer();
    if (!saved.ok()) {
        _timer.status = TimerStatus::Paused;
        _timer.startedAtMs = 0;
        return saved;
    }
    updateTickTimer();
    emit changed();
    return data::Status::success();
}

data::Status ToolsRuntime::resetTimer()
{
    const TimerMode mode = _timer.mode;
    const qint64 durationMs = _timer.durationMs;
    const QString label = _timer.label;

    _timer = ToolTimer();
    _timer.mode = mode;
    _timer.durationMs = durationMs;
    _timer.label = label;
    _laps.clear();

    data::ToolsRepository repository(*_database);
    const data::Status saved = persistTimer();
    repository.clearLaps();
    updateTickTimer();
    emit changed();
    return saved;
}

data::Status ToolsRuntime::addLap()
{
    if (_timer.status != TimerStatus::Running || _timer.mode != TimerMode::Stopwatch) {
        return data::Status::failure(QStringLiteral("只有运行中的秒表可以计次"));
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsed = _timer.elapsedMsAt(nowMs);

    ToolLap lap;
    lap.index = static_cast<int>(_laps.size()) + 1;
    lap.endedAtMs = nowMs;
    lap.durationMs = elapsed;
    lap.startedAtMs = nowMs - elapsed;

    // 圈次先落库：跨重启要还在。内存里再补一份给界面。
    if (_database != nullptr && _database->isOpen()) {
        data::ToolsRepository repository(*_database);
        data::ToolLapRow row;
        row.index = lap.index;
        row.startedAtMs = lap.startedAtMs;
        row.endedAtMs = lap.endedAtMs;
        row.durationMs = lap.durationMs;
        const data::Status saved = repository.appendLap(row);
        if (!saved.ok()) {
            return saved;
        }
    }
    _laps.append(lap);
    emit changed();
    return data::Status::success();
}

// —— 番茄钟 ——

data::Status ToolsRuntime::startPomodoro()
{
    if (_database == nullptr || !_database->isOpen()) {
        return data::Status::failure(QStringLiteral("数据库未打开"));
    }
    if (_pomodoro.status == PomodoroStatus::Running || _pomodoro.status == PomodoroStatus::Paused) {
        return data::Status::failure(QStringLiteral("番茄钟还在进行中，先重置"));
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    _pomodoro.phase = PomodoroPhase::Focus;
    _pomodoro.status = PomodoroStatus::Running;
    _pomodoro.cycleIndex = _pomodoro.completedFocusCount % std::max(1, _pomodoro.longBreakEvery) + 1;
    _pomodoro.focusMs = std::max<qint64>(1000, static_cast<qint64>(_settings.pomodoroFocusMinutes) * kMinuteMs);
    _pomodoro.shortBreakMs =
        std::max<qint64>(1000, static_cast<qint64>(_settings.pomodoroShortBreakMinutes) * kMinuteMs);
    _pomodoro.longBreakMs =
        std::max<qint64>(1000, static_cast<qint64>(_settings.pomodoroLongBreakMinutes) * kMinuteMs);
    _pomodoro.longBreakEvery = std::max(1, _settings.pomodoroLongBreakEvery);
    _pomodoro.phaseStartedAtMs = nowMs;
    _pomodoro.phaseRemainingAtStartMs = _pomodoro.phaseDurationMs();
    _pomodoro.statDateKey = todayDateKey();
    loadDailyStat();

    const data::Status saved = persistPomodoro();
    if (!saved.ok()) {
        _pomodoro.status = PomodoroStatus::Idle;
        return saved;
    }
    updateTickTimer();
    emit changed();
    return data::Status::success();
}

data::Status ToolsRuntime::pausePomodoro()
{
    if (_pomodoro.status != PomodoroStatus::Running) {
        return data::Status::failure(QStringLiteral("番茄钟没有在跑"));
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    _pomodoro.phaseRemainingAtStartMs = _pomodoro.remainingMsAt(nowMs);
    _pomodoro.phaseStartedAtMs = 0;
    _pomodoro.status = PomodoroStatus::Paused;

    const data::Status saved = persistPomodoro();
    if (!saved.ok()) {
        _pomodoro.status = PomodoroStatus::Running;
        return saved;
    }
    updateTickTimer();
    emit changed();
    return data::Status::success();
}

data::Status ToolsRuntime::resumePomodoro()
{
    if (_pomodoro.status != PomodoroStatus::Paused) {
        return data::Status::failure(QStringLiteral("番茄钟没有暂停"));
    }
    _pomodoro.phaseStartedAtMs = QDateTime::currentMSecsSinceEpoch();
    _pomodoro.status = PomodoroStatus::Running;

    const data::Status saved = persistPomodoro();
    if (!saved.ok()) {
        _pomodoro.status = PomodoroStatus::Paused;
        _pomodoro.phaseStartedAtMs = 0;
        return saved;
    }
    updateTickTimer();
    emit changed();
    return data::Status::success();
}

data::Status ToolsRuntime::skipPomodoroPhase()
{
    if (_pomodoro.status == PomodoroStatus::Idle
        || _pomodoro.status == PomodoroStatus::Completed) {
        return data::Status::failure(QStringLiteral("番茄钟没有在进行"));
    }
    // 手动跳过不算完成：不推进今日完成数，下一阶段也停在暂停态。
    _pomodoro.advanceAfterCompletion();
    _pomodoro.status = PomodoroStatus::Paused;
    _pomodoro.phaseStartedAtMs = 0;

    const data::Status saved = persistPomodoro();
    updateTickTimer();
    emit changed();
    return saved;
}

data::Status ToolsRuntime::resetPomodoro()
{
    const int completedFocusCount = _pomodoro.completedFocusCount;

    _pomodoro = ToolPomodoro();
    _pomodoro.completedFocusCount = 0;
    _pomodoro.statDateKey = todayDateKey();
    _pomodoro.focusMs = std::max<qint64>(1000, static_cast<qint64>(_settings.pomodoroFocusMinutes) * kMinuteMs);
    _pomodoro.shortBreakMs =
        std::max<qint64>(1000, static_cast<qint64>(_settings.pomodoroShortBreakMinutes) * kMinuteMs);
    _pomodoro.longBreakMs =
        std::max<qint64>(1000, static_cast<qint64>(_settings.pomodoroLongBreakMinutes) * kMinuteMs);
    _pomodoro.longBreakEvery = std::max(1, _settings.pomodoroLongBreakEvery);
    _pomodoro.phaseRemainingAtStartMs = _pomodoro.phaseDurationMs();
    loadDailyStat();

    const data::Status saved = persistPomodoro();
    updateTickTimer();
    emit changed();
    Q_UNUSED(completedFocusCount)
    return saved;
}

// —— 设置 ——

data::Status ToolsRuntime::applySettings(const ToolsSettings& settings)
{
    ToolsSettings normalized = settings;
    normalized.defaultCountdownMinutes = std::clamp(normalized.defaultCountdownMinutes, 1, 180);
    normalized.pomodoroFocusMinutes = std::clamp(normalized.pomodoroFocusMinutes, 1, 180);
    normalized.pomodoroShortBreakMinutes = std::clamp(normalized.pomodoroShortBreakMinutes, 1, 60);
    normalized.pomodoroLongBreakMinutes = std::clamp(normalized.pomodoroLongBreakMinutes, 1, 120);
    normalized.pomodoroLongBreakEvery = std::clamp(normalized.pomodoroLongBreakEvery, 2, 12);

    _settings = normalized;

    // 没在跑的时候顺手把番茄钟的时长带上：设置改了，空闲态的显示也该跟着改。
    if (_pomodoro.status == PomodoroStatus::Idle || _pomodoro.status == PomodoroStatus::Completed) {
        _pomodoro.focusMs = static_cast<qint64>(normalized.pomodoroFocusMinutes) * kMinuteMs;
        _pomodoro.shortBreakMs = static_cast<qint64>(normalized.pomodoroShortBreakMinutes) * kMinuteMs;
        _pomodoro.longBreakMs = static_cast<qint64>(normalized.pomodoroLongBreakMinutes) * kMinuteMs;
        _pomodoro.longBreakEvery = normalized.pomodoroLongBreakEvery;
        _pomodoro.phaseRemainingAtStartMs = _pomodoro.phaseDurationMs();
        const data::Status savedPomodoro = persistPomodoro();
        if (!savedPomodoro.ok()) {
            return savedPomodoro;
        }
    }

    if (_database == nullptr || !_database->isOpen()) {
        return data::Status::failure(QStringLiteral("数据库未打开"));
    }
    // 设置五项仍是轻量键值，写回 settings 表的 tools.settings 键。
    QJsonObject object;
    object.insert(QStringLiteral("defaultCountdownMinutes"), _settings.defaultCountdownMinutes);
    object.insert(QStringLiteral("pomodoroFocusMinutes"), _settings.pomodoroFocusMinutes);
    object.insert(QStringLiteral("pomodoroShortBreakMinutes"), _settings.pomodoroShortBreakMinutes);
    object.insert(QStringLiteral("pomodoroLongBreakMinutes"), _settings.pomodoroLongBreakMinutes);
    object.insert(QStringLiteral("pomodoroLongBreakEvery"), _settings.pomodoroLongBreakEvery);
    const QString text =
        QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
    data::SettingsRepository settingsRepo(*_database);
    const data::Status saved = settingsRepo.setValue(QStringLiteral("tools.settings"), text);
    if (saved.ok()) {
        emit changed();
    }
    return saved;
}

// —— 落盘 ——

data::Status ToolsRuntime::persistTimer()
{
    if (_database == nullptr || !_database->isOpen()) {
        return data::Status::failure(QStringLiteral("数据库未打开"));
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    // 落盘前把「运行中的这一段」折进 accumulatedMs：这样进程被强杀，
    // 下次读到的也是最后一次落库的时长，而不是一段没有尽头的 startedAt。
    if (_timer.status == TimerStatus::Running) {
        _timer.foldElapsed(nowMs);
    }
    _lastTimerPersistMs = nowMs;
    data::ToolsRepository repository(*_database);
    return repository.saveTimer(toTimerRow(_timer));
}

data::Status ToolsRuntime::persistPomodoro()
{
    if (_database == nullptr || !_database->isOpen()) {
        return data::Status::failure(QStringLiteral("数据库未打开"));
    }
    data::ToolsRepository repository(*_database);
    return repository.savePomodoro(toPomodoroRow(_pomodoro));
}

data::Status ToolsRuntime::persistReminder(const ToolReminder& reminder)
{
    if (_database == nullptr || !_database->isOpen()) {
        return data::Status::failure(QStringLiteral("数据库未打开"));
    }
    data::ToolReminderRow row;
    row.id = reminder.id;
    row.label = reminder.label;
    row.scheduledAtMs = reminder.scheduledAtMs;
    row.createdAtMs = reminder.createdAtMs;
    switch (reminder.status) {
    case ReminderStatus::Fired:
        row.status = data::ToolReminderStatus::Fired;
        break;
    case ReminderStatus::Cancelled:
        row.status = data::ToolReminderStatus::Cancelled;
        break;
    case ReminderStatus::Scheduled:
        row.status = data::ToolReminderStatus::Scheduled;
        break;
    }
    if (reminder.firedAtMs > 0) {
        row.firedAtMs = reminder.firedAtMs;
    }
    data::ToolsRepository repository(*_database);
    return repository.upsertReminder(row);
}

} // namespace patina::ui
