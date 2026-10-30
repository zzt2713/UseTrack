#ifndef PATINA_UI_TOOLS_TOOLS_PAGE_H
#define PATINA_UI_TOOLS_TOOLS_PAGE_H

#include "data/repositories.h"
#include "ui/tools/tools_types.h"

#include <QVector>
#include <QWidget>

class ElaComboBox;
class ElaLineEdit;
class ElaPushButton;
class ElaText;

class QLabel;
class QTimer;
class QVBoxLayout;

namespace patina::ui
{

class CountdownRingWidget;
class ToolsRuntime;
struct ToolAlert;

// 工具页：一次性提醒 / 计时器 / 番茄钟，三栏并列（方案 A）。
//
// 状态源是 ToolsRuntime 单例，不在本页：页面切走、窗口最小化到托盘时
// 提醒与计时都必须继续跑，所以本页只是它的一层视图。
//
// 页面不可见时把圈次列表等控件数据清掉，只留运行时状态。
class ToolsPage : public QWidget
{
    Q_OBJECT

public:
    explicit ToolsPage(data::Database* database, QWidget* parent = nullptr);
    ~ToolsPage() override;

    void setActive(bool active);

    // 到点提示统一由主窗口弹：页面可能根本没打开。
    // 主窗口把 ToolsRuntime 的 alertRaised 转到这里没有意义，
    // 所以提示弹窗放在 MainWindow，本页只负责按钮触发的即时反馈。

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void buildUi();
    QWidget* buildReminderPanel();
    QWidget* buildTimerPanel();
    QWidget* buildPomodoroPanel();

    void refreshAll();
    void refreshReminders();
    void refreshActivityRules();
    void refreshTimer();
    void refreshPomodoro();
    void refreshTrayHint();

    // 按当前「按应用 / 按分类」选择重填目标下拉。
    // 候选来自库：应用取分类管理页那批应用，分类取生效注册表。
    void reloadActivityTargets();

    // 下拉框的文字色跟着明暗主题走（Ela 没暴露文字色接口）。
    // 页面建好后调一次，主题切换信号里再调一次。
    void applyComboTheme(bool dark);

    // 说明文字、计数、状态那几行的次要色。这些 QLabel/ElaText 只能靠样式表上色，
    // 写死十六进制的话切到深色主题就变成压在深底上的浅灰，读不出来。
    void applyThemeColors(bool dark);

    // 把标签登记进 _secondaryLabels 并立刻按当前主题上色，返回同一个指针，
    // 方便在 makeLabel(...) 外面套一层。
    QLabel* trackSecondary(QLabel* label);

    // 只刷「会随时间自己变的」那几处文字：秒表读数、倒计时、番茄钟环。
    // 每 1 秒由 _refreshTimer 调一次，不重建列表，避免每秒重建控件。
    void tickDisplay();

    void onAddReminder();
    void onCancelReminder(const QString& id);
    void onAddActivityRule();
    void onDisableActivityRule(qint64 ruleId);
    void onActivityTargetKindChanged();
    void onTimerPrimary();
    void onTimerSecondary();
    void onTimerTertiary();
    void onPomodoroPrimary();
    void onPomodoroSecondary();
    void onPomodoroTertiary();

    ToolsRuntime& runtime();

    // 目标下拉的「这是什么」标记：data 是 "app" / "category"。
    data::Database* _database = nullptr;

    bool _active = false;
    bool _updating = false;

    // 秒表/倒计时/番茄钟的读数每 1 秒自己刷一次。
    // 不依赖 ToolsRuntime::changed：那条信号只在「到点」时才发，
    // 光靠它跑着的秒表数字会一直停在开始时那一刻。
    QTimer* _refreshTimer = nullptr;
    // 上一次重建提醒列表时的分钟序号：只在跨分钟时重建，避免每秒抽走取消按钮。
    qint64 _lastReminderMinute = 0;

    // 提醒
    ElaLineEdit* _reminderLabelEdit = nullptr;
    ElaComboBox* _reminderDelayBox = nullptr;
    ElaLineEdit* _reminderCustomMinutesEdit = nullptr;
    ElaPushButton* _reminderAddButton = nullptr;
    QVBoxLayout* _reminderListLayout = nullptr;
    QLabel* _reminderEmptyLabel = nullptr;
    QLabel* _reminderCountLabel = nullptr;
    QLabel* _reminderNoteLabel = nullptr;

    // 每日时长提醒（按应用 / 按分类的每日累计上限）
    ElaComboBox* _activityKindBox = nullptr;
    ElaComboBox* _activityTargetBox = nullptr;
    ElaLineEdit* _activityLimitMinutesEdit = nullptr;
    ElaLineEdit* _activityMessageEdit = nullptr;
    ElaPushButton* _activityAddButton = nullptr;
    QVBoxLayout* _activityRuleListLayout = nullptr;
    QLabel* _activityEmptyLabel = nullptr;
    QLabel* _activityCountLabel = nullptr;

    // 建页面时一次性创建、之后只改文字的那些次要色标签。
    // 列表行里的动态标签不在这里（每次重建都会换新的，建的时候就上色）。
    QVector<QLabel*> _secondaryLabels;
    // 计时器
    ElaLineEdit* _timerLabelEdit = nullptr;
    ElaComboBox* _timerModeBox = nullptr;
    ElaComboBox* _timerMinutesBox = nullptr;
    ElaLineEdit* _timerCustomMinutesEdit = nullptr;
    QLabel* _timerDisplayLabel = nullptr;
    QLabel* _timerStateLabel = nullptr;
    ElaPushButton* _timerPrimaryButton = nullptr;
    ElaPushButton* _timerSecondaryButton = nullptr;
    ElaPushButton* _timerTertiaryButton = nullptr;
    QVBoxLayout* _lapListLayout = nullptr;
    QLabel* _lapEmptyLabel = nullptr;

    // 番茄钟
    CountdownRingWidget* _pomodoroRing = nullptr;
    QLabel* _pomodoroPhaseLabel = nullptr;
    QLabel* _pomodoroCycleLabel = nullptr;
    QLabel* _pomodoroTodayLabel = nullptr;
    ElaPushButton* _pomodoroPrimaryButton = nullptr;
    ElaPushButton* _pomodoroSecondaryButton = nullptr;
    ElaPushButton* _pomodoroTertiaryButton = nullptr;
    QWidget* _settingsBox = nullptr;
    ElaComboBox* _focusMinutesBox = nullptr;
    ElaComboBox* _shortBreakBox = nullptr;
    ElaComboBox* _longBreakBox = nullptr;
    ElaComboBox* _longEveryBox = nullptr;

    // 托盘提示
    ElaText* _trayHintLabel = nullptr;
};

} // namespace patina::ui

#endif // PATINA_UI_TOOLS_TOOLS_PAGE_H
