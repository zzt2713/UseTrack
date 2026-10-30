#ifndef PATINA_UI_TOOLS_TOOLS_RUNTIME_H
#define PATINA_UI_TOOLS_TOOLS_RUNTIME_H

#include "data/repositories.h"
#include "ui/tools/tools_types.h"

#include <QObject>
#include <QString>
#include <QVector>

class QTimer;

namespace patina::ui
{

// 到点提示：由运行时产生，交给外面弹（托盘气泡 / 对话框）。
// 运行时不自己弹窗——提醒到点时页面可能根本没打开。
struct ToolAlert
{
    enum class Kind
    {
        Reminder,
        Countdown,
        Pomodoro,
        Activity,
    };

    Kind kind = Kind::Reminder;
    QString title;
    QString body;
    qint64 occurredAtMs = 0;
};

// 工具页的唯一状态源。
//
// 为什么是单例而不是页面成员：状态要在页面销毁后继续存在。
// 用户切到别的页、把窗口最小化到托盘，提醒和计时都必须照跑；
// 托盘提示文字与进度环也要能随时读到当前状态。
//
// 全部读写都在 GUI 线程，内部不加锁。落库经 data/ 的 ToolsRepository
// （ui/ 不写 SQL）。
class ToolsRuntime : public QObject
{
    Q_OBJECT

public:
    static ToolsRuntime& instance();

    // 绑定数据库并做一次启动自愈（重启后的状态恢复规则就落在这里）。
    // 重复调用同一 database 是无操作；换成另一个 database 会重新加载。
    void attach(data::Database* database);
    bool isReady() const { return _database != nullptr; }

    const QVector<ToolReminder>& reminders() const { return _reminders; }
    const ToolTimer& timer() const { return _timer; }
    const ToolPomodoro& pomodoro() const { return _pomodoro; }
    const ToolsSettings& settings() const { return _settings; }
    const QVector<ToolLap>& laps() const { return _laps; }
    // 活动提醒规则（按应用/分类的每日时长上限）。停用的不列。
    const QVector<data::ToolActivityReminderRule>& activityRules() const { return _activityRules; }

    // 页面可见时保持秒级刷新（显示要每秒跳）；不可见时只在有活干时才跑表。
    void setUiVisible(bool visible);

    // —— 提醒 ——
    data::Status addReminder(const QString& label, qint64 delayMs);
    data::Status cancelReminder(const QString& id);

    // —— 活动提醒规则 ——
    // targetKind 为 "app" 或 "category"；app 规则用 appName(+exeName)，
    // category 规则用 categoryId。limitMs 是每日累计上限。
    data::Status addActivityRule(const QString& targetKind,
                                 const QString& appName,
                                 const QString& exeName,
                                 const QString& categoryId,
                                 const QString& label,
                                 qint64 limitMs,
                                 const QString& message);
    data::Status disableActivityRule(qint64 ruleId);

    // —— 计时器 ——
    data::Status startTimer(TimerMode mode, qint64 durationMs, const QString& label);
    data::Status pauseTimer();
    data::Status resumeTimer();
    data::Status resetTimer();
    data::Status addLap();

    // —— 番茄钟 ——
    data::Status startPomodoro();
    data::Status pausePomodoro();
    data::Status resumePomodoro();
    data::Status skipPomodoroPhase();
    data::Status resetPomodoro();

    // —— 设置 ——
    data::Status applySettings(const ToolsSettings& settings);

    // —— 托盘 ——
    // 有正在跑的东西（含待触发的提醒）时为真，托盘图标据此换成活跃态。
    bool hasActiveWork() const;
    // 托盘悬停文字；没在跑任何东西时返回空串。
    QString trayText() const;
    // 0..1 的进度；没有可度量进度的活动时返回 -1。
    qreal trayProgress() const;

signals:
    // 任何状态变化（供页面重绘）。
    void changed();
    // 到点提示。接收方负责真正弹出来。
    void alertRaised(const patina::ui::ToolAlert& alert);

private:
    explicit ToolsRuntime(QObject* parent = nullptr);

    void reloadFromStore();
    void recoverAfterStartup(qint64 nowMs);
    void onTick();
    void updateTickTimer();
    data::Status persistTimer();
    data::Status persistPomodoro();
    data::Status persistReminder(const ToolReminder& reminder);
    // 每日统计的读写改走 tool_daily_stats 表：内存里仍以
    // _pomodoro.statDateKey + completedFocusToday 展示。
    bool loadDailyStat();
    bool saveDailyStat();
    // 把启用中的活动提醒规则从库里读进 _activityRules。
    void reloadActivityRules();
    // 跨日时把「今日完成数」清零。返回是否发生了跨日。
    bool rolloverDailyStat(qint64 nowMs);
    bool hasPendingReminder() const;
    bool hasRunningWork() const;

    data::Database* _database = nullptr;
    QTimer* _tickTimer = nullptr;
    bool _loaded = false;
    bool _uiVisible = false;
    qint64 _lastTimerPersistMs = 0;

    QVector<ToolReminder> _reminders;
    QVector<ToolLap> _laps;
    // 活动提醒规则。改完规则要重载一次（规则数量个位数，代价可忽略）。
    QVector<data::ToolActivityReminderRule> _activityRules;
    ToolTimer _timer;
    ToolPomodoro _pomodoro;
    ToolsSettings _settings;
};

} // namespace patina::ui

#endif // PATINA_UI_TOOLS_TOOLS_RUNTIME_H
