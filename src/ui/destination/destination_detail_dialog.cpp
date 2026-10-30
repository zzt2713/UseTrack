#include "ui/destination/destination_detail_dialog.h"

#include <QDate>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>

#include "ElaIconButton.h"
#include "ElaPushButton.h"
#include "ElaScrollArea.h"
#include "ElaScrollBar.h"
#include "ElaScrollPageArea.h"
#include "ElaText.h"
#include "ElaTheme.h"

#include "shared/app_icon_cache.h"
#include "shared/theme_styles.h"

namespace patina::ui
{
namespace
{

constexpr int kMinDurationStepMs = 60 * 1000;      // 过滤步长 1 分钟
constexpr qint64 kMinDurationMaxMs = 60 * 60 * 1000; // 过滤上限 60 分钟
constexpr int kTrackHeight = 22;

QString formatDurationLong(qint64 ms)
{
    const qint64 totalMinutes = std::max<qint64>(0, ms) / 60000;
    const qint64 hours = totalMinutes / 60;
    const qint64 minutes = totalMinutes % 60;
    if (hours > 0) {
        return QObject::tr("%1 小时 %2 分").arg(hours).arg(minutes, 2, 10, QLatin1Char('0'));
    }
    return QObject::tr("%1 分").arg(minutes);
}

QString formatClock(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("HH:mm"));
}

QString formatDurationOf(qint64 ms)
{
    const qint64 totalMinutes = std::max<qint64>(0, ms) / 60000;
    const qint64 hours = totalMinutes / 60;
    const qint64 minutes = totalMinutes % 60;
    if (hours > 0) {
        return QObject::tr("%1h %2m").arg(hours).arg(minutes, 2, 10, QLatin1Char('0'));
    }
    return QObject::tr("%1m").arg(minutes);
}

ElaText* makeLabel(const QString& text, int pixelSize, QWidget* parent, bool strong = false)
{
    auto* label = new ElaText(text, pixelSize, parent);
    label->setWordWrap(false);
    if (strong) {
        label->setTextStyle(ElaTextType::BodyStrong);
        label->setTextPixelSize(pixelSize);
    }
    return label;
}

} // namespace

// —— 当日全景条：该目标的全部段按时间落点画在一根轨道上 ——
// 定义在 patina::ui 名字空间级（头文件前置声明的就是它），
// 块类型用文件级的 DestinationTimelineBlock。
class DestinationDayTimelineWidget : public QWidget
{
public:
    explicit DestinationDayTimelineWidget(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(kTrackHeight + 30);
        // 画布透明：基线与色块直接画在卡片底色上。
        setAttribute(Qt::WA_StyledBackground, false);
        setAutoFillBackground(false);
    }

    void setData(qint64 dayStartMs, qint64 dayEndMs,
                 const QVector<DestinationTimelineBlock>& blocks)
    {
        _dayStartMs = dayStartMs;
        _dayEndMs = dayEndMs;
        _blocks = blocks;
        update();
    }

    void clear()
    {
        _blocks.clear();
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event)
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const ElaThemeType::ThemeMode mode = eTheme->getThemeMode();
        const QColor lineColor = ElaThemeColor(mode, BasicDetailsText);
        const QColor gridColor = ElaThemeColor(mode, BasicBorder);

        const qreal trackLeft = 8.0;
        const qreal trackWidth = width() - 16.0;
        const qreal trackCenterY = 22 + kTrackHeight / 2.0;

        // 背景：只画一条基线，不铺满宽色块。
        QPen basePen(gridColor);
        basePen.setWidthF(2.0);
        basePen.setCapStyle(Qt::RoundCap);
        painter.setPen(basePen);
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(QPointF(trackLeft, trackCenterY),
                         QPointF(trackLeft + trackWidth, trackCenterY));

        const qreal span = std::max<qint64>(1, _dayEndMs - _dayStartMs);
        for (const DestinationTimelineBlock& block : _blocks) {
            const qreal left = trackLeft + trackWidth * block.startMs / span;
            const qreal right = trackLeft + trackWidth * block.endMs / span;
            QRectF rect(left, 22 + 3, std::max<qreal>(2.0, right - left), kTrackHeight - 6);
            painter.setPen(Qt::NoPen);
            painter.setBrush(block.color);
            painter.drawRoundedRect(rect, 4, 4);
            if (block.isLive) {
                QPen livePen(ElaThemeColor(mode, PrimaryNormal));
                livePen.setStyle(Qt::DashLine);
                painter.setPen(livePen);
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(rect.adjusted(-1, -1, 1, 1), 5, 5);
            }
        }

        // 刻度：0/6/12/18/24。
        painter.setPen(lineColor);
        QFont font = painter.font();
        font.setPixelSize(10);
        painter.setFont(font);
        for (int hour : {0, 6, 12, 18, 24}) {
            const qreal x = trackLeft + trackWidth * hour / 24.0;
            const QString label = QStringLiteral("%1:00").arg(hour % 24, 2, 10, QLatin1Char('0'));
            QFontMetrics metrics(font);
            qreal textX = x - metrics.horizontalAdvance(label) / 2.0;
            textX = std::clamp(textX, 1.0, width() - metrics.horizontalAdvance(label) - 1.0);
            painter.drawText(QPointF(textX, 14), label);
        }
    }

private:
    qint64 _dayStartMs = 0;
    qint64 _dayEndMs = 1;
    QVector<DestinationTimelineBlock> _blocks;
};

DestinationDetailDialog::DestinationDetailDialog(data::Database* database, QWidget* parent)
    : ElaDialog(parent)
    , _database(database)
{
    buildUi();
    setModal(true);
    resize(680, 520);

    // 弹窗是顶层窗口，继承不到页面的调色板；自己的样式表颜色得跟着主题重铺。
    connect(eTheme, &ElaTheme::themeModeChanged, this, [this](ElaThemeType::ThemeMode mode) {
        refreshThemeStyles(mode == ElaThemeType::ThemeMode::Dark);
        _timeline->update();
    });
}

void DestinationDetailDialog::buildUi()
{
    this->setWindowTitle("");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);

    // —— 头部：图标 + 名字 + 关闭 ——
    auto* head = new QWidget(this);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->setSpacing(10);

    _iconLabel = new QLabel(head);
    _iconLabel->setFixedSize(30, 30);
    _iconLabel->setAlignment(Qt::AlignCenter);
    headLayout->addWidget(_iconLabel);

    auto* titleBox = new QWidget(head);
    auto* titleLayout = new QVBoxLayout(titleBox);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(1);
    _titleLabel = makeLabel(QString(), 15, titleBox, true);
    titleLayout->addWidget(_titleLabel);
    _subtitleLabel = makeLabel(QString(), 11, titleBox);
    titleLayout->addWidget(_subtitleLabel);
    headLayout->addWidget(titleBox, 1);
    layout->addWidget(head);

    // —— 控制条：日期翻页 + 最短时长 + 合计 ——
    auto* controls = new QWidget(this);
    auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(8);

    _prevDateButton = new ElaIconButton(ElaIconType::CaretLeft, 11, 26, 26, controls);
    _prevDateButton->setBorderRadius(8);
    _prevDateButton->setToolTip(tr("前一天"));
    connect(_prevDateButton, &ElaIconButton::clicked, this, [this]() { shiftDate(-1); });
    controlsLayout->addWidget(_prevDateButton);

    _todayButton = new ElaPushButton(tr("今天"), controls);
    _todayButton->setFixedHeight(26);
    _todayButton->setMinimumWidth(56);
    _todayButton->setBorderRadius(8);
    connect(_todayButton, &ElaPushButton::clicked, this,
            [this]() { _date = QDate::currentDate(); loadDay(); });
    controlsLayout->addWidget(_todayButton);

    _nextDateButton = new ElaIconButton(ElaIconType::CaretRight, 11, 26, 26, controls);
    _nextDateButton->setBorderRadius(8);
    _nextDateButton->setToolTip(tr("后一天"));
    connect(_nextDateButton, &ElaIconButton::clicked, this, [this]() { shiftDate(1); });
    controlsLayout->addWidget(_nextDateButton);

    _dateLabel = makeLabel(QString(), 12, controls, true);
    controlsLayout->addWidget(_dateLabel);

    // 日期翻页一组，最短时长过滤一组，合计靠右。原来全塞在一行里，
    // 两处伸缩各自分摊剩余宽度，680 宽的对话框一窄就互相挤。
    controlsLayout->addStretch();

    _minMinusButton = new ElaIconButton(ElaIconType::Minus, 10, 22, 22, controls);
    _minMinusButton->setBorderRadius(6);
    _minMinusButton->setToolTip(tr("放宽过滤"));
    connect(_minMinusButton, &ElaIconButton::clicked, this,
            [this]() { adjustMinDuration(-kMinDurationStepMs); });
    controlsLayout->addWidget(_minMinusButton);

    _minLabel = makeLabel(QString(), 11, controls);
    // 文案在「最短 0 分钟」和「最短 30 分钟」之间变，宽度按最长的给死，
    // 否则点加减时左右按钮会跟着文字长度来回跳。
    _minLabel->setMinimumWidth(74);
    _minLabel->setAlignment(Qt::AlignCenter);
    controlsLayout->addWidget(_minLabel);

    _minPlusButton = new ElaIconButton(ElaIconType::Plus, 10, 22, 22, controls);
    _minPlusButton->setBorderRadius(6);
    _minPlusButton->setToolTip(tr("收紧过滤（只看更长的时段）"));
    connect(_minPlusButton, &ElaIconButton::clicked, this,
            [this]() { adjustMinDuration(kMinDurationStepMs); });
    controlsLayout->addWidget(_minPlusButton);

    _totalLabel = makeLabel(QString(), 12, controls);
    _totalLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    _totalLabel->setMinimumWidth(120);
    controlsLayout->addWidget(_totalLabel);
    layout->addWidget(controls);

    // —— 时间轴全景条 ——
    auto* timelineCard = new ElaScrollPageArea(this);
    timelineCard->setMinimumHeight(0);
    timelineCard->setMaximumHeight(QWIDGETSIZE_MAX);
    timelineCard->setBorderRadius(12);
    auto* timelineLayout = new QVBoxLayout(timelineCard);
    timelineLayout->setContentsMargins(6, 4, 6, 4);
    _timeline = new DestinationDayTimelineWidget(timelineCard);
    timelineLayout->addWidget(_timeline);
    layout->addWidget(timelineCard);

    // —— 明细列表（内滚） ——
    _statusLabel = makeLabel(QString(), 12, this);
    _statusLabel->setWordWrap(true);
    _statusLabel->setVisible(false);
    layout->addWidget(_statusLabel);

    auto* recordsHost = new QWidget(this);
    _recordsLayout = new QVBoxLayout(recordsHost);
    _recordsLayout->setContentsMargins(0, 0, 0, 0);
    _recordsLayout->setSpacing(2);

    // 滚动条统一 Ela 浮层条：ElaScrollArea 内置滚动条被 AlwaysOff 关掉显示，
    // 外挂 ElaScrollBar 浮在视口右缘（与历史页活动流同一做法）。
    // 不能用 ScrollBarAsNeeded：那会让内置 ElaScrollBar 以布局内方式显示，
    // 丢掉浮层定位与样式，看起来就是一条原生窄条。
    auto* scroll = new ElaScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setIsGrabGesture(false);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("background:transparent;"));
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(recordsHost);
    layout->addWidget(scroll, 1);
    auto* floatBar = new ElaScrollBar(scroll->verticalScrollBar(), scroll);
    floatBar->setIsAnimation(true);

    _emptyLabel = makeLabel(QString(), 12, this);
    _emptyLabel->setAlignment(Qt::AlignCenter);
    _emptyLabel->setVisible(false);
    layout->addWidget(_emptyLabel);

    refreshThemeStyles(eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark);
}

void DestinationDetailDialog::refreshThemeStyles(bool dark)
{
    if (_subtitleLabel != nullptr) {
        _subtitleLabel->setStyleSheet(shared::secondaryTextStyle(dark));
    }
    if (_emptyLabel != nullptr) {
        _emptyLabel->setStyleSheet(shared::secondaryTextStyle(dark)
                                   + QStringLiteral(" padding:18px 0;"));
    }
    // 明细行的时间列是动态建的，重铺一次让它跟上（行数不多，代价可忽略）。
    if (_recordsLayout != nullptr && !_day.records.isEmpty()) {
        applyRecords();
    }
}

QString DestinationDetailDialog::secondaryStyle() const
{
    return shared::secondaryTextStyle(eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark);
}

QString DestinationDetailDialog::iconFallbackStyle() const
{
    // 没有分类色时的兜底块：用次要文字色，跟主题一起明暗。
    return QStringLiteral("background:%1; border-radius:8px;")
        .arg(shared::secondaryTextColor(eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark));
}

void DestinationDetailDialog::openFor(const DestinationTargetInfo& target, const QDate& date)
{
    _target = target;
    _date = date;
    _minDurationMs = 0;

    // 头部：应用维度显示 exe 名，分类维度显示成员提示。
    _titleLabel->setText(target.displayName);
    _subtitleLabel->setText(target.secondaryText);
    if (target.color.isValid()) {
        _iconLabel->setStyleSheet(
            QStringLiteral("background:%1; border-radius:8px;").arg(target.color.name()));
    } else {
        _iconLabel->setStyleSheet(iconFallbackStyle());
    }

    // 应用维度：优先画 exe 真图标，取不到才留色块。带 dpr 取图，
    // 高缩放屏上 30px 的格子要 60 物理像素，源不够就会糊。
    _iconLabel->setPixmap(QPixmap());
    if (target.target.mode == data::DestinationTarget::Mode::App) {
        const QPixmap icon = shared::appIcon(
            _database != nullptr ? resolveAppIconPath(target.target.key) : QString(), 30,
            devicePixelRatioF());
        if (!icon.isNull()) {
            _iconLabel->setPixmap(icon);
            _iconLabel->setStyleSheet(QString());
            _iconLabel->setFixedSize(30, 30);
            _iconLabel->setScaledContents(true);
        }
    }

    loadDay();
    // show() 而不是 exec()：由调用方决定模态性；这里是应用级模态。
    show();
    raise();
    activateWindow();
}

QString DestinationDetailDialog::resolveAppIconPath(const QString& exeName) const
{
    // 弹窗手里只有 exe 名（来源页传的是 key）；路径从 apps 表补齐。
    if (_database == nullptr || !_database->isOpen()) {
        return QString();
    }
    data::AppRepository apps(*_database);
    const data::Result<std::optional<data::App>> app = apps.findByExeName(exeName);
    return app.ok() && app.value.has_value() ? app.value->exePath : QString();
}

void DestinationDetailDialog::shiftDate(int days)
{
    const QDate next = _date.addDays(days);
    // 不能翻到未来。
    if (next > QDate::currentDate() || !next.isValid()) {
        return;
    }
    _date = next;
    loadDay();
}

void DestinationDetailDialog::adjustMinDuration(int delta)
{
    const qint64 next = std::clamp<qint64>(_minDurationMs + delta, 0, kMinDurationMaxMs);
    if (next == _minDurationMs) {
        return;
    }
    _minDurationMs = next;
    // 过滤是展示层的事：不重查库，在已有结果上重放。
    // updateControls 必须跟着刷：_minLabel 的「最短 N 分钟」就是在这里更新，
    // 漏了它点加减号数字永远不动。
    applyRecords();
    updateControls();
}

void DestinationDetailDialog::loadDay()
{
    clearData();

    if (_database == nullptr || !_database->isOpen()) {
        _statusLabel->setText(tr("数据库未就绪，无法读取详情。"));
        _statusLabel->setVisible(true);
        updateControls();
        return;
    }

    data::DestinationReadModel model(*_database);
    const data::Result<data::DestinationDetailDay> result =
        model.loadDay(_target.target, _date, 0);
    if (!result.ok()) {
        _statusLabel->setText(tr("读取详情失败：%1").arg(result.error));
        _statusLabel->setVisible(true);
        updateControls();
        return;
    }
    _day = result.value;

    // 全景条块：全部段（不受最短时长过滤影响）。
    QVector<DestinationTimelineBlock> blocks;
    blocks.reserve(_day.records.size());
    for (const data::DestinationRecord& record : _day.records) {
        // 记录已按标题样本切行；全景条按块合并同色相邻行没有意义（同色本就连着）。
        DestinationTimelineBlock block;
        block.startMs = record.startTimeMs;
        block.endMs = record.endTimeMs;
        block.color = _target.color.isValid()
            ? _target.color
            : shared::secondaryTextColor(eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark);
        block.isLive = record.isLive;
        blocks.append(block);
    }
    _timeline->setData(_day.dayStartMs, _day.dayEndMs, blocks);

    applyRecords();
    updateControls();
}

void DestinationDetailDialog::applyRecords()
{
    while (_recordsLayout->count() > 0) {
        QLayoutItem* item = _recordsLayout->takeAt(0);
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    qint64 filteredTotalMs = 0;
    int shown = 0;
    for (const data::DestinationRecord& record : _day.records) {
        if (_minDurationMs > 0 && record.durationMs < _minDurationMs) {
            continue;
        }
        ++shown;
        filteredTotalMs += record.durationMs;

        auto* row = new QWidget(_recordsLayout->parentWidget());
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 5, 8, 5);
        rowLayout->setSpacing(10);

        auto* timeLabel = new QLabel(
            QStringLiteral("%1 – %2").arg(formatClock(record.startTimeMs),
                                          record.isLive ? tr("进行中") : formatClock(record.endTimeMs)),
            row);
        timeLabel->setFixedWidth(120);
        QFont timeFont = timeLabel->font();
        timeFont.setPixelSize(11);
        timeLabel->setFont(timeFont);
        timeLabel->setStyleSheet(secondaryStyle());
        rowLayout->addWidget(timeLabel);

        QString titleText = record.title.trimmed().isEmpty()
            ? tr("（无标题时段）") : record.title.trimmed();
        // 分类维度必然要标来源应用；应用维度只在「这一行不是目标应用自己的」
        // 时才标——关联应用之后，父应用的详情里混着成员的段，不标出来
        // 就是一堆无主标题。未关联的应用 exeName 恒等于 target.key，行为不变。
        const bool isCategory = _target.target.mode == data::DestinationTarget::Mode::Category;
        const bool isOtherApp = record.exeName.trimmed().compare(_target.target.key.trimmed(),
                                                                 Qt::CaseInsensitive) != 0;
        if (!record.exeName.trimmed().isEmpty() && (isCategory || isOtherApp)) {
            titleText += QStringLiteral("  ·  %1").arg(record.exeName);
        }
        auto* titleLabel = new QLabel(titleText, row);
        titleLabel->setWordWrap(false);
        QFont titleFont = titleLabel->font();
        titleFont.setPixelSize(12);
        titleLabel->setFont(titleFont);
        rowLayout->addWidget(titleLabel, 1);

        auto* durationLabel = new QLabel(formatDurationOf(record.durationMs), row);
        durationLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        QFont durationFont = durationLabel->font();
        durationFont.setPixelSize(11);
        durationFont.setBold(true);
        durationLabel->setFont(durationFont);
        rowLayout->addWidget(durationLabel);

        _recordsLayout->addWidget(row);
    }
    _recordsLayout->addStretch();

    _totalLabel->setText(tr("当日合计 %1").arg(formatDurationLong(filteredTotalMs)));
    if (_day.records.isEmpty()) {
        _emptyLabel->setText(
            tr("这一天没有「%1」的记录。\n若刚改过应用的归属，曲线上的历史分布不会回溯——归属以当前为准。")
                .arg(_target.displayName));
        _emptyLabel->setVisible(true);
    } else if (shown == 0) {
        _emptyLabel->setText(tr("全部时段都短于 %1 分钟，放宽过滤看看。").arg(_minDurationMs / 60000));
        _emptyLabel->setVisible(true);
    } else {
        _emptyLabel->setVisible(false);
    }
}

void DestinationDetailDialog::updateControls()
{
    _dateLabel->setText(
        _date.toString(QStringLiteral("M 月 d 日"))
        + (_date == QDate::currentDate() ? tr("（今天）") : QString()));
    _nextDateButton->setEnabled(_date < QDate::currentDate());
    _todayButton->setEnabled(_date != QDate::currentDate());
    _minLabel->setText(tr("最短 %1 分钟").arg(_minDurationMs / 60000));
    _minMinusButton->setEnabled(_minDurationMs > 0);
    _minPlusButton->setEnabled(_minDurationMs < kMinDurationMaxMs);
}

void DestinationDetailDialog::clearData()
{
    _day = data::DestinationDetailDay();
    _timeline->clear();
    _statusLabel->setVisible(false);
    _emptyLabel->setVisible(false);
    while (_recordsLayout->count() > 0) {
        QLayoutItem* item = _recordsLayout->takeAt(0);
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
        delete item;
    }
}
void DestinationDetailDialog::hideEvent(QHideEvent* event)
{
    QDialog::hideEvent(event);
    // 关闭/隐藏即清数据：明细行与时间轴块都不留。
    clearData();
}

} // namespace patina::ui
