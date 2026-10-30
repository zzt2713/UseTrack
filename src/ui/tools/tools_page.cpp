#include "ui/tools/tools_page.h"

#include "ui/tools/countdown_ring_widget.h"
#include "ui/tools/tools_runtime.h"

#include "ElaComboBox.h"
#include "ElaFlowLayout.h"
#include "ElaLineEdit.h"
#include "ElaPushButton.h"
#include "ElaScrollPageArea.h"
#include "ElaText.h"
#include "ElaTheme.h"

#include "shared/theme_styles.h"

#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>

#include <algorithm>

namespace patina::ui
{

namespace
{

constexpr int kPanelGap = 14;
constexpr qint64 kMinuteMs = 60 * 1000;
// 读数刷新粒度。1 秒足够看清秒表，再密就纯属浪费重绘。
constexpr int kDisplayIntervalMs = 1000;
// 自定义时长允许的分钟范围，和一次建两小时的会议对得上就够。
constexpr int kMinCustomMinutes = 1;
constexpr int kMaxCustomMinutes = 600;

// 提醒的延时档位。一次性提醒不需要日历那种精度，够用就好。
struct DelayOption
{
    const char* text;
    qint64 delayMs;
};

const DelayOption kDelayOptions[] = {
    {"5 分钟后", 5 * kMinuteMs},
    {"15 分钟后", 15 * kMinuteMs},
    {"30 分钟后", 30 * kMinuteMs},
    {"1 小时后", 60 * kMinuteMs},
    {"2 小时后", 120 * kMinuteMs},
};

ElaScrollPageArea* makeCard(QWidget* parent)
{
    auto* card = new ElaScrollPageArea(parent);
    // ElaScrollPageArea 构造里写死了 setFixedHeight(75)，当容器使必须先解开。
    card->setMinimumHeight(0);
    card->setMaximumHeight(QWIDGETSIZE_MAX);
    // 三栏要 1:1:1 等分。光 setMinimumWidth(0) 没用：布局取的是 minimumSizeHint
    // （各栏里最宽的那个控件撑出来的），三列最小值之和一旦超过可视宽度，
    // ElaScrollPage 的横向滚动条虽然被 AlwaysOff 藏住，但横向范围不为零，
    // 拖拽/滚轮照样能把内容推着左右走。把水平策略置为 Ignored 才是 CSS 里
    // min-width:0 的等价语义：列宽由布局等分决定，不再被内容最小宽撑开。
    card->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    card->setBorderRadius(14);
    return card;
}

ElaText* makeLabel(const QString& text, int pixelSize, QWidget* parent, bool strong = false)
{
    auto* label = new ElaText(text, pixelSize, parent);
    label->setWordWrap(false);
    if (strong) {
        label->setTextStyle(ElaTextType::BodyStrong);
        // setTextStyle 会改字号，必须再复位一次。
        label->setTextPixelSize(pixelSize);
    }
    return label;
}

// 解析「自定义分钟」输入框。
//   留空      → 返回 0，调用方回落到下拉档位
//   合法正整数 → 返回分钟数（夹在允许范围内）
//   填了但不合法 → 返回 -1
int parseCustomMinutes(const QString& text, int* clampedOut)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return 0;
    }
    bool ok = false;
    const int value = trimmed.toInt(&ok);
    if (!ok || value <= 0) {
        return -1;
    }
    const int clamped = std::clamp(value, kMinCustomMinutes, kMaxCustomMinutes);
    if (clampedOut != nullptr) {
        *clampedOut = clamped;
    }
    return clamped;
}

QString formatRemaining(qint64 ms)
{
    const qint64 totalSeconds = std::max<qint64>(0, ms) / 1000;
    if (totalSeconds >= 3600) {
        const qint64 hours = totalSeconds / 3600;
        const qint64 minutes = (totalSeconds % 3600) / 60;
        return hours > 0 ? QStringLiteral("%1 小时 %2 分").arg(hours).arg(minutes)
                         : QStringLiteral("%1 分").arg(minutes);
    }
    const qint64 minutes = totalSeconds / 60;
    if (minutes >= 1) {
        return QStringLiteral("%1 分钟后").arg(minutes);
    }
    return QStringLiteral("%1 秒后").arg(totalSeconds);
}

QString phaseText(PomodoroPhase phase)
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

void clearLayout(QVBoxLayout* layout)
{
    // 布局里可能有 label、行控件，还有末尾的 stretch（spacer，没有 widget）。
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            // 先 hide 再 deleteLater：这里常在自己的槽里被调用，
            // 直接 delete 可能删掉正在处理事件的那个控件。
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

} // namespace

ToolsPage::ToolsPage(data::Database* database, QWidget* parent)
    : QWidget(parent)
    , _database(database)
{
    // 状态源是 ToolsRuntime 单例，页面切走之后计时与提醒还要继续跑。
    // 但提醒目标候选要查库（应用列表 / 分类注册表），所以留一份库指针。
    buildUi();

    // 默认倒计时时长从设置里取；找不到对应档位就留在第一项。
    {
        const ToolsSettings settings = runtime().settings();
        const int index = _timerMinutesBox->findData(settings.defaultCountdownMinutes);
        if (index >= 0) {
            _timerMinutesBox->setCurrentIndex(index);
        }
    }

    connect(&runtime(), &ToolsRuntime::changed, this, [this]() {
        if (_active) {
            refreshAll();
        }
    });

    // 读数自己的心跳：秒表跑起来以后每一秒都要变数，
    // 而 ToolsRuntime::changed 只在到点时才发，靠它会一直停在开始时那一格。
    _refreshTimer = new QTimer(this);
    _refreshTimer->setInterval(kDisplayIntervalMs);
    _refreshTimer->setTimerType(Qt::CoarseTimer);
    connect(_refreshTimer, &QTimer::timeout, this, &ToolsPage::tickDisplay);

    connect(eTheme, &ElaTheme::themeModeChanged, this,
            [this](ElaThemeType::ThemeMode mode) {
                _pomodoroRing->update();
                const bool dark = mode == ElaThemeType::ThemeMode::Dark;
                applyComboTheme(dark);
                applyThemeColors(dark);
            });

    // 建好之后先按当前主题铺一次：不然深色主题下第一个 combo 就已经是黑字了。
    const bool initialDark = eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark;
    applyComboTheme(initialDark);
    applyThemeColors(initialDark);

    // 目标候选在页面建好后装一次，切「按应用 / 按分类」时再重装。
    reloadActivityTargets();
}

QLabel* ToolsPage::trackSecondary(QLabel* label)
{
    if (label == nullptr) {
        return label;
    }
    label->setStyleSheet(
        shared::secondaryTextStyle(eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark));
    _secondaryLabels.append(label);
    return label;
}

void ToolsPage::applyThemeColors(bool dark)
{
    // 建页面时创建、之后只改文字的那几行。列表行里的标签每次重建都会换新的，
    // 它们在自己的创建处上色。
    const QString secondary = shared::secondaryTextStyle(dark);
    for (QLabel* label : _secondaryLabels) {
        if (label != nullptr) {
            label->setStyleSheet(secondary);
        }
    }
}

void ToolsPage::applyComboTheme(bool dark)
{
    // Ela 的 combo 不暴露文字色接口，只能自己补一条 color 规则。
    // 这里一口气覆盖页面上全部七个（延时/模式/时长档/四个时长设置）。
    shared::applyComboBoxTheme(QVector<QComboBox*>{_reminderDelayBox,
                                                   _activityKindBox,
                                                   _activityTargetBox,
                                                   _timerModeBox,
                                                   _timerMinutesBox,
                                                   _focusMinutesBox,
                                                   _shortBreakBox,
                                                   _longBreakBox,
                                                   _longEveryBox},
                               dark);
}

ToolsPage::~ToolsPage() = default;

ToolsRuntime& ToolsPage::runtime()
{
    return ToolsRuntime::instance();
}

void ToolsPage::buildUi()
{
    // 外层 ElaScrollPage 已经提供滚动容器，这里只做布局。
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 16, 24, 24);
    layout->setSpacing(14);

    // —— 页头 ——
    auto* header = new QWidget(this);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(12);

    auto* titleBox = new QWidget(header);
    auto* titleLayout = new QVBoxLayout(titleBox);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(2);
    titleLayout->addWidget(makeLabel(tr("工具"), 19, titleBox, true));
    titleLayout->addWidget(makeLabel(tr("提醒、计时与番茄钟都只存在本机"), 12, titleBox));
    headerLayout->addWidget(titleBox);
    headerLayout->addStretch();

    _trayHintLabel = makeLabel(QString(), 11, header);
    trackSecondary(_trayHintLabel);
    headerLayout->addWidget(_trayHintLabel, 0, Qt::AlignVCenter);

    layout->addWidget(header);

    // —— 三栏并列（方案 A）——
    auto* body = new QWidget(this);
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(kPanelGap);

    // 三等分：提醒栏内容比另两栏少，给它多份宽度只会让计时器与番茄钟更挤。
    bodyLayout->addWidget(buildReminderPanel(), 1);
    bodyLayout->addWidget(buildTimerPanel(), 1);
    bodyLayout->addWidget(buildPomodoroPanel(), 1);

    layout->addWidget(body, 1);
}

QWidget* ToolsPage::buildReminderPanel()
{
    auto* card = makeCard(this);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* head = new QWidget(card);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->addWidget(makeLabel(tr("一次性提醒"), 14, head, true));
    headLayout->addStretch();
    _reminderCountLabel = makeLabel(QString(), 11, head);
    trackSecondary(_reminderCountLabel);
    headLayout->addWidget(_reminderCountLabel);
    layout->addWidget(head);

    _reminderLabelEdit = new ElaLineEdit(card);
    _reminderLabelEdit->setPlaceholderText(tr("提醒内容，例如「起来走两步」"));
    _reminderLabelEdit->setBorderRadius(8);
    _reminderLabelEdit->setIsClearButtonEnable(true);
    layout->addWidget(_reminderLabelEdit);

    auto* actions = new QWidget(card);
    auto* actionsLayout = new QHBoxLayout(actions);
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    actionsLayout->setSpacing(8);

    _reminderDelayBox = new ElaComboBox(actions);
    _reminderDelayBox->setBorderRadius(8);
    // 这一行动静最宽（下拉 + 固定 96 输入 + 按钮），放开最小宽度让整行可压缩。
    _reminderDelayBox->setMinimumWidth(0);
    for (const DelayOption& option : kDelayOptions) {
        _reminderDelayBox->addItem(tr(option.text), static_cast<qint64>(option.delayMs));
    }
    actionsLayout->addWidget(_reminderDelayBox, 1);

    // 自定义分钟：填了就以这里为准，下拉的档位只在留空时生效。
    _reminderCustomMinutesEdit = new ElaLineEdit(actions);
    _reminderCustomMinutesEdit->setPlaceholderText(tr("自定义分钟"));
    _reminderCustomMinutesEdit->setBorderRadius(8);
    _reminderCustomMinutesEdit->setIsClearButtonEnable(true);
    // 原来是写死 96：三栏等分时最窄那栏放不下「下拉 + 输入 + 按钮」，
    // 整行会往右顶出去。给个上下限，让它随栏宽伸缩。
    _reminderCustomMinutesEdit->setMinimumWidth(64);
    _reminderCustomMinutesEdit->setMaximumWidth(96);
    actionsLayout->addWidget(_reminderCustomMinutesEdit);

    _reminderAddButton = new ElaPushButton(tr("添加"), actions);
    _reminderAddButton->setBorderRadius(8);
    _reminderAddButton->setMinimumWidth(0);
    actionsLayout->addWidget(_reminderAddButton);
    layout->addWidget(actions);

    connect(_reminderAddButton, &ElaPushButton::clicked, this, &ToolsPage::onAddReminder);

    auto* listHost = new QWidget(card);
    _reminderListLayout = new QVBoxLayout(listHost);
    _reminderListLayout->setContentsMargins(0, 4, 0, 0);
    _reminderListLayout->setSpacing(6);
    layout->addWidget(listHost, 1);

    _reminderEmptyLabel = makeLabel(tr("还没有待触发的提醒。"), 12, card);
    trackSecondary(_reminderEmptyLabel);
    layout->addWidget(_reminderEmptyLabel);

    // —— 每日时长提醒 ——
    // 与上面的一次性提醒是两种东西：这里盯的是「某个应用/分类今天累计用了多久」，
    // 超了就弹一次（一天一次，落库标记）。规则由本页创建，引擎侧每分钟查一次。
    auto* divider = new QFrame(card);
    divider->setFrameShape(QFrame::HLine);
    divider->setFrameShadow(QFrame::Plain);
    layout->addWidget(divider);

    auto* activityHead = new QWidget(card);
    auto* activityHeadLayout = new QHBoxLayout(activityHead);
    activityHeadLayout->setContentsMargins(0, 0, 0, 0);
    activityHeadLayout->addWidget(makeLabel(tr("每日时长提醒"), 14, activityHead, true));
    activityHeadLayout->addStretch();
    _activityCountLabel = makeLabel(QString(), 11, activityHead);
    trackSecondary(_activityCountLabel);
    activityHeadLayout->addWidget(_activityCountLabel);
    layout->addWidget(activityHead);

    auto* activityActions = new QWidget(card);
    auto* activityActionsLayout = new QHBoxLayout(activityActions);
    activityActionsLayout->setContentsMargins(0, 0, 0, 0);
    activityActionsLayout->setSpacing(8);

    _activityKindBox = new ElaComboBox(activityActions);
    _activityKindBox->setBorderRadius(8);
    _activityKindBox->setMinimumWidth(0);
    _activityKindBox->addItem(tr("按应用"), QStringLiteral("app"));
    _activityKindBox->addItem(tr("按分类"), QStringLiteral("category"));
    activityActionsLayout->addWidget(_activityKindBox, 1);

    _activityTargetBox = new ElaComboBox(activityActions);
    _activityTargetBox->setBorderRadius(8);
    _activityTargetBox->setMinimumWidth(0);
    activityActionsLayout->addWidget(_activityTargetBox, 1);
    layout->addWidget(activityActions);

    auto* activityLimitRow = new QWidget(card);
    auto* activityLimitLayout = new QHBoxLayout(activityLimitRow);
    activityLimitLayout->setContentsMargins(0, 0, 0, 0);
    activityLimitLayout->setSpacing(8);

    _activityLimitMinutesEdit = new ElaLineEdit(activityLimitRow);
    _activityLimitMinutesEdit->setPlaceholderText(tr("每日上限分钟"));
    _activityLimitMinutesEdit->setBorderRadius(8);
    _activityLimitMinutesEdit->setIsClearButtonEnable(true);
    _activityLimitMinutesEdit->setMinimumWidth(64);
    _activityLimitMinutesEdit->setMaximumWidth(96);
    activityLimitLayout->addWidget(_activityLimitMinutesEdit);

    _activityMessageEdit = new ElaLineEdit(activityLimitRow);
    _activityMessageEdit->setPlaceholderText(tr("提醒文案，可留空"));
    _activityMessageEdit->setBorderRadius(8);
    _activityMessageEdit->setIsClearButtonEnable(true);
    _activityMessageEdit->setMinimumWidth(0);
    activityLimitLayout->addWidget(_activityMessageEdit, 1);

    _activityAddButton = new ElaPushButton(tr("添加"), activityLimitRow);
    _activityAddButton->setBorderRadius(8);
    _activityAddButton->setMinimumWidth(0);
    activityLimitLayout->addWidget(_activityAddButton);
    layout->addWidget(activityLimitRow);

    connect(_activityKindBox, &QComboBox::currentIndexChanged, this,
            [this](int) { onActivityTargetKindChanged(); });
    connect(_activityAddButton, &ElaPushButton::clicked, this, &ToolsPage::onAddActivityRule);

    auto* ruleHost = new QWidget(card);
    _activityRuleListLayout = new QVBoxLayout(ruleHost);
    _activityRuleListLayout->setContentsMargins(0, 4, 0, 0);
    _activityRuleListLayout->setSpacing(6);
    layout->addWidget(ruleHost, 1);

    _activityEmptyLabel = makeLabel(tr("还没有每日时长提醒。"), 12, card);
    trackSecondary(_activityEmptyLabel);
    layout->addWidget(_activityEmptyLabel);

    _reminderNoteLabel = makeLabel(tr("到点会弹系统通知；关掉窗口也会照常提醒。"), 11, card);
    _reminderNoteLabel->setWordWrap(true);
    trackSecondary(_reminderNoteLabel);
    layout->addWidget(_reminderNoteLabel);

    return card;
}

QWidget* ToolsPage::buildTimerPanel()
{
    auto* card = makeCard(this);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* head = new QWidget(card);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->addWidget(makeLabel(tr("计时器"), 14, head, true));
    headLayout->addStretch();
    layout->addWidget(head);

    // 模式与时长并排：两栏等分时纵向每多一行输入就多 45px，四个独立行会把
    // 32px 读数挤出可视区。
    auto* modeRow = new QWidget(card);
    auto* modeRowLayout = new QHBoxLayout(modeRow);
    modeRowLayout->setContentsMargins(0, 0, 0, 0);
    modeRowLayout->setSpacing(8);

    _timerModeBox = new ElaComboBox(modeRow);
    _timerModeBox->setBorderRadius(8);
    _timerModeBox->setMinimumWidth(0);
    _timerModeBox->addItem(tr("秒表"), static_cast<int>(TimerMode::Stopwatch));
    _timerModeBox->addItem(tr("倒计时"), static_cast<int>(TimerMode::Countdown));
    modeRowLayout->addWidget(_timerModeBox, 1);

    _timerMinutesBox = new ElaComboBox(modeRow);
    _timerMinutesBox->setBorderRadius(8);
    _timerMinutesBox->setMinimumWidth(0);
    for (int minutes : {5, 10, 15, 25, 30, 45, 60, 90}) {
        _timerMinutesBox->addItem(tr("%1 分钟").arg(minutes), minutes);
    }
    modeRowLayout->addWidget(_timerMinutesBox, 1);
    layout->addWidget(modeRow);

    auto* detailRow = new QWidget(card);
    auto* detailRowLayout = new QHBoxLayout(detailRow);
    detailRowLayout->setContentsMargins(0, 0, 0, 0);
    detailRowLayout->setSpacing(8);

    // 自定义时长：填了就以这里为准，下拉档位只在留空时生效。
    _timerCustomMinutesEdit = new ElaLineEdit(detailRow);
    _timerCustomMinutesEdit->setPlaceholderText(tr("自定义分钟"));
    _timerCustomMinutesEdit->setBorderRadius(8);
    _timerCustomMinutesEdit->setIsClearButtonEnable(true);
    _timerCustomMinutesEdit->setMinimumWidth(0);
    detailRowLayout->addWidget(_timerCustomMinutesEdit, 1);

    _timerLabelEdit = new ElaLineEdit(detailRow);
    _timerLabelEdit->setPlaceholderText(tr("标签，可留空"));
    _timerLabelEdit->setBorderRadius(8);
    _timerLabelEdit->setIsClearButtonEnable(true);
    _timerLabelEdit->setMinimumWidth(0);
    detailRowLayout->addWidget(_timerLabelEdit, 1);
    layout->addWidget(detailRow);

    _timerDisplayLabel = makeLabel(QStringLiteral("00:00:00"), 32, card, true);
    _timerDisplayLabel->setAlignment(Qt::AlignCenter);
    _timerDisplayLabel->setWordWrap(false);
    // 32px 大字读数的 sizeHint 约 200px，是这一栏最小宽度的主要来源，放开它。
    _timerDisplayLabel->setMinimumWidth(0);
    layout->addWidget(_timerDisplayLabel);

    _timerStateLabel = makeLabel(QString(), 11, card);
    _timerStateLabel->setAlignment(Qt::AlignCenter);
    trackSecondary(_timerStateLabel);
    layout->addWidget(_timerStateLabel);

    auto* buttons = new QWidget(card);
    auto* buttonsLayout = new QHBoxLayout(buttons);
    buttonsLayout->setContentsMargins(0, 0, 0, 0);
    buttonsLayout->setSpacing(8);
    // 三连按钮行是这栏最窄 230px 的来源，放开最小宽度使其可随列压缩。
    _timerPrimaryButton = new ElaPushButton(tr("开始"), buttons);
    _timerPrimaryButton->setBorderRadius(8);
    _timerPrimaryButton->setMinimumWidth(0);
    _timerSecondaryButton = new ElaPushButton(tr("暂停"), buttons);
    _timerSecondaryButton->setBorderRadius(8);
    _timerSecondaryButton->setMinimumWidth(0);
    _timerTertiaryButton = new ElaPushButton(tr("重置"), buttons);
    _timerTertiaryButton->setBorderRadius(8);
    _timerTertiaryButton->setMinimumWidth(0);
    buttonsLayout->addWidget(_timerPrimaryButton);
    buttonsLayout->addWidget(_timerSecondaryButton);
    buttonsLayout->addWidget(_timerTertiaryButton);
    layout->addWidget(buttons);

    connect(_timerPrimaryButton, &ElaPushButton::clicked, this, &ToolsPage::onTimerPrimary);
    connect(_timerSecondaryButton, &ElaPushButton::clicked, this, &ToolsPage::onTimerSecondary);
    connect(_timerTertiaryButton, &ElaPushButton::clicked, this, &ToolsPage::onTimerTertiary);
    // 切模式立刻刷新一次：时长框的可用性跟下拉选中项走，不等下一次 tick。
    connect(_timerModeBox, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!_active || _updating) {
            return;
        }
        refreshTimer();
    });

    auto* lapHead = makeLabel(tr("计次记录"), 11, card, true);
    layout->addWidget(lapHead);

    auto* lapHost = new QWidget(card);
    _lapListLayout = new QVBoxLayout(lapHost);
    _lapListLayout->setContentsMargins(0, 0, 0, 0);
    _lapListLayout->setSpacing(4);
    layout->addWidget(lapHost, 1);

    _lapEmptyLabel = makeLabel(tr("秒表运行时可以计次。"), 11, card);
    trackSecondary(_lapEmptyLabel);
    layout->addWidget(_lapEmptyLabel);

    return card;
}

QWidget* ToolsPage::buildPomodoroPanel()
{
    auto* card = makeCard(this);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* head = new QWidget(card);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->addWidget(makeLabel(tr("番茄钟"), 14, head, true));
    headLayout->addStretch();
    _pomodoroTodayLabel = makeLabel(QString(), 11, head);
    trackSecondary(_pomodoroTodayLabel);
    headLayout->addWidget(_pomodoroTodayLabel);
    layout->addWidget(head);

    _pomodoroRing = new CountdownRingWidget(card);
    layout->addWidget(_pomodoroRing, 0, Qt::AlignHCenter);

    _pomodoroPhaseLabel = makeLabel(QString(), 13, card, true);
    _pomodoroPhaseLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(_pomodoroPhaseLabel);

    _pomodoroCycleLabel = makeLabel(QString(), 11, card);
    _pomodoroCycleLabel->setAlignment(Qt::AlignCenter);
    trackSecondary(_pomodoroCycleLabel);
    layout->addWidget(_pomodoroCycleLabel);

    auto* buttons = new QWidget(card);
    auto* buttonsLayout = new QHBoxLayout(buttons);
    buttonsLayout->setContentsMargins(0, 0, 0, 0);
    buttonsLayout->setSpacing(8);
    _pomodoroPrimaryButton = new ElaPushButton(tr("开始"), buttons);
    _pomodoroPrimaryButton->setBorderRadius(8);
    _pomodoroPrimaryButton->setMinimumWidth(0);
    _pomodoroSecondaryButton = new ElaPushButton(tr("暂停"), buttons);
    _pomodoroSecondaryButton->setBorderRadius(8);
    _pomodoroSecondaryButton->setMinimumWidth(0);
    _pomodoroTertiaryButton = new ElaPushButton(tr("跳过"), buttons);
    _pomodoroTertiaryButton->setBorderRadius(8);
    _pomodoroTertiaryButton->setMinimumWidth(0);
    buttonsLayout->addWidget(_pomodoroPrimaryButton);
    buttonsLayout->addWidget(_pomodoroSecondaryButton);
    buttonsLayout->addWidget(_pomodoroTertiaryButton);
    layout->addWidget(buttons);

    connect(_pomodoroPrimaryButton, &ElaPushButton::clicked, this, &ToolsPage::onPomodoroPrimary);
    connect(_pomodoroSecondaryButton, &ElaPushButton::clicked, this, &ToolsPage::onPomodoroSecondary);
    connect(_pomodoroTertiaryButton, &ElaPushButton::clicked, this, &ToolsPage::onPomodoroTertiary);

    _settingsBox = new QWidget(card);
    auto* settingsLayout = new QVBoxLayout(_settingsBox);
    settingsLayout->setContentsMargins(0, 8, 0, 0);
    settingsLayout->setSpacing(6);
    settingsLayout->addWidget(makeLabel(tr("时长设置（分钟）"), 11, _settingsBox, true));

    auto makeRow = [this](QWidget* host, const QString& caption, ElaComboBox** boxOut,
                          const QVector<int>& values) {
        auto* row = new QWidget(host);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);
        auto* captionLabel = makeLabel(caption, 11, row);
        trackSecondary(captionLabel);
        rowLayout->addWidget(captionLabel);
        rowLayout->addStretch();
        auto* box = new ElaComboBox(row);
        box->setBorderRadius(8);
        // 最小宽度按最长 item 算，「每隔几轮长休」那一行会把整栏撑宽。
        box->setMinimumWidth(0);
        box->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        for (int value : values) {
            box->addItem(QString::number(value), value);
        }
        rowLayout->addWidget(box);
        *boxOut = box;
        return row;
    };

    settingsLayout->addWidget(makeRow(_settingsBox, tr("专注"), &_focusMinutesBox,
                                      {15, 20, 25, 30, 40, 45, 50, 60}));
    settingsLayout->addWidget(makeRow(_settingsBox, tr("短休息"), &_shortBreakBox, {3, 5, 8, 10}));
    settingsLayout->addWidget(makeRow(_settingsBox, tr("长休息"), &_longBreakBox, {10, 15, 20, 25, 30}));
    settingsLayout->addWidget(makeRow(_settingsBox, tr("每隔几轮长休"), &_longEveryBox, {2, 3, 4, 5, 6}));

    layout->addWidget(_settingsBox);
    layout->addStretch();

    auto applySettings = [this](int) {
        if (_updating) {
            return;
        }
        ToolsSettings settings = runtime().settings();
        settings.pomodoroFocusMinutes = _focusMinutesBox->currentData().toInt();
        settings.pomodoroShortBreakMinutes = _shortBreakBox->currentData().toInt();
        settings.pomodoroLongBreakMinutes = _longBreakBox->currentData().toInt();
        settings.pomodoroLongBreakEvery = _longEveryBox->currentData().toInt();
        runtime().applySettings(settings);
    };
    connect(_focusMinutesBox, &QComboBox::currentIndexChanged, this, applySettings);
    connect(_shortBreakBox, &QComboBox::currentIndexChanged, this, applySettings);
    connect(_longBreakBox, &QComboBox::currentIndexChanged, this, applySettings);
    connect(_longEveryBox, &QComboBox::currentIndexChanged, this, applySettings);

    return card;
}

void ToolsPage::setActive(bool active)
{
    _active = active;
    runtime().setUiVisible(active);

    if (!active) {
        // 离开页面时把列表清掉，顺手停掉读数心跳。
        // 运行时状态不在页面里，提醒与计时照常跑。
        _refreshTimer->stop();
        clearLayout(_reminderListLayout);
        clearLayout(_activityRuleListLayout);
        clearLayout(_lapListLayout);
        return;
    }
    refreshAll();
    _refreshTimer->start();
}

void ToolsPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (_active) {
        refreshAll();
        _refreshTimer->start();
    }
}

void ToolsPage::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    // 整窗隐藏（最小化到托盘）时也要停：看不见的秒表没必要每秒重绘。
    _refreshTimer->stop();
    runtime().setUiVisible(false);
}

void ToolsPage::refreshAll()
{
    _updating = true;
    refreshReminders();
    refreshActivityRules();
    refreshTimer();
    refreshPomodoro();
    refreshTrayHint();
    _updating = false;
}

void ToolsPage::tickDisplay()
{
    // 只动会自己变的文字，不碰列表：每秒重建一次提醒行会把按钮从鼠标底下抽走。
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const ToolTimer& timer = runtime().timer();
    const bool isCountdown = timer.mode == TimerMode::Countdown;
    const qint64 shownMs = isCountdown ? timer.remainingMsAt(nowMs) : timer.elapsedMsAt(nowMs);
    _timerDisplayLabel->setText(formatHms(shownMs));

    const ToolPomodoro& pomodoro = runtime().pomodoro();
    const bool measurable = pomodoro.status == PomodoroStatus::Running
                            || pomodoro.status == PomodoroStatus::Paused;
    const qint64 remainingMs = pomodoro.remainingMsAt(nowMs);
    const qint64 totalMs = std::max<qint64>(1, pomodoro.phaseDurationMs());
    _pomodoroRing->setProgress(measurable ? 1.0 - static_cast<qreal>(remainingMs) / totalMs : -1.0);
    _pomodoroRing->setPrimaryText(formatCompact(remainingMs));

    // 提醒的「还剩几分钟」也跟着走。但整行重建会把「取消」按钮从鼠标底下抽走，
    // 所以只在分钟数变化时重建一次，一秒一次太凶。
    const qint64 currentMinute = nowMs / kMinuteMs;
    if (currentMinute != _lastReminderMinute) {
        _lastReminderMinute = currentMinute;
        if (!runtime().reminders().isEmpty()) {
            refreshReminders();
        }
    }
    refreshTrayHint();
}

void ToolsPage::refreshReminders()
{
    clearLayout(_reminderListLayout);

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    int pendingCount = 0;

    // 只列待触发的：已触发和已取消的留在库里做记录，但不占界面。
    for (const ToolReminder& reminder : runtime().reminders()) {
        if (!reminder.isPending()) {
            continue;
        }
        ++pendingCount;

        auto* row = new QWidget(_reminderListLayout->parentWidget());
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);

        auto* textBox = new QWidget(row);
        auto* textLayout = new QVBoxLayout(textBox);
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(1);
        const QString label =
            reminder.label.trimmed().isEmpty() ? tr("未命名提醒") : reminder.label.trimmed();
        textLayout->addWidget(makeLabel(label, 12, textBox, true));
        textLayout->addWidget(makeLabel(formatRemaining(reminder.remainingMsAt(nowMs)), 10, textBox));
        rowLayout->addWidget(textBox, 1);

        auto* cancelButton = new ElaPushButton(tr("取消"), row);
        cancelButton->setBorderRadius(8);
        const QString reminderId = reminder.id;
        connect(cancelButton, &ElaPushButton::clicked, this,
                [this, reminderId]() { onCancelReminder(reminderId); });
        rowLayout->addWidget(cancelButton);

        _reminderListLayout->addWidget(row);
    }
    _reminderListLayout->addStretch();

    _reminderEmptyLabel->setVisible(pendingCount == 0);
    _reminderCountLabel->setText(pendingCount > 0 ? tr("%1 个待触发").arg(pendingCount) : QString());
}

void ToolsPage::reloadActivityTargets()
{
    if (_activityTargetBox == nullptr) {
        return;
    }
    const QString kind = _activityKindBox != nullptr
                             ? _activityKindBox->currentData().toString()
                             : QStringLiteral("app");
    _activityTargetBox->clear();

    if (_database == nullptr || !_database->isOpen()) {
        return;
    }

    if (kind == QLatin1String("category")) {
        const data::Result<QVector<data::CategoryEntry>> categories =
            data::loadCategoryRegistry(*_database);
        if (!categories.ok()) {
            return;
        }
        for (const data::CategoryEntry& entry : categories.value) {
            _activityTargetBox->addItem(entry.label, entry.id);
        }
        return;
    }

    // 应用候选：按最近使用排序，和分类管理页看到的一致。
    data::ClassificationRepository repository(*_database);
    const data::Result<QVector<data::AppClassificationEntry>> apps = repository.listApps();
    if (!apps.ok()) {
        return;
    }
    for (const data::AppClassificationEntry& app : apps.value) {
        // 「不在列表显示」的应用不再出现在任何列表里，提醒规则的目标下拉同理。
        if (app.hidden) {
            continue;
        }
        const QString display =
            app.displayName.trimmed().isEmpty() ? app.exeName : app.displayName.trimmed();
        _activityTargetBox->addItem(QStringLiteral("%1 (%2)").arg(display, app.exeName), app.exeName);
    }
}

void ToolsPage::onActivityTargetKindChanged()
{
    reloadActivityTargets();
}

void ToolsPage::refreshActivityRules()
{
    clearLayout(_activityRuleListLayout);

    const QVector<data::ToolActivityReminderRule>& rules = runtime().activityRules();
    int count = 0;
    for (const data::ToolActivityReminderRule& rule : rules) {
        if (!rule.isActive()) {
            continue;
        }
        ++count;

        auto* row = new QWidget(_activityRuleListLayout->parentWidget());
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);

        auto* textBox = new QWidget(row);
        auto* textLayout = new QVBoxLayout(textBox);
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(1);

        // 目标名优先用建规则时的快照：应用被改名/分类被删之后，历史规则
        // 仍要能看出当初提醒的是什么。
        const QString target = rule.labelSnapshot.trimmed().isEmpty()
                                   ? (rule.targetKind == QLatin1String("category")
                                          ? rule.categoryId
                                          : rule.exeName)
                                   : rule.labelSnapshot.trimmed();
        textLayout->addWidget(makeLabel(target, 12, textBox, true));
        const qint64 limitMinutes = rule.limitMs / kMinuteMs;
        textLayout->addWidget(
            makeLabel(tr("每日超过 %1 分钟").arg(limitMinutes), 10, textBox));
        rowLayout->addWidget(textBox, 1);

        auto* disableButton = new ElaPushButton(tr("停用"), row);
        disableButton->setBorderRadius(8);
        const qint64 ruleId = rule.id;
        connect(disableButton, &ElaPushButton::clicked, this,
                [this, ruleId]() { onDisableActivityRule(ruleId); });
        rowLayout->addWidget(disableButton);

        _activityRuleListLayout->addWidget(row);
    }
    _activityRuleListLayout->addStretch();

    _activityEmptyLabel->setVisible(count == 0);
    _activityCountLabel->setText(count > 0 ? tr("%1 条规则").arg(count) : QString());
}

void ToolsPage::refreshTimer()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const ToolTimer& timer = runtime().timer();

    const bool isCountdown = timer.mode == TimerMode::Countdown;
    const qint64 shownMs = isCountdown ? timer.remainingMsAt(nowMs) : timer.elapsedMsAt(nowMs);
    _timerDisplayLabel->setText(formatHms(shownMs));

    // 模式与时长在有活跃计时器时锁定：中途换模式会让已累计的时长失去意义。
    // 时长框跟随「下拉框当前选中项」而不是运行时 mode——计时器还没开始时
    // runtime 的 mode 还是上一次的值，跟着它走会出现「切到倒计时却选不了时长，
    // 必须先开始再重置一次」的死锁。
    const bool locked = timer.isActive();
    const bool comboIsCountdown =
        _timerModeBox->currentData().toInt() == static_cast<int>(TimerMode::Countdown);
    _timerModeBox->setEnabled(!locked);
    _timerMinutesBox->setEnabled(!locked && comboIsCountdown);
    _timerCustomMinutesEdit->setEnabled(!locked && comboIsCountdown);
    _timerLabelEdit->setEnabled(!locked);

    switch (timer.status) {
    case TimerStatus::Idle:
        _timerStateLabel->setText(tr("未开始"));
        _timerPrimaryButton->setText(tr("开始"));
        _timerPrimaryButton->setEnabled(true);
        _timerSecondaryButton->setEnabled(false);
        _timerTertiaryButton->setEnabled(false);
        break;
    case TimerStatus::Running:
        _timerStateLabel->setText(isCountdown ? tr("倒计时中") : tr("计时中"));
        _timerPrimaryButton->setText(isCountdown ? tr("暂停") : tr("计次"));
        _timerPrimaryButton->setEnabled(true);
        _timerSecondaryButton->setText(tr("暂停"));
        _timerSecondaryButton->setEnabled(isCountdown);
        _timerTertiaryButton->setEnabled(true);
        break;
    case TimerStatus::Paused:
        _timerStateLabel->setText(tr("已暂停"));
        _timerPrimaryButton->setText(tr("继续"));
        _timerPrimaryButton->setEnabled(true);
        _timerSecondaryButton->setEnabled(false);
        _timerTertiaryButton->setEnabled(true);
        break;
    case TimerStatus::Completed:
        _timerStateLabel->setText(tr("已完成"));
        _timerPrimaryButton->setText(tr("开始"));
        _timerPrimaryButton->setEnabled(true);
        _timerSecondaryButton->setEnabled(false);
        _timerTertiaryButton->setEnabled(true);
        break;
    }

    clearLayout(_lapListLayout);
    const QVector<ToolLap>& laps = runtime().laps();
    for (const ToolLap& lap : laps) {
        auto* row = new QWidget(_lapListLayout->parentWidget());
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);
        rowLayout->addWidget(makeLabel(tr("第 %1 次").arg(lap.index), 11, row));
        rowLayout->addStretch();
        rowLayout->addWidget(makeLabel(formatHms(lap.durationMs), 11, row, true));
        _lapListLayout->addWidget(row);
    }
    _lapListLayout->addStretch();
    _lapEmptyLabel->setVisible(laps.isEmpty());
}

void ToolsPage::refreshPomodoro()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const ToolPomodoro& pomodoro = runtime().pomodoro();

    const qint64 remainingMs = pomodoro.remainingMsAt(nowMs);
    const qint64 totalMs = std::max<qint64>(1, pomodoro.phaseDurationMs());
    const bool measurable =
        pomodoro.status == PomodoroStatus::Running || pomodoro.status == PomodoroStatus::Paused;
    _pomodoroRing->setProgress(measurable ? 1.0 - static_cast<qreal>(remainingMs) / totalMs : -1.0);
    _pomodoroRing->setPrimaryText(formatCompact(remainingMs));
    _pomodoroRing->setSecondaryText(phaseText(pomodoro.phase));
    _pomodoroRing->setPaused(pomodoro.status == PomodoroStatus::Paused);

    _pomodoroPhaseLabel->setText(phaseText(pomodoro.phase));
    _pomodoroCycleLabel->setText(tr("第 %1 轮 · 每 %2 轮长休息")
                                     .arg(std::max(1, pomodoro.cycleIndex))
                                     .arg(std::max(1, pomodoro.longBreakEvery)));
    _pomodoroTodayLabel->setText(tr("今日 %1 个").arg(pomodoro.completedFocusToday));

    switch (pomodoro.status) {
    case PomodoroStatus::Idle:
        _pomodoroPrimaryButton->setText(tr("开始专注"));
        _pomodoroPrimaryButton->setEnabled(true);
        _pomodoroSecondaryButton->setEnabled(false);
        _pomodoroTertiaryButton->setEnabled(false);
        break;
    case PomodoroStatus::Running:
        _pomodoroPrimaryButton->setText(tr("暂停"));
        _pomodoroPrimaryButton->setEnabled(true);
        _pomodoroSecondaryButton->setEnabled(false);
        _pomodoroTertiaryButton->setText(tr("跳过"));
        _pomodoroTertiaryButton->setEnabled(true);
        break;
    case PomodoroStatus::Paused:
        // 到点自动推进后也停在这个状态：等人点「开始」。
        _pomodoroPrimaryButton->setText(tr("开始"));
        _pomodoroPrimaryButton->setEnabled(true);
        _pomodoroSecondaryButton->setEnabled(false);
        _pomodoroTertiaryButton->setText(tr("重置"));
        _pomodoroTertiaryButton->setEnabled(true);
        break;
    case PomodoroStatus::Completed:
        _pomodoroPrimaryButton->setText(tr("开始专注"));
        _pomodoroPrimaryButton->setEnabled(true);
        _pomodoroSecondaryButton->setEnabled(false);
        _pomodoroTertiaryButton->setEnabled(true);
        break;
    }

    const ToolsSettings settings = runtime().settings();
    const bool settingsLocked =
        pomodoro.status == PomodoroStatus::Running || pomodoro.status == PomodoroStatus::Paused;
    _settingsBox->setEnabled(!settingsLocked);

    const auto selectByData = [](ElaComboBox* box, int value) {
        const int index = box->findData(value);
        if (index >= 0) {
            box->setCurrentIndex(index);
        }
    };
    selectByData(_focusMinutesBox, settings.pomodoroFocusMinutes);
    selectByData(_shortBreakBox, settings.pomodoroShortBreakMinutes);
    selectByData(_longBreakBox, settings.pomodoroLongBreakMinutes);
    selectByData(_longEveryBox, settings.pomodoroLongBreakEvery);
}

void ToolsPage::refreshTrayHint()
{
    const QString text = runtime().trayText();
    _trayHintLabel->setText(text.isEmpty() ? tr("托盘无进行中的项目") : text);
}

// —— 槽 ——

void ToolsPage::onAddReminder()
{
    const QString label = _reminderLabelEdit->text().trimmed();

    // 自定义分钟优先：填了就用它，留空才用下拉档位。
    qint64 delayMs = _reminderDelayBox->currentData().toLongLong();
    const QString customText = _reminderCustomMinutesEdit->text().trimmed();
    if (!customText.isEmpty()) {
        const int customMinutes = parseCustomMinutes(customText, nullptr);
        if (customMinutes <= 0) {
            _reminderCustomMinutesEdit->setPlaceholderText(
                tr("填 %1–%2 之间的分钟数").arg(kMinCustomMinutes).arg(kMaxCustomMinutes));
            return;
        }
        delayMs = static_cast<qint64>(customMinutes) * kMinuteMs;
    }

    const data::Status status = runtime().addReminder(label, delayMs);
    if (status.ok()) {
        _reminderLabelEdit->clear();
        _reminderCustomMinutesEdit->clear();
        _reminderCustomMinutesEdit->setPlaceholderText(tr("自定义分钟"));
        return;
    }
    _reminderLabelEdit->setPlaceholderText(tr("添加失败：%1").arg(status.error));
}

void ToolsPage::onCancelReminder(const QString& id)
{
    runtime().cancelReminder(id);
}

void ToolsPage::onAddActivityRule()
{
    const QString kind = _activityKindBox->currentData().toString();
    const QString target = _activityTargetBox->currentData().toString();
    if (target.isEmpty()) {
        _activityTargetBox->setToolTip(tr("先选一个目标"));
        return;
    }

    // 上限按分钟填（1–1440 分钟）。
    bool ok = false;
    const int minutes = _activityLimitMinutesEdit->text().trimmed().toInt(&ok);
    if (!ok || minutes < 1 || minutes > 24 * 60) {
        _activityLimitMinutesEdit->setPlaceholderText(tr("填 1–1440 之间的分钟数"));
        return;
    }

    // 标签快照取候选里的显示名：应用被改名之后规则列表还能认出来。
    const int index = _activityTargetBox->currentIndex();
    const QString label = index >= 0 ? _activityTargetBox->itemText(index) : target;
    const QString appName = kind == QLatin1String("app") ? label : QString();
    const QString exeName = kind == QLatin1String("app") ? target : QString();
    const QString categoryId = kind == QLatin1String("category") ? target : QString();

    const data::Status status = runtime().addActivityRule(
        kind, appName, exeName, categoryId, label, static_cast<qint64>(minutes) * kMinuteMs,
        _activityMessageEdit->text().trimmed());
    if (status.ok()) {
        _activityLimitMinutesEdit->clear();
        _activityMessageEdit->clear();
        _activityLimitMinutesEdit->setPlaceholderText(tr("每日上限分钟"));
        return;
    }
    _activityLimitMinutesEdit->setPlaceholderText(tr("添加失败：%1").arg(status.error));
}

void ToolsPage::onDisableActivityRule(qint64 ruleId)
{
    runtime().disableActivityRule(ruleId);
}

void ToolsPage::onTimerPrimary()
{
    const ToolTimer& timer = runtime().timer();
    if (timer.status == TimerStatus::Running) {
        if (timer.mode == TimerMode::Stopwatch) {
            runtime().addLap();
        } else {
            runtime().pauseTimer();
        }
        return;
    }
    if (timer.status == TimerStatus::Paused) {
        runtime().resumeTimer();
        return;
    }

    const auto mode = static_cast<TimerMode>(_timerModeBox->currentData().toInt());

    // 自定义分钟优先：填了就用它，留空才用下拉档位。
    qint64 minutes = static_cast<qint64>(_timerMinutesBox->currentData().toInt());
    const QString customText = _timerCustomMinutesEdit->text().trimmed();
    if (!customText.isEmpty()) {
        const int customMinutes = parseCustomMinutes(customText, nullptr);
        if (customMinutes <= 0) {
            _timerCustomMinutesEdit->setPlaceholderText(
                tr("填 %1–%2 之间的分钟数").arg(kMinCustomMinutes).arg(kMaxCustomMinutes));
            return;
        }
        minutes = customMinutes;
    }

    runtime().startTimer(mode, minutes * kMinuteMs, _timerLabelEdit->text());
}

void ToolsPage::onTimerSecondary()
{
    runtime().pauseTimer();
}

void ToolsPage::onTimerTertiary()
{
    runtime().resetTimer();
}

void ToolsPage::onPomodoroPrimary()
{
    const ToolPomodoro& pomodoro = runtime().pomodoro();
    if (pomodoro.status == PomodoroStatus::Running) {
        runtime().pausePomodoro();
        return;
    }
    if (pomodoro.status == PomodoroStatus::Paused) {
        runtime().resumePomodoro();
        return;
    }
    runtime().startPomodoro();
}

void ToolsPage::onPomodoroSecondary()
{
    runtime().pausePomodoro();
}

void ToolsPage::onPomodoroTertiary()
{
    const ToolPomodoro& pomodoro = runtime().pomodoro();
    if (pomodoro.status == PomodoroStatus::Running) {
        runtime().skipPomodoroPhase();
        return;
    }
    runtime().resetPomodoro();
}

} // namespace patina::ui
