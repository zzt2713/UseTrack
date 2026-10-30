#include "ui/settings/settings_page.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QRadioButton>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>

#include <algorithm>

#include "ElaComboBox.h"
#include "ElaContentDialog.h"
#include "ElaPivot.h"
#include "ElaPushButton.h"
#include "ElaRadioButton.h"
#include "ElaScrollPageArea.h"
#include "ElaSlider.h"
#include "ElaText.h"
#include "ElaTheme.h"
#include "ElaToggleButton.h"
#include "ElaToggleSwitch.h"

#include "data/data_directory_migrator.h"
#include "data/export_service.h"
#include "data/import_service.h"
#include "shared/app_icon_cache.h"
#include "shared/theme_styles.h"
#include "ui/settings/settings_config.h"

namespace
{

constexpr int kPanelGap = 14;
constexpr int kBodyGap = 14;
constexpr int kContentMarginLeft = 0;
constexpr qint64 kDayMs = 24LL * 60 * 60 * 1000;
constexpr int kRecentBackupLimit = 5;
// 页面上的按钮统一尺寸。裸 QPushButton 走 Fusion 默认样式，
// 高度跟着字体度量飘，同一个页面里能出现三种高度，看着很散。
constexpr int kButtonHeight = 32;
constexpr int kButtonMinWidth = 84;
constexpr int kButtonRadius = 8;
// 壁纸透明度区间：0% 等于没开壁纸，放开只会让人以为坏了；上限 100% 保持原图。
constexpr int kWallpaperOpacityMin = 20;
constexpr int kWallpaperOpacityMax = 100;
constexpr int kWallpaperOpacityDefault = 100;

// 标签的「语义色」用一个动态属性记着，不直接写进样式表。
//
// 为什么不直接 setStyleSheet 一行颜色：这些标签的颜色会随状态变（校验通过变绿、
// 失败变红），而色值本身又要随明暗主题变。两件事叠在一起，散落在十几处的
// setStyleSheet 里就再也刷不回来了。记个角色，颜色统一由 applyLabelColor() 给。
constexpr char kLabelRoleProperty[] = "patinaRole"; // secondary | danger | success | neutral

bool currentThemeDark()
{
    return eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark;
}

// 按标签上记的角色给颜色。建标签时调一次，主题切换时整页再刷一遍。
void applyLabelColor(QLabel* label, bool dark)
{
    if (label == nullptr) {
        return;
    }
    const QString role = label->property(kLabelRoleProperty).toString();
    if (role.isEmpty()) {
        return;
    }
    if (role == QLatin1String("secondary")) {
        // 这个匿名 namespace 在全局作用域，`shared::` 找不到 patina::shared，
        // 必须写全限定名（页面成员函数里能省略是因为它们已在 patina::ui 内）。
        label->setStyleSheet(patina::shared::secondaryTextStyle(dark));
    } else if (role == QLatin1String("danger")) {
        label->setStyleSheet(patina::shared::dangerTextStyle(dark));
    } else if (role == QLatin1String("success")) {
        label->setStyleSheet(patina::shared::successTextStyle(dark));
    } else if (role == QLatin1String("primary")) {
        // 数值标签：显式跟主题的正文色，不依赖全局调色板（调色板与 Ela 主题
        // 的同步链有断点时，深色下会出现黑字压深底）。
        label->setStyleSheet(
            QStringLiteral("color:%1;").arg(patina::shared::primaryTextColor(dark)));
    } else {
        // neutral：清空样式表，回到调色板给的正文色。
        label->setStyleSheet(QString());
    }
}

void setLabelRole(QLabel* label, const char* role)
{
    if (label != nullptr) {
        label->setProperty(kLabelRoleProperty, QString::fromLatin1(role));
        applyLabelColor(label, currentThemeDark());
    }
}

// 统一按同一套尺寸建按钮：Ela 的按钮自带圆角与 hover，裸 Qt 按钮不行。
ElaPushButton* makeButton(const QString& text, QWidget* parent, int minWidth = kButtonMinWidth)
{
    auto* button = new ElaPushButton(text, parent);
    button->setFixedHeight(kButtonHeight);
    button->setMinimumWidth(minWidth);
    button->setBorderRadius(kButtonRadius);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

// 文字按钮：用在「打开」这类次要动作上。不再是超链接样式（下划线链接
// 和页面里的一排实心按钮混着很乱），统一走和其它按钮一样的实心样式，
// 只是矮一点（26px），视觉上是次级动作。
QPushButton* makeFlatButton(const QString& text, QWidget* parent)
{
    auto* button = new ElaPushButton(text, parent);
    button->setFixedHeight(26);
    button->setMinimumWidth(64);
    button->setBorderRadius(8);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

// 所有界面文字都要过 tr()，这里只集中放不随状态变化的常量文案。
QString formatBytes(qint64 bytes)
{
    if (bytes < 0) {
        return QStringLiteral("—");
    }
    const double kilobytes = static_cast<double>(bytes) / 1024.0;
    if (kilobytes < 1024.0) {
        return QStringLiteral("%1 KB").arg(kilobytes, 0, 'f', kilobytes < 10.0 ? 1 : 0);
    }
    const double megabytes = kilobytes / 1024.0;
    if (megabytes < 1024.0) {
        return QStringLiteral("%1 MB").arg(megabytes, 0, 'f', 1);
    }
    return QStringLiteral("%1 GB").arg(megabytes / 1024.0, 0, 'f', 2);
}

QString formatCount(qint64 count)
{
    // 四位以上加千分位：数据库里动辄几万条，裸数字读起来要数位数。
    if (count < 10000) {
        return QString::number(count);
    }
    QString digits = QString::number(count);
    QString grouped;
    int seen = 0;
    for (int index = digits.size() - 1; index >= 0; --index) {
        grouped.prepend(digits.at(index));
        if (++seen % 3 == 0 && index > 0) {
            grouped.prepend(QLatin1Char(','));
        }
    }
    return grouped;
}

ElaText* makeLabel(const QString& text, int pixelSize, QWidget* parent, bool strong = false)
{
    auto* label = new ElaText(text, pixelSize, parent);
    label->setWordWrap(false);
    if (strong) {
        label->setTextStyle(ElaTextType::BodyStrong);
        // setTextStyle 会改字号，必须在它之后再设一次像素大小。
        label->setTextPixelSize(pixelSize);
    }
    return label;
}

// ElaScrollPageArea 的构造里写死了 setFixedHeight(75)，当容器用时必须先解开。
ElaScrollPageArea* makeCard(QWidget* parent)
{
    auto* card = new ElaScrollPageArea(parent);
    card->setMinimumHeight(0);
    card->setMaximumHeight(QWIDGETSIZE_MAX);
    card->setBorderRadius(14);
    return card;
}

QLabel* makeValueLabel(QWidget* parent, int pixelSize = 13)
{
    auto* label = new QLabel(QStringLiteral("—"), parent);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont font = label->font();
    font.setPixelSize(pixelSize);
    font.setWeight(QFont::DemiBold);
    label->setFont(font);
    // 数值标签不给 role（applyLabelColor 对无 role 的跳过）——但那样它就靠
    // 全局调色板取色，而调色板与 Ela 主题的同步链一旦有断点（运行时切主题、
    // 启动时库未开），深色下就是黑字压深底。这里直接定色：
    // 无 role 时按当前主题给正文色，并靠 role 机制在主题切换时跟着重刷。
    setLabelRole(label, "primary");
    return label;
}

QLabel* makeCaptionLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    QFont font = label->font();
    font.setPixelSize(11);
    label->setFont(font);
    setLabelRole(label, "secondary");
    return label;
}

// 说明行：左边一句主文案加一句小字，右边放控件。
QWidget* makeRow(const QString& title, const QString& detail, QWidget* control, QWidget* parent)
{
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto* textBox = new QWidget(row);
    auto* textLayout = new QVBoxLayout(textBox);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(2);
    textLayout->addWidget(makeLabel(title, 12, textBox, true));
    if (!detail.isEmpty()) {
        textLayout->addWidget(makeCaptionLabel(detail, textBox));
    }
    layout->addWidget(textBox, 1);
    if (control != nullptr) {
        layout->addWidget(control, 0, Qt::AlignRight | Qt::AlignVCenter);
    }
    return row;
}

// 校验清单里的一行：左边一个记号（✓ / ✗ / ·），右边一句说明。
QWidget* makeCheckRow(QLabel** markOut, QLabel** textOut, QWidget* parent)
{
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* mark = new QLabel(QStringLiteral("·"), row);
    mark->setFixedWidth(12);
    QFont markFont = mark->font();
    markFont.setPixelSize(12);
    markFont.setWeight(QFont::DemiBold);
    mark->setFont(markFont);
    setLabelRole(mark, "secondary");
    layout->addWidget(mark, 0, Qt::AlignTop);

    auto* text = new QLabel(row);
    text->setWordWrap(true);
    QFont textFont = text->font();
    textFont.setPixelSize(11);
    text->setFont(textFont);
    layout->addWidget(text, 1);

    if (markOut != nullptr) {
        *markOut = mark;
    }
    if (textOut != nullptr) {
        *textOut = text;
    }
    return row;
}

// 校验行三态：通过（✓ 正文色）/ 失败（✗ 红）/ 未校验（· 次要灰）。
// 「还没选择文件」这类是中性状态，不该和真正的校验失败一样标红。
void setCheckRow(QLabel* mark, QLabel* text, int state, const QString& message)
{
    // state: 1 = 通过，0 = 失败，-1 = 未校验。
    if (mark != nullptr) {
        if (state > 0) {
            mark->setText(QStringLiteral("✓"));
            setLabelRole(mark, "success");
        } else if (state == 0) {
            mark->setText(QStringLiteral("✗"));
            setLabelRole(mark, "danger");
        } else {
            mark->setText(QStringLiteral("·"));
            setLabelRole(mark, "secondary");
        }
    }
    if (text != nullptr) {
        text->setText(message);
        // 通过时回到正文色，失败时整行标红，未校验时次要灰——
        // 让人一眼分清「没做」和「做错了」。
        setLabelRole(text, state > 0 ? "neutral" : state == 0 ? "danger" : "secondary");
    }
}

void clearLayout(QVBoxLayout* layout)
{
    // 常在自己的槽里被调用：直接 delete 可能删掉正在处理事件的控件。
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

QVector<QFileInfo> listRecentBackups(const QString& directory, int limit)
{
    QVector<QFileInfo> files;
    const QDir dir(directory);
    if (!dir.exists()) {
        return files;
    }
    const QFileInfoList entries =
        dir.entryInfoList(QStringList{QStringLiteral("UseTrack-backup-*.db")}, QDir::Files, QDir::Time);
    for (const QFileInfo& entry : entries) {
        if (files.size() >= limit) {
            break;
        }
        files.append(entry);
    }
    return files;
}

qint64 directorySize(const QString& directory)
{
    const QDir dir(directory);
    if (!dir.exists()) {
        return -1;
    }
    qint64 total = 0;
    const QFileInfoList entries = dir.entryInfoList(QDir::Files);
    for (const QFileInfo& entry : entries) {
        total += entry.size();
    }
    return total;
}

} // namespace

namespace patina::ui
{

SettingsPage::SettingsPage(data::Database* database, QWidget* parent)
    : QWidget(parent), _database(database)
{
    buildUi();

    // 本页所有次要色、危险色都来自标签上的 role 属性，切主题时整页刷一遍即可。
    connect(eTheme, &ElaTheme::themeModeChanged, this, [this](ElaThemeType::ThemeMode mode) {
        const bool dark = mode == ElaThemeType::ThemeMode::Dark;
        applyThemeColors(dark);
        shared::applyComboBoxTheme(_dataExportFormatBox, dark);
        // 分段按钮的颜色在样式表里，主题一换要整组重刷。
        refreshThemeModeControl();
        refreshBehaviorSegments();
    });
}

void SettingsPage::applyThemeColors(bool dark)
{
    // 走一遍子控件而不是维护一张名单：这张页面里的标签会随状态反复新建
    // （备份列表、导入批次都靠 clearLayout 重建），名单一漏就永远是旧颜色。
    const QList<QLabel*> labels = findChildren<QLabel*>();
    for (QLabel* label : labels) {
        applyLabelColor(label, dark);
    }
    refreshDropAreaStyle(dark);
    // 壁纸区块与主题模式之间的分隔线：QFrame 不吃 role 机制，单独上色。
    const QList<QFrame*> dividers = findChildren<QFrame*>(QStringLiteral("wallpaperDivider"));
    for (QFrame* divider : dividers) {
        divider->setStyleSheet(
            QStringLiteral("#wallpaperDivider { border:none; background:%1; }")
                .arg(shared::borderColor(dark)));
    }
}

void SettingsPage::refreshDropAreaStyle(bool dark)
{
    if (_dropArea == nullptr) {
        return;
    }
    _dropArea->setStyleSheet(
        QStringLiteral("#settingsDropArea { border:1px dashed %1; border-radius:10px;"
                       " background:transparent; }")
            .arg(shared::borderColor(dark)));
}

SettingsPage::~SettingsPage() = default;

void SettingsPage::buildUi()
{
    // 外层 ElaScrollPage 已经提供滚动容器，本页只做布局。
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 16, 24, 24);
    layout->setSpacing(kBodyGap);

    auto* header = new QWidget(this);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(12);

    auto* titleBox = new QWidget(header);
    auto* titleLayout = new QVBoxLayout(titleBox);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(2);
    titleLayout->addWidget(makeLabel(tr("设置"), 19, titleBox, true));
    titleLayout->addWidget(makeLabel(tr("数据全部保存在本机，导出与恢复都是本地文件操作"), 12, titleBox));
    headerLayout->addWidget(titleBox, 1);

    layout->addWidget(header);

    _statusLabel = makeLabel(QString(), 13, this);
    _statusLabel->setWordWrap(true);
    _statusLabel->setVisible(false);
    layout->addWidget(_statusLabel);

    // ---- 保存栏：三面板「攒草稿 → 保存」的 header ----
    layout->addWidget(buildSaveBar());
    // ---- 三面板：追踪 / 外观 / 常驻 ----
    // 竖排堆叠：横排时三个面板的最小宽度会顶破视口，外层 ElaScrollPage
    // 出现横向可滚内容，QScroller 的拖拽手势就能左右滑。
    // 竖排后内容宽度始终贴合视口，只保留纵向滚动。
    auto* panels = new QWidget(this);
    auto* panelsLayout = new QVBoxLayout(panels);
    panelsLayout->setContentsMargins(kContentMarginLeft, 0, 0, 0);
    panelsLayout->setSpacing(kPanelGap);
    panelsLayout->addWidget(buildTrackingPanel());
    panelsLayout->addWidget(buildAppearancePanel());
    panelsLayout->addWidget(buildResidentPanel());
    layout->addWidget(panels);

    auto* body = new QWidget(this);
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(kContentMarginLeft, 0, 0, 0);
    bodyLayout->setSpacing(kPanelGap);

    auto* leftColumn = new QWidget(body);
    auto* leftLayout = new QVBoxLayout(leftColumn);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(kPanelGap);
    leftLayout->addWidget(buildStorageCard());
    leftLayout->addWidget(buildDiagnosticsCard());
    leftLayout->addWidget(buildRecentBackupCard());
    leftLayout->addWidget(buildCleanupCard());
    // 开机自启迁入常驻面板，原独立「启动」卡不再挂载。
    leftLayout->addStretch();

    auto* rightColumn = new QWidget(body);
    auto* rightLayout = new QVBoxLayout(rightColumn);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(kPanelGap);
    rightLayout->addWidget(buildExportCard());
    rightLayout->addWidget(buildDataImportCard());
    rightLayout->addWidget(buildRestoreCard());
    rightLayout->addStretch();

    bodyLayout->addWidget(leftColumn, 115);
    bodyLayout->addWidget(rightColumn, 100);
    layout->addWidget(body, 1);

    _exportPath = data::BackupService::defaultBackupDirectory() + QLatin1Char('/')
        + data::BackupService::defaultBackupFileName(QDateTime::currentMSecsSinceEpoch());
    loadThreePanelSettings();
    refreshAll();
}

QWidget* SettingsPage::buildStorageCard()
{
    auto* card = makeCard(this);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* head = new QWidget(card);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->addWidget(makeLabel(tr("存储"), 14, head, true), 1);
    headLayout->addWidget(makeCaptionLabel(tr("本地"), head), 0, Qt::AlignRight);
    layout->addWidget(head);

    // 路径行：目录本身可选中复制，旁边一个「打开」直接调资源管理器。
    auto* pathRow = new QWidget(card);
    auto* pathLayout = new QHBoxLayout(pathRow);
    pathLayout->setContentsMargins(0, 0, 0, 0);
    pathLayout->setSpacing(8);
    _dataDirLabel = new QLabel(pathRow);
    _dataDirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _dataDirLabel->setWordWrap(true);
    QFont mono = _dataDirLabel->font();
    mono.setPixelSize(11);
    _dataDirLabel->setFont(mono);
    setLabelRole(_dataDirLabel, "secondary");
    pathLayout->addWidget(_dataDirLabel, 1);

    auto* openButton = makeFlatButton(tr("打开"), pathRow);
    connect(openButton, &QPushButton::clicked, this, &SettingsPage::onOpenDataDirectory);
    pathLayout->addWidget(openButton, 0, Qt::AlignVCenter);
    layout->addWidget(pathRow);

    auto* stats = new QWidget(card);
    auto* statsLayout = new QHBoxLayout(stats);
    statsLayout->setContentsMargins(0, 0, 0, 0);
    statsLayout->setSpacing(18);

    const auto addStat = [&](const QString& caption, QLabel** valueOut) {
        auto* box = new QWidget(stats);
        auto* boxLayout = new QVBoxLayout(box);
        boxLayout->setContentsMargins(0, 0, 0, 0);
        boxLayout->setSpacing(2);
        boxLayout->addWidget(makeCaptionLabel(caption, box));
        QLabel* value = makeValueLabel(box);
        boxLayout->addWidget(value);
        statsLayout->addWidget(box, 1);
        *valueOut = value;
    };
    addStat(tr("数据库"), &_databaseSizeLabel);
    addStat(tr("备份目录"), &_backupDirSizeLabel);
    addStat(tr("活动段"), &_sessionCountLabel);
    layout->addWidget(stats);

    _dataDirHint = makeCaptionLabel(QString(), card);
    _dataDirHint->setWordWrap(true);
    layout->addWidget(_dataDirHint);

    auto* changeButton = makeButton(tr("更改…"), card);
    connect(changeButton, &QPushButton::clicked, this, &SettingsPage::onChooseDataDirectory);
    layout->addWidget(makeRow(tr("数据目录"), tr("更改时库文件随目录一起搬过去"), changeButton, card));

    return card;
}

QWidget* SettingsPage::buildDiagnosticsCard()
{
    auto* card = makeCard(this);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* head = new QWidget(card);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->setSpacing(8);
    headLayout->addWidget(makeLabel(tr("诊断"), 14, head, true), 1);
    headLayout->addWidget(makeCaptionLabel(tr("手动刷新"), head), 0, Qt::AlignRight);

    auto* refreshButton = makeFlatButton(tr("刷新"), head);
    connect(refreshButton, &QPushButton::clicked, this, &SettingsPage::refreshDiagnostics);
    headLayout->addWidget(refreshButton, 0, Qt::AlignVCenter);
    layout->addWidget(head);

    // 三列读数：内存 / 句柄 / 缓存。数值标签沿用 makeValueLabel（DemiBold + role primary）。
    const auto addStat = [&](QWidget* row, const QString& caption, QLabel** valueOut) {
        auto* box = new QWidget(row);
        auto* boxLayout = new QVBoxLayout(box);
        boxLayout->setContentsMargins(0, 0, 0, 0);
        boxLayout->setSpacing(2);
        boxLayout->addWidget(makeCaptionLabel(caption, box));
        QLabel* value = makeValueLabel(box);
        boxLayout->addWidget(value);
        *valueOut = value;
        return box;
    };

    auto* memoryRow = new QWidget(card);
    auto* memoryLayout = new QHBoxLayout(memoryRow);
    memoryLayout->setContentsMargins(0, 0, 0, 0);
    memoryLayout->setSpacing(18);
    memoryLayout->addWidget(addStat(memoryRow, tr("工作集"), &_workingSetLabel), 1);
    memoryLayout->addWidget(addStat(memoryRow, tr("私有提交"), &_privateBytesLabel), 1);
    memoryLayout->addWidget(addStat(memoryRow, tr("句柄"), &_handleCountLabel), 1);
    layout->addWidget(memoryRow);

    auto* cacheRow = new QWidget(card);
    auto* cacheLayout = new QHBoxLayout(cacheRow);
    cacheLayout->setContentsMargins(0, 0, 0, 0);
    cacheLayout->setSpacing(18);
    cacheLayout->addWidget(addStat(cacheRow, tr("图标缓存"), &_iconCacheLabel), 1);
    cacheLayout->addWidget(addStat(cacheRow, tr("图表缓存"), &_chartCacheLabel), 1);
    cacheLayout->addWidget(addStat(cacheRow, tr("库文件"), &_databaseFileLabel), 1);
    layout->addWidget(cacheRow);

    // 看门狗三时间戳：谁最后动过手，出问题时先看这三个的相对先后。
    auto* healthRow = new QWidget(card);
    auto* healthLayout = new QHBoxLayout(healthRow);
    healthLayout->setContentsMargins(0, 0, 0, 0);
    healthLayout->setSpacing(18);
    healthLayout->addWidget(addStat(healthRow, tr("最后心跳"), &_healthHeartbeatLabel), 1);
    healthLayout->addWidget(addStat(healthRow, tr("最后一次采样"), &_healthSampleLabel), 1);
    healthLayout->addWidget(addStat(healthRow, tr("最后一次看门狗封口"), &_healthSealLabel), 1);
    layout->addWidget(healthRow);

    auto* hint = makeCaptionLabel(
        tr("工作集与任务管理器的「内存」同源；数字只在点「刷新」时重取，不自动轮询。"), card);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    return card;
}

QWidget* SettingsPage::buildRecentBackupCard()
{
    auto* card = makeCard(this);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* head = new QWidget(card);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->addWidget(makeLabel(tr("最近备份"), 14, head, true), 1);
    layout->addWidget(head);

    auto* listHost = new QWidget(card);
    _recentLayout = new QVBoxLayout(listHost);
    _recentLayout->setContentsMargins(0, 0, 0, 0);
    _recentLayout->setSpacing(6);
    layout->addWidget(listHost);

    _recentEmptyLabel = makeCaptionLabel(tr("备份目录里还没有备份文件。"), card);
    _recentEmptyLabel->setWordWrap(true);
    layout->addWidget(_recentEmptyLabel);

    auto* note = makeCaptionLabel(tr("备份只在你点「导出」时生成，不会自动跑。"), card);
    note->setWordWrap(true);
    layout->addWidget(note);

    return card;
}

// ---------------- 三面板 ----------------

// 步进控件的一行：− [值] ＋。step 是每次加减的秒数（内部存秒、显示分钟）。
QWidget* makeStepper(ElaPushButton** minusOut, QLabel** valueOut, ElaPushButton** plusOut,
                     QWidget* parent)
{
    auto* host = new QWidget(parent);
    auto* layout = new QHBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* minus = new ElaPushButton(QStringLiteral("−"), host);
    minus->setFixedSize(24, 24);
    minus->setBorderRadius(6);
    minus->setCursor(Qt::PointingHandCursor);
    layout->addWidget(minus);

    auto* value = makeValueLabel(host, 12);
    value->setAlignment(Qt::AlignCenter);
    value->setMinimumWidth(64);
    layout->addWidget(value);

    auto* plus = new ElaPushButton(QStringLiteral("＋"), host);
    plus->setFixedSize(24, 24);
    plus->setBorderRadius(6);
    plus->setCursor(Qt::PointingHandCursor);
    layout->addWidget(plus);

    *minusOut = minus;
    *valueOut = value;
    *plusOut = plus;
    return host;
}

// 分段组的一枚按钮（ElaToggleButton：库内置互斥选中样式，
// 按压缩放 / hover 渐变 / 主题色全由库绘制，与其它 Ela 控件观感一致）。
ElaToggleButton* makeSegmentButton(const QString& text, QWidget* parent)
{
    auto* button = new ElaToggleButton(text, parent);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

// 整组互斥：只把 selected 设为选中，其余清掉。
//
// 必须带值守卫：库的 setIsToggled 无条件 Q_EMIT toggled（见 ElaToggleButton.cpp），
// 无守卫的写入会在「点击 → 处理器 → setIsToggled → 再 emit」里无限递归，
// 栈溢出闪退。值相同的按钮直接跳过，递归链就断了。
void SettingsPage::setSegmentChecked(ElaToggleButton* selected,
                                     QVector<ElaToggleButton*> group)
{
    for (ElaToggleButton* button : group) {
        if (button == nullptr) {
            continue;
        }
        const bool shouldBe = button == selected;
        if (button->getIsToggled() != shouldBe) {
            button->setIsToggled(shouldBe);
        }
    }
}

// 分段组的 toggled 信号接线：选中 → 组内互斥 + 执行动作；
// 取消选中（点击了已选中项，库会把它翻成 false）→ 分段不允许空选，摆回去。
// 摆回去的 setIsToggled(true) 会再 emit toggled(true)，走 checked 分支，
// 值守卫保证只执行一次动作。
void SettingsPage::bindSegment(ElaToggleButton* button, QVector<ElaToggleButton*> group,
                               const std::function<void()>& onActivated)
{
    connect(button, &ElaToggleButton::toggled, this,
            [this, button, group, onActivated](bool checked) {
                if (_loading) {
                    return;
                }
                if (checked) {
                    setSegmentChecked(button, group);
                    if (onActivated) {
                        onActivated();
                    }
                    return;
                }
                bool anyToggled = false;
                for (ElaToggleButton* item : group) {
                    if (item != nullptr && item->getIsToggled()) {
                        anyToggled = true;
                        break;
                    }
                }
                if (!anyToggled) {
                    button->setIsToggled(true);
                }
            });
}

QWidget* SettingsPage::buildSaveBar()
{
    auto* bar = makeCard(this);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(16, 10, 16, 10);
    layout->setSpacing(12);

    _dirtyLabel = makeLabel(tr("有未保存的改动"), 12, bar);
    layout->addWidget(_dirtyLabel);
    layout->addStretch();

    _cancelButton = makeButton(tr("取消"), bar, 72);
    connect(_cancelButton, &QPushButton::clicked, this, &SettingsPage::discardThreePanelDraft);
    layout->addWidget(_cancelButton);

    _saveButton = makeButton(tr("保存"), bar, 72);
    connect(_saveButton, &QPushButton::clicked, this, &SettingsPage::saveThreePanelDraft);
    layout->addWidget(_saveButton);

    refreshSaveBar();
    return bar;
}

QWidget* SettingsPage::buildTrackingPanel()
{
    auto* card = makeCard(this);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* head = new QWidget(card);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->addWidget(makeLabel(tr("追踪"), 14, head, true), 1);
    headLayout->addWidget(makeCaptionLabel(tr("保存后 5s 内生效"), head), 0, Qt::AlignRight);
    layout->addWidget(head);

    layout->addWidget(makeCaptionLabel(tr("时间规则"), card));

    // 合并间隔（timeline_merge_gap_secs，兼 AFK 阈值）：
    // 文案行 + 右对齐的步进器，两行各占一项。
    layout->addWidget(makeRow(tr("活动段合并间隔"),
                              tr("切走又切回不超过该间隔视为同一段；AFK 阈值同源"),
                              nullptr, card));
    auto* mergeHost = new QWidget(card);
    auto* mergeLayout = new QHBoxLayout(mergeHost);
    mergeLayout->setContentsMargins(0, 0, 0, 0);
    mergeLayout->addStretch();
    mergeLayout->addWidget(makeStepper(&_mergeGapMinus, &_mergeGapValue, &_mergeGapPlus,
                                       mergeHost));
    layout->addWidget(mergeHost);

    // 空闲超时（idle_timeout_secs）。
    layout->addWidget(makeRow(tr("空闲超时"),
                              tr("无输入超过该时长，当前段封口不再计时"),
                              nullptr, card));
    auto* idleHost = new QWidget(card);
    auto* idleLayout = new QHBoxLayout(idleHost);
    idleLayout->setContentsMargins(0, 0, 0, 0);
    idleLayout->addStretch();
    idleLayout->addWidget(makeStepper(&_idleMinus, &_idleValue, &_idlePlus, idleHost));
    layout->addWidget(idleHost);

    layout->addWidget(makeCaptionLabel(tr("记录选项"), card));

    _trackingPausedSwitch = new ElaToggleSwitch(card);
    connect(_trackingPausedSwitch, &ElaToggleSwitch::toggled, this, [this](bool checked) {
        if (!_loading) {
            _draftTrackingPaused = checked;
            refreshSaveBar();
        }
    });
    layout->addWidget(makeRow(tr("暂停追踪"),
                              tr("暂停期间不记录任何活动；现有未闭合段立即封口"),
                              _trackingPausedSwitch, card));

    _titleRecordingSwitch = new ElaToggleSwitch(card);
    connect(_titleRecordingSwitch, &ElaToggleSwitch::toggled, this, [this](bool checked) {
        if (!_loading) {
            _draftTitleRecording = checked;
            refreshSaveBar();
        }
    });
    layout->addWidget(makeRow(tr("记录窗口标题"),
                              tr("关闭后不记录任何应用的窗口标题，历史里只剩应用名与时长"),
                              _titleRecordingSwitch, card));

    layout->addStretch();

    const auto stepMerge = [this](int deltaSeconds) {
        // 范围 60s..3600s。步长必须与显示粒度一致（1 分钟）：
        // 之前步长 30s 而显示按分钟取整，一半点击落在同一个分钟档里，
        // 看起来就是「按两下数字才动一次」。
        const qint64 next = std::clamp<qint64>(static_cast<qint64>(_draftMergeGapSecs) + deltaSeconds,
                                               60, 3600);
        _draftMergeGapSecs = static_cast<int>(next);
        _mergeGapValue->setText(tr("%1 分钟").arg(static_cast<int>(next / 60)));
        refreshSaveBar();
    };
    connect(_mergeGapMinus, &QPushButton::clicked, this, [stepMerge]() { stepMerge(-60); });
    connect(_mergeGapPlus, &QPushButton::clicked, this, [stepMerge]() { stepMerge(60); });

    const auto stepIdle = [this](int deltaSeconds) {
        // 范围 60s..7200s，步长 1 分钟（与显示粒度一致）。
        const qint64 next = std::clamp<qint64>(static_cast<qint64>(_draftIdleTimeoutSecs) + deltaSeconds,
                                               60, 7200);
        _draftIdleTimeoutSecs = static_cast<int>(next);
        _idleValue->setText(tr("%1 分钟").arg(static_cast<int>(next / 60)));
        refreshSaveBar();
    };
    connect(_idleMinus, &QPushButton::clicked, this, [stepIdle]() { stepIdle(-60); });
    connect(_idlePlus, &QPushButton::clicked, this, [stepIdle]() { stepIdle(60); });

    return card;
}

QWidget* SettingsPage::buildAppearancePanel()
{
    auto* card = makeCard(this);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* head = new QWidget(card);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->addWidget(makeLabel(tr("外观"), 14, head, true), 1);
    headLayout->addWidget(makeCaptionLabel(tr("保存后生效"), head), 0, Qt::AlignRight);
    layout->addWidget(head);

    // 主题模式：Ela 只有 Light/Dark 两档，「跟随系统」由本层监听
    // QStyleHints::colorSchemeChanged 换算，设置键存三值。
    layout->addWidget(makeRow(tr("主题模式"),
                              tr("跟随系统：系统深浅色变化时自动切换"),
                              nullptr, card));
    auto* segmentHost = new QWidget(card);
    auto* segmentLayout = new QHBoxLayout(segmentHost);
    segmentLayout->setContentsMargins(0, 0, 0, 0);
    segmentLayout->setSpacing(6);
    _themeLightButton = makeSegmentButton(tr("浅色"), segmentHost);
    _themeDarkButton = makeSegmentButton(tr("深色"), segmentHost);
    _themeSystemButton = makeSegmentButton(tr("跟随系统"), segmentHost);
    segmentLayout->addWidget(_themeLightButton);
    segmentLayout->addWidget(_themeDarkButton);
    segmentLayout->addWidget(_themeSystemButton);
    layout->addWidget(segmentHost, 0, Qt::AlignRight);

    // 外观也走草稿 —— 三个面板共用同一根保存条，点选就落库会让「取消」
    // 只回滚追踪面板，外观却已经改了。主题真正切换在 saveThreePanelDraft() 里发信号。
    bindSegment(_themeLightButton, {_themeLightButton, _themeDarkButton, _themeSystemButton},
                [this]() {
                    _draftThemeMode = QStringLiteral("light");
                    refreshSaveBar();
                });
    bindSegment(_themeDarkButton, {_themeLightButton, _themeDarkButton, _themeSystemButton},
                [this]() {
                    _draftThemeMode = QStringLiteral("dark");
                    refreshSaveBar();
                });
    bindSegment(_themeSystemButton, {_themeLightButton, _themeDarkButton, _themeSystemButton},
                [this]() {
                    _draftThemeMode = QStringLiteral("system");
                    refreshSaveBar();
                });

    // 动效开关：ElaTheme 未提供独立动效开关（无 setAnimations 之类接口），
    // 这里不自建体系，只留一个说明占位不显示。

    // 壁纸：与上面的主题模式不是一套编辑模型 —— 开关、换源、换图都当场生效，
    // 混进草稿会出现「图已经换了但按取消又变回去」的矛盾，所以单独一块即时区。
    layout->addWidget(buildWallpaperBlock(card));

    layout->addStretch();
    return card;
}

QWidget* SettingsPage::buildWallpaperBlock(QWidget* parent)
{
    auto* block = new QWidget(parent);
    auto* layout = new QVBoxLayout(block);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    // 与主题模式之间拉一条分隔，明确「上面保存后生效 / 下面即时生效」。
    auto* divider = new QFrame(block);
    divider->setFrameShape(QFrame::HLine);
    divider->setFixedHeight(1);
    divider->setObjectName(QStringLiteral("wallpaperDivider"));
    layout->addWidget(divider);
    layout->addWidget(makeCaptionLabel(tr("壁纸 · 即时生效"), block));

    _wallpaperSwitch = new ElaToggleSwitch(block);
    connect(_wallpaperSwitch, &ElaToggleSwitch::toggled, this, [this](bool checked) {
        if (_loading) {
            return;
        }
        _wallpaperRefreshButton->setEnabled(checked);
        // 打开：盘上已有图就直接复用，没有才由 MainWindow 去下一张。
        // 关掉：撤图并落库。两条都只发一个信号，下载与否由 MainWindow 判断。
        emit wallpaperSettingsChanged(checked, _wallpaperSource);
    });
    layout->addWidget(makeRow(tr("随机壁纸"),
                              tr("每次换图覆盖同一份文件，同时只留一张"),
                              _wallpaperSwitch, block));

    // 来源单选：两个接口站返回的图都在 1920 级别，选哪个只是口味差别。
    auto* sourceHost = new QWidget(block);
    auto* sourceLayout = new QHBoxLayout(sourceHost);
    sourceLayout->setContentsMargins(0, 0, 0, 0);
    sourceLayout->setSpacing(16);
    _wallpaperLoliApiButton = new ElaRadioButton(tr("LoliApi"), sourceHost);
    _wallpaperUapisButton = new ElaRadioButton(tr("uapis"), sourceHost);
    sourceLayout->addWidget(_wallpaperLoliApiButton);
    sourceLayout->addWidget(_wallpaperUapisButton);
    sourceLayout->addStretch();
    layout->addWidget(makeRow(tr("壁纸来源"),
                              tr("两个公开接口二选一，只下载图片本身"),
                              sourceHost, block));

    auto onSourcePicked = [this](const QString& source) {
        if (_loading || _wallpaperSource == source) {
            return;
        }
        _wallpaperSource = source;
        emit wallpaperSettingsChanged(_wallpaperSwitch->getIsToggled(), source);
        if (_wallpaperSwitch->getIsToggled()) {
            emit wallpaperRefreshRequested(source);
        }
    };
    connect(_wallpaperLoliApiButton, &QRadioButton::toggled, this, [onSourcePicked](bool checked) {
        if (checked) {
            onSourcePicked(QStringLiteral("loliapi"));
        }
    });
    connect(_wallpaperUapisButton, &QRadioButton::toggled, this, [onSourcePicked](bool checked) {
        if (checked) {
            onSourcePicked(QStringLiteral("uapis"));
        }
    });

    auto* actionHost = new QWidget(block);
    auto* actionLayout = new QHBoxLayout(actionHost);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(12);
    actionLayout->addStretch();
    _wallpaperStatusLabel = makeCaptionLabel(QString(), actionHost);
    actionLayout->addWidget(_wallpaperStatusLabel);
    _wallpaperRefreshButton = makeButton(tr("换一张"), actionHost, 96);
    connect(_wallpaperRefreshButton, &QPushButton::clicked, this, [this]() {
        emit wallpaperRefreshRequested(_wallpaperSource);
    });
    actionLayout->addWidget(_wallpaperRefreshButton);
    layout->addWidget(actionHost);

    // 透明度：滑块要能看清刻度，所以右边跟一个百分数，并和滑块同宽排布。
    auto* opacityHost = new QWidget(block);
    auto* opacityLayout = new QHBoxLayout(opacityHost);
    opacityLayout->setContentsMargins(0, 0, 0, 0);
    opacityLayout->setSpacing(12);
    _wallpaperOpacitySlider = new ElaSlider(Qt::Horizontal, opacityHost);
    _wallpaperOpacitySlider->setRange(kWallpaperOpacityMin, kWallpaperOpacityMax);
    // QSlider 构造值是 0，setRange 会把它夹到下限，所以这里必须显式给默认值，
    // 否则控件建出来先站在 20%，要等 loadThreePanelSettings 再拨上来。
    _wallpaperOpacitySlider->setValue(kWallpaperOpacityDefault);
    _wallpaperOpacitySlider->setSingleStep(5);
    _wallpaperOpacitySlider->setPageStep(10);
    _wallpaperOpacitySlider->setFixedWidth(180);
    connect(_wallpaperOpacitySlider, &QSlider::valueChanged, this, [this](int value) {
        if (_wallpaperOpacityValueLabel != nullptr) {
            _wallpaperOpacityValueLabel->setText(QStringLiteral("%1%").arg(value));
        }
        if (_loading) {
            return;
        }
        emit wallpaperOpacityChanged(value);
    });
    opacityLayout->addWidget(_wallpaperOpacitySlider);
    _wallpaperOpacityValueLabel = makeValueLabel(opacityHost, 12);
    // 上面 setValue 在 connect 之前执行，标签还没建出来；这里补一次，
    // 免得在 loadThreePanelSettings 跑到之前显示成空白。
    _wallpaperOpacityValueLabel->setText(
        QStringLiteral("%1%").arg(_wallpaperOpacitySlider->value()));
    _wallpaperOpacityValueLabel->setFixedWidth(44);
    _wallpaperOpacityValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    opacityLayout->addWidget(_wallpaperOpacityValueLabel);
    layout->addWidget(makeRow(tr("图片透明度"),
                              tr("降低不透明度让背景更安静，只影响壁纸本身"),
                              opacityHost, block));

    return block;
}

void SettingsPage::setWallpaperOpacity(int percent)
{
    if (_wallpaperOpacitySlider == nullptr) {
        return;
    }
    const int clamped = std::clamp(percent, kWallpaperOpacityMin, kWallpaperOpacityMax);
    // 回灌也会触发 valueChanged：用 _loading 挡住，避免又被当成用户操作发一次信号。
    const bool previousLoading = _loading;
    _loading = true;
    _wallpaperOpacitySlider->setValue(clamped);
    _loading = previousLoading;
    if (_wallpaperOpacityValueLabel != nullptr) {
        _wallpaperOpacityValueLabel->setText(QStringLiteral("%1%").arg(clamped));
    }
}

void SettingsPage::setWallpaperState(bool enabled, const QString& source,
                                     const QString& statusText, bool isError, bool busy)
{
    // 回灌全部带 _loading 守卫：setIsToggled / setChecked 会 emit toggled，
    // 不挡就会把「引擎回灌」当成「用户又点了一下」，再发一轮请求。
    // 用局部变量恢复原值，避免把外层（例如 applyThreePanelDraft）的守卫提前解掉。
    const bool previousLoading = _loading;
    _loading = true;
    if (_wallpaperSwitch->getIsToggled() != enabled) {
        _wallpaperSwitch->setIsToggled(enabled);
    }
    if (!source.isEmpty()) {
        _wallpaperSource = source;
    }
    ElaRadioButton* target =
        _wallpaperSource == QLatin1String("uapis") ? _wallpaperUapisButton : _wallpaperLoliApiButton;
    if (!target->isChecked()) {
        target->setChecked(true);
    }
    _loading = previousLoading;

    // 下载中连点会开出第二个线程，禁用整组控件比只禁按钮更省心。
    _wallpaperRefreshButton->setEnabled(enabled && !busy);
    if (_wallpaperOpacitySlider != nullptr) {
        _wallpaperOpacitySlider->setEnabled(enabled);
    }
    if (_wallpaperOpacityValueLabel != nullptr) {
        _wallpaperOpacityValueLabel->setEnabled(enabled);
    }
    _wallpaperStatusLabel->setText(statusText);
    // 空状态不与错误共用同一条通道：没消息就把角色退回次要色，不留红字。
    setLabelRole(_wallpaperStatusLabel, isError ? "danger" : "secondary");
}

QWidget* SettingsPage::buildResidentPanel()
{
    auto* card = makeCard(this);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* head = new QWidget(card);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->addWidget(makeLabel(tr("常驻"), 14, head, true), 1);
    headLayout->addWidget(makeCaptionLabel(tr("保存后生效"), head), 0, Qt::AlignRight);
    layout->addWidget(head);

    layout->addWidget(makeCaptionLabel(tr("窗口行为"), card));

    _trayIconSwitch = new ElaToggleSwitch(card);
    connect(_trayIconSwitch, &ElaToggleSwitch::toggled, this, [this](bool checked) {
        if (_loading) {
            return;
        }
        _draftTrayIconVisible = checked;
        refreshSaveBar();
    });
    layout->addWidget(makeRow(tr("显示托盘图标"),
                              tr("关闭后托盘不可见，只能从任务栏唤回主窗口"),
                              _trayIconSwitch, card));

    layout->addWidget(makeRow(tr("关闭行为"),
                              tr("点标题栏 ✕ 时的动作；选「退出」不再弹确认框"),
                              nullptr, card));
    auto* closeHost = new QWidget(card);
    auto* closeLayout = new QHBoxLayout(closeHost);
    closeLayout->setContentsMargins(0, 0, 0, 0);
    closeLayout->setSpacing(6);
    _closeTrayButton = makeSegmentButton(tr("到托盘"), closeHost);
    _closeExitButton = makeSegmentButton(tr("退出"), closeHost);
    closeLayout->addWidget(_closeTrayButton);
    closeLayout->addWidget(_closeExitButton);
    layout->addWidget(closeHost, 0, Qt::AlignRight);
    bindSegment(_closeTrayButton, {_closeTrayButton, _closeExitButton}, [this]() {
        _draftCloseToTray = true;
        refreshSaveBar();
    });
    bindSegment(_closeExitButton, {_closeTrayButton, _closeExitButton}, [this]() {
        _draftCloseToTray = false;
        refreshSaveBar();
    });

    layout->addWidget(makeRow(tr("最小化行为"),
                              tr("任务栏最小化 / 收进托盘"),
                              nullptr, card));
    auto* minHost = new QWidget(card);
    auto* minLayout = new QHBoxLayout(minHost);
    minLayout->setContentsMargins(0, 0, 0, 0);
    minLayout->setSpacing(6);
    _minTaskbarButton = makeSegmentButton(tr("任务栏"), minHost);
    _minTrayButton = makeSegmentButton(tr("到托盘"), minHost);
    minLayout->addWidget(_minTaskbarButton);
    minLayout->addWidget(_minTrayButton);
    layout->addWidget(minHost, 0, Qt::AlignRight);
    bindSegment(_minTaskbarButton, {_minTaskbarButton, _minTrayButton}, [this]() {
        _draftMinimizeToTray = false;
        refreshSaveBar();
    });
    bindSegment(_minTrayButton, {_minTaskbarButton, _minTrayButton}, [this]() {
        _draftMinimizeToTray = true;
        refreshSaveBar();
    });

    layout->addWidget(makeCaptionLabel(tr("启动"), card));

    // 开机自启（自「启动」卡迁入）：与原卡同款「只发信号」模型。
    _autostartSwitch = new ElaToggleSwitch(card);
    connect(_autostartSwitch, &ElaToggleSwitch::toggled, this, [this](bool checked) {
        if (_loading) {
            return;
        }
        emit autostartRequested(checked);
    });
    layout->addWidget(makeRow(tr("开机自启"),
                              tr("登录后自动启动"),
                              _autostartSwitch, card));

    _autostartHint = makeCaptionLabel(QString(), card);
    _autostartHint->setWordWrap(true);
    layout->addWidget(_autostartHint);

    _startMinimizedSwitch = new ElaToggleSwitch(card);
    connect(_startMinimizedSwitch, &ElaToggleSwitch::toggled, this, [this](bool checked) {
        if (_loading) {
            return;
        }
        _draftStartMinimized = checked;
        refreshSaveBar();
    });
    layout->addWidget(makeRow(tr("启动时最小化"),
                              tr("启动后不弹主窗口，只在托盘继续记录"),
                              _startMinimizedSwitch, card));

    layout->addStretch();
    return card;
}

void SettingsPage::loadThreePanelSettings()
{
    if (_database == nullptr || !_database->isOpen()) {
        return;
    }
    data::SettingsRepository repository(*_database);
    const auto readBool = [&repository](const char* key, bool defaultValue) {
        const auto result = repository.value(QString::fromLatin1(key));
        if (!result.ok() || !result.value.has_value()) {
            return defaultValue;
        }
        // 两种写法都要认：本页写 "1"/"0"，schema v4 迁移预置的是 "true"/"false"。
        // 只认 "1" 会让预置的 title_recording_enabled='true' 被读成 false，
        // 引擎侧（tracking_settings.cpp）却是认 "true" 的，两边对不上。
        const QString value = result.value->trimmed().toLower();
        return value == QLatin1String("1") || value == QLatin1String("true");
    };
    const auto readSecs = [&repository](const char* key, int defaultSecs) {
        const auto result = repository.value(QString::fromLatin1(key));
        if (!result.ok() || !result.value.has_value()) {
            return defaultSecs;
        }
        bool ok = false;
        const int secs = result.value->trimmed().toInt(&ok);
        return ok && secs > 0 ? secs : defaultSecs;
    };
    const auto readText = [&repository](const char* key, const QString& defaultValue) {
        const auto result = repository.value(QString::fromLatin1(key));
        if (!result.ok() || !result.value.has_value()
            || result.value->trimmed().isEmpty()) {
            return defaultValue;
        }
        return result.value->trimmed();
    };

    _savedTrackingPaused = readBool("tracking_paused", false);
    _savedTitleRecording = readBool("title_recording_enabled", true);
    _savedMergeGapSecs = readSecs("timeline_merge_gap_secs", 180);
    _savedIdleTimeoutSecs = readSecs("idle_timeout_secs", 900);
    _savedThemeMode = readText("theme_mode", QStringLiteral("system"));
    _savedTrayIconVisible = readBool("show_tray_icon", true);
    _savedCloseToTray = readText("close_behavior", QStringLiteral("tray")) == QStringLiteral("tray");
    _savedMinimizeToTray =
        readText("minimize_behavior", QStringLiteral("taskbar")) == QStringLiteral("tray");
    _savedStartMinimized = readBool("start_minimized", true);

    // 壁纸是即时生效项：没有草稿，读到的就是当前值，直接摆到控件上。
    const bool wallpaperEnabled = readBool("wallpaper_enabled", false);
    const QString wallpaperSource =
        readText("wallpaper_source", QStringLiteral("loliapi")) == QStringLiteral("uapis")
            ? QStringLiteral("uapis")
            : QStringLiteral("loliapi");
    setWallpaperState(wallpaperEnabled, wallpaperSource, QString(), false);
    // 透明度也是即时项：库里没写或写坏了都回到 100%，不把用户卡在怪状态上。
    bool opacityOk = false;
    const int opacity = readText("wallpaper_opacity", QString()).toInt(&opacityOk);
    setWallpaperOpacity(opacityOk ? opacity : kWallpaperOpacityDefault);

    discardThreePanelDraft();
}

void SettingsPage::applyThreePanelDraft()
{
    _loading = true;
    if (_trackingPausedSwitch != nullptr) {
        _trackingPausedSwitch->setIsToggled(_draftTrackingPaused);
    }
    if (_titleRecordingSwitch != nullptr) {
        _titleRecordingSwitch->setIsToggled(_draftTitleRecording);
    }
    if (_mergeGapValue != nullptr) {
        // 与步进器同一换算：整分钟（值本身按 60s 步长存，直接整除）。
        _mergeGapValue->setText(
            tr("%1 分钟").arg(std::max(1, _draftMergeGapSecs / 60)));
    }
    if (_idleValue != nullptr) {
        _idleValue->setText(
            tr("%1 分钟").arg(std::max(1, _draftIdleTimeoutSecs / 60)));
    }
    if (_trayIconSwitch != nullptr) {
        _trayIconSwitch->setIsToggled(_draftTrayIconVisible);
    }
    if (_startMinimizedSwitch != nullptr) {
        _startMinimizedSwitch->setIsToggled(_draftStartMinimized);
    }
    refreshThemeModeControl();
    refreshBehaviorSegments();
    _loading = false;
}

void SettingsPage::saveThreePanelDraft()
{
    if (_database == nullptr || !_database->isOpen()) {
        setStatus(tr("数据库未就绪，无法保存追踪设置。"), true);
        return;
    }

    data::SettingsRepository repository(*_database);
    const data::Status pauseStatus = repository.setValue(
        QStringLiteral("tracking_paused"), _draftTrackingPaused ? QStringLiteral("1")
                                                                : QStringLiteral("0"));
    if (!pauseStatus.ok()) {
        setStatus(tr("保存失败：%1").arg(pauseStatus.error), true);
        return;
    }
    // 余下的键逐个写；首键成功后的失败同样报出来，但不再回滚已写的
    // （settings 是独立键值，部分生效可接受，下轮保存补齐）。
    const auto trySet = [this, &repository](const QString& key, const QString& value) {
        const data::Status status = repository.setValue(key, value);
        if (!status.ok()) {
            setStatus(tr("保存失败 %1：%2").arg(key, status.error), true);
        }
        return status.ok();
    };
    trySet(QStringLiteral("title_recording_enabled"),
           _draftTitleRecording ? QStringLiteral("1") : QStringLiteral("0"));
    trySet(QStringLiteral("timeline_merge_gap_secs"), QString::number(_draftMergeGapSecs));
    trySet(QStringLiteral("idle_timeout_secs"), QString::number(_draftIdleTimeoutSecs));
    trySet(QStringLiteral("theme_mode"), _draftThemeMode);
    trySet(QStringLiteral("show_tray_icon"),
           _draftTrayIconVisible ? QStringLiteral("1") : QStringLiteral("0"));
    trySet(QStringLiteral("close_behavior"),
           _draftCloseToTray ? QStringLiteral("tray") : QStringLiteral("exit"));
    trySet(QStringLiteral("minimize_behavior"),
           _draftMinimizeToTray ? QStringLiteral("tray") : QStringLiteral("taskbar"));
    trySet(QStringLiteral("start_minimized"),
           _draftStartMinimized ? QStringLiteral("1") : QStringLiteral("0"));

    _savedTrackingPaused = _draftTrackingPaused;
    _savedTitleRecording = _draftTitleRecording;
    _savedMergeGapSecs = _draftMergeGapSecs;
    _savedIdleTimeoutSecs = _draftIdleTimeoutSecs;
    _savedThemeMode = _draftThemeMode;
    _savedTrayIconVisible = _draftTrayIconVisible;
    _savedCloseToTray = _draftCloseToTray;
    _savedMinimizeToTray = _draftMinimizeToTray;
    _savedStartMinimized = _draftStartMinimized;

    // 暂停/恢复要立即作用到追踪服务（TrackingService::setPaused 会封当前段
    // 并写同一键；这里库已写好，信号只让 MainWindow 同步服务状态）。
    emit trackingPauseChanged(_draftTrackingPaused);
    // 标题录制同理：只写库的话引擎要等缓存 TTL 过期才认，托盘菜单也会滞后。
    emit titleRecordingChanged(_draftTitleRecording);
    // 外观与常驻面板的即时副作用同样只能在保存后发。
    emit themeModePicked(_draftThemeMode);
    emit trayIconVisibleChanged(_draftTrayIconVisible);
    emit closeBehaviorChanged(_draftCloseToTray);
    emit minimizeBehaviorChanged(_draftMinimizeToTray);

    refreshSaveBar();
    setStatus(tr("设置已写入，引擎 5 秒内生效。"), false);
}

void SettingsPage::discardThreePanelDraft()
{
    _draftTrackingPaused = _savedTrackingPaused;
    _draftTitleRecording = _savedTitleRecording;
    _draftMergeGapSecs = _savedMergeGapSecs;
    _draftIdleTimeoutSecs = _savedIdleTimeoutSecs;
    _draftThemeMode = _savedThemeMode;
    _draftTrayIconVisible = _savedTrayIconVisible;
    _draftCloseToTray = _savedCloseToTray;
    _draftMinimizeToTray = _savedMinimizeToTray;
    _draftStartMinimized = _savedStartMinimized;
    applyThreePanelDraft();
    refreshSaveBar();
}

void SettingsPage::refreshSaveBar()
{
    const bool dirty = _draftTrackingPaused != _savedTrackingPaused
        || _draftTitleRecording != _savedTitleRecording
        || _draftMergeGapSecs != _savedMergeGapSecs
        || _draftIdleTimeoutSecs != _savedIdleTimeoutSecs
        || _draftThemeMode != _savedThemeMode
        || _draftTrayIconVisible != _savedTrayIconVisible
        || _draftCloseToTray != _savedCloseToTray
        || _draftMinimizeToTray != _savedMinimizeToTray
        || _draftStartMinimized != _savedStartMinimized;
    if (_dirtyLabel != nullptr) {
        if (dirty) {
            _dirtyLabel->setText(tr("有未保存的改动"));
            setLabelRole(_dirtyLabel, "danger");
        } else {
            _dirtyLabel->setText(tr("已保存"));
            setLabelRole(_dirtyLabel, "secondary");
        }
    }
    if (_saveButton != nullptr) {
        _saveButton->setEnabled(dirty);
    }
    if (_cancelButton != nullptr) {
        _cancelButton->setEnabled(dirty);
    }
}

void SettingsPage::refreshThemeModeControl()
{
    // ElaToggleButton 自绘选中态，回灌时只要摆 IsToggled（_loading 挡信号）。
    if (_themeLightButton == nullptr) {
        return;
    }
    setSegmentChecked(_draftThemeMode == QStringLiteral("light") ? _themeLightButton
        : _draftThemeMode == QStringLiteral("dark") ? _themeDarkButton
                                                    : _themeSystemButton,
                      {_themeLightButton, _themeDarkButton, _themeSystemButton});
}

void SettingsPage::refreshBehaviorSegments()
{
    if (_closeTrayButton != nullptr) {
        setSegmentChecked(_draftCloseToTray ? _closeTrayButton : _closeExitButton,
                          {_closeTrayButton, _closeExitButton});
    }
    if (_minTaskbarButton != nullptr) {
        setSegmentChecked(_draftMinimizeToTray ? _minTrayButton : _minTaskbarButton,
                          {_minTaskbarButton, _minTrayButton});
    }
}

QWidget* SettingsPage::buildExportCard()
{
    auto* card = makeCard(this);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* head = new QWidget(card);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->addWidget(makeLabel(tr("导出备份"), 14, head, true), 1);
    headLayout->addWidget(makeCaptionLabel(tr(".db 单文件"), head), 0, Qt::AlignRight);
    layout->addWidget(head);

    auto* note = makeCaptionLabel(
        tr("把整个数据库复制到指定位置，含应用、活动段、分类覆盖项与设置。"), card);
    note->setWordWrap(true);
    layout->addWidget(note);

    _exportPathLabel = new QLabel(card);
    _exportPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _exportPathLabel->setWordWrap(true);
    QFont mono = _exportPathLabel->font();
    mono.setPixelSize(11);
    _exportPathLabel->setFont(mono);
    setLabelRole(_exportPathLabel, "secondary");
    layout->addWidget(_exportPathLabel);

    auto* buttons = new QWidget(card);
    auto* buttonsLayout = new QHBoxLayout(buttons);
    buttonsLayout->setContentsMargins(0, 0, 0, 0);
    buttonsLayout->setSpacing(8);

    _exportButton = makeButton(tr("导出到…"), buttons);
    connect(_exportButton, &QPushButton::clicked, this, &SettingsPage::onExportBackup);
    buttonsLayout->addWidget(_exportButton);

    auto* defaultButton = makeButton(tr("用默认位置"), buttons);
    defaultButton->setFixedWidth(100);
    connect(defaultButton, &QPushButton::clicked, this, &SettingsPage::onUseDefaultBackupPath);
    buttonsLayout->addWidget(defaultButton);
    buttonsLayout->addStretch();
    layout->addWidget(buttons);

    return card;
}

QWidget* SettingsPage::buildDataImportCard()
{
    auto* card = makeCard(this);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* head = new QWidget(card);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->addWidget(makeLabel(tr("导出数据 / 导入"), 14, head, true), 1);
    headLayout->addWidget(makeCaptionLabel(tr("CSV / MD / DB"), head), 0, Qt::AlignRight);
    layout->addWidget(head);

    // —— 导出数据（结构化三格式）——
    auto* formatRow = new QWidget(card);
    auto* formatLayout = new QHBoxLayout(formatRow);
    formatLayout->setContentsMargins(0, 0, 0, 0);
    formatLayout->setSpacing(8);
    _dataExportFormatBox = new ElaComboBox(formatRow);
    _dataExportFormatBox->setBorderRadius(8);
    _dataExportFormatBox->setMinimumWidth(0);
    _dataExportFormatBox->addItem(tr("CSV 表格"), QStringLiteral("csv"));
    _dataExportFormatBox->addItem(tr("Markdown 报表"), QStringLiteral("markdown"));
    _dataExportFormatBox->addItem(tr("SQLite 单库"), QStringLiteral("sqlite"));
    formatLayout->addWidget(_dataExportFormatBox, 1);

    auto* exportDataButton = makeButton(tr("导出数据…"), formatRow);
    connect(exportDataButton, &QPushButton::clicked, this, &SettingsPage::onExportData);
    formatLayout->addWidget(exportDataButton);
    layout->addWidget(formatRow);

    auto* exportNote = makeCaptionLabel(
        tr("按当前库的全部活动段导出结构化文件；与「导出备份」（整库快照）是两条路。"), card);
    exportNote->setWordWrap(true);
    layout->addWidget(exportNote);

    // —— 导入（外部数据单向迁移）——
    auto* importPickRow = new QWidget(card);
    auto* importPickLayout = new QHBoxLayout(importPickRow);
    importPickLayout->setContentsMargins(0, 0, 0, 0);
    importPickLayout->setSpacing(8);
    auto* pickImportButton = makeButton(tr("选择导入文件…"), importPickRow);
    pickImportButton->setFixedWidth(150);
    connect(pickImportButton, &QPushButton::clicked, this, &SettingsPage::onPickImportFile);
    importPickLayout->addWidget(pickImportButton);
    importPickLayout->addStretch();
    layout->addWidget(importPickRow);

    _importPreviewLabel = makeCaptionLabel(QString(), card);
    _importPreviewLabel->setWordWrap(true);
    layout->addWidget(_importPreviewLabel);

    // 说清楚能吃什么：用户按「导出数据」得到的那份 CSV 也在支持范围内，
    // 否则没人知道这两个按钮其实能对接。
    auto* importNote = makeCaptionLabel(
        tr("支持三种来源：本页「导出数据」生成的 CSV（报表列）、canonical CSV"
           "（record_type 为 exact_session / hour_bucket），以及同格式的 .db 数据库。"),
        card);
    importNote->setWordWrap(true);
    layout->addWidget(importNote);

    auto* importConfirmRow = new QWidget(card);
    auto* importConfirmLayout = new QHBoxLayout(importConfirmRow);
    importConfirmLayout->setContentsMargins(0, 0, 0, 0);
    importConfirmLayout->setSpacing(8);
    auto* confirmImportButton = makeButton(tr("确认导入"), importConfirmRow, 104);
    connect(confirmImportButton, &QPushButton::clicked, this, &SettingsPage::onConfirmImport);
    importConfirmLayout->addWidget(confirmImportButton);
    importConfirmLayout->addStretch();
    layout->addWidget(importConfirmRow);

    // 批次列表：可按批次整体删除回滚。
    auto* batchHead = makeLabel(tr("导入批次"), 11, card, true);
    layout->addWidget(batchHead);
    auto* batchHost = new QWidget(card);
    _importBatchLayout = new QVBoxLayout(batchHost);
    _importBatchLayout->setContentsMargins(0, 0, 0, 0);
    _importBatchLayout->setSpacing(6);
    layout->addWidget(batchHost);

    _importEmptyLabel = makeCaptionLabel(tr("还没有导入过数据。"), card);
    _importEmptyLabel->setWordWrap(true);
    layout->addWidget(_importEmptyLabel);

    return card;
}

QWidget* SettingsPage::buildRestoreCard()
{
    auto* card = makeCard(this);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* head = new QWidget(card);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->addWidget(makeLabel(tr("从备份恢复"), 14, head, true), 1);
    auto* warn = makeCaptionLabel(tr("覆盖现有数据"), head);
    setLabelRole(warn, "danger");
    headLayout->addWidget(warn, 0, Qt::AlignRight);
    layout->addWidget(head);

    // 拖放区：接受文件拖入，也可以点按钮选。dragEnterEvent 在 eventFilter 里处理。
    _dropArea = new QWidget(card);
    _dropArea->setObjectName(QStringLiteral("settingsDropArea"));
    _dropArea->setMinimumHeight(56);
    _dropArea->setAcceptDrops(true);
    _dropArea->installEventFilter(this);
    // 虚线框：不画的话这块空白跟普通留白没区别，用户不会想到可以往这里拖。
    // 选择器只认自己：写成 QWidget 会连里面的标签一起套上边框。
    refreshDropAreaStyle(currentThemeDark());
    auto* dropLayout = new QVBoxLayout(_dropArea);
    dropLayout->setContentsMargins(12, 10, 12, 10);
    dropLayout->setSpacing(2);
    _dropHintLabel = makeCaptionLabel(tr("把备份文件拖到这里"), _dropArea);
    _dropHintLabel->setWordWrap(true);
    dropLayout->addWidget(_dropHintLabel);
    layout->addWidget(_dropArea);

    auto* pickButton = makeButton(tr("选择文件…"), card);
    connect(pickButton, &QPushButton::clicked, this, &SettingsPage::onPickBackupFile);
    auto* pickRow = new QWidget(card);
    auto* pickLayout = new QHBoxLayout(pickRow);
    pickLayout->setContentsMargins(0, 0, 0, 0);
    pickLayout->addWidget(pickButton);
    pickLayout->addStretch();
    layout->addWidget(pickRow);

    // 四条校验行：与 BackupInspection 的检查项一一对应。
    auto* checks = new QWidget(card);
    auto* checksLayout = new QVBoxLayout(checks);
    checksLayout->setContentsMargins(0, 0, 0, 0);
    checksLayout->setSpacing(5);
    for (int index = 0; index < 4; ++index) {
        CheckRow row;
        checksLayout->addWidget(makeCheckRow(&row.mark, &row.text, checks));
        _restoreChecks.append(row);
    }
    layout->addWidget(checks);

    _restoreMessageLabel = makeCaptionLabel(QString(), card);
    _restoreMessageLabel->setWordWrap(true);
    layout->addWidget(_restoreMessageLabel);

    auto* note = makeCaptionLabel(
        tr("恢复会整体替换当前的应用、活动段与设置；写库在事务里，失败自动回滚。"), card);
    note->setWordWrap(true);
    layout->addWidget(note);

    _confirmRestoreButton = makeButton(tr("确认恢复"), card, 104);
    _confirmRestoreButton->setEnabled(false);
    connect(_confirmRestoreButton, &QPushButton::clicked, this, &SettingsPage::onConfirmRestore);
    auto* confirmRow = new QWidget(card);
    auto* confirmLayout = new QHBoxLayout(confirmRow);
    confirmLayout->setContentsMargins(0, 0, 0, 0);
    confirmLayout->setSpacing(8);
    confirmLayout->addWidget(_confirmRestoreButton);

    // 重置配色：分类配色覆盖 + 应用自定义色一次性清空，应用回「跟随分类色」。
    // 与「恢复备份」同属改数据的危险动作，收在同一张卡里；二次确认后才动手。
    auto* resetColorsButton = makeButton(tr("重置分类和应用配色"), card, 148);
    connect(resetColorsButton, &QPushButton::clicked, this, [this]() {
        if (_database == nullptr || !_database->isOpen()) {
            setStatus(tr("数据库未就绪，无法重置配色。"), true);
            return;
        }
        // ElaContentDialog 这版没有 setTitle：标题写在内容里（与主窗口
        // 关闭弹框同一做法）。中间按钮藏掉（不设文字时会露出库的
        // "minimum" 占位符）；左右按钮加宽保证中文完整显示。
        ElaContentDialog confirmDialog(this);
        confirmDialog.setLeftButtonText(tr("取 消"));
        confirmDialog.setRightButtonText(tr("重 置"));
        confirmDialog.setMiddleButtonVisible(false);
        auto* content = new QWidget();
        auto* contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(15, 25, 15, 10);
        contentLayout->setSpacing(6);
        auto* title = new ElaText(tr("重置配色"), 15, content);
        title->setWordWrap(false);
        title->setTextStyle(ElaTextType::BodyStrong);
        // setTextStyle 会顺手改字号，这里必须复位回调用方要的像素大小。
        title->setTextPixelSize(15);
        auto* detail = new ElaText(
            tr("将清除所有分类配色与应用自定义颜色：\n分类恢复默认色，应用恢复跟随分类色。\n分类归属与改名不受影响。此操作不可撤销。"),
            13, content);
        detail->setWordWrap(true);
        contentLayout->addWidget(title);
        contentLayout->addWidget(detail);
        contentLayout->addStretch();
        confirmDialog.setCentralWidget(content);
        if (confirmDialog.exec() != QDialog::Accepted) {
            return;
        }

        data::ClassificationRepository repository(*_database);
        const data::Status reset = repository.resetAllColors();
        if (!reset.ok()) {
            setStatus(tr("重置失败：%1").arg(reset.error), true);
            return;
        }
        setStatus(tr("已重置：分类恢复默认色，应用恢复跟随分类色。"), false);
        emit databaseReplaced();
    });
    confirmLayout->addWidget(resetColorsButton);
    confirmLayout->addStretch();
    layout->addWidget(confirmRow);

    return card;
}

QWidget* SettingsPage::buildCleanupCard()
{
    auto* card = makeCard(this);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* head = new QWidget(card);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->addWidget(makeLabel(tr("清理历史"), 14, head, true), 1);
    headLayout->addWidget(makeCaptionLabel(tr("不可撤销"), head), 0, Qt::AlignRight);
    layout->addWidget(head);

    _cleanupPivot = new ElaPivot(card);
    _cleanupPivot->setTextPixelSize(12);
    _cleanupPivot->setPivotSpacing(12);
    _cleanupPivot->setMarkWidth(18);
    _cleanupPivot->appendPivot(tr("30 天"));
    _cleanupPivot->appendPivot(tr("90 天"));
    _cleanupPivot->appendPivot(tr("全部"));
    _cleanupPivot->setCurrentIndex(1);
    connect(_cleanupPivot, &ElaPivot::pivotClicked, this, [this](int) { refreshCleanupPreview(); });
    layout->addWidget(makeRow(tr("保留范围"), tr("按活动段起点判断"), _cleanupPivot, card));

    _cleanupPreviewLabel = makeCaptionLabel(QString(), card);
    _cleanupPreviewLabel->setWordWrap(true);
    layout->addWidget(_cleanupPreviewLabel);

    _cleanupButton = makeButton(tr("删除"), card);
    connect(_cleanupButton, &QPushButton::clicked, this, &SettingsPage::onCleanup);
    auto* buttonRow = new QWidget(card);
    auto* buttonLayout = new QHBoxLayout(buttonRow);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(8);
    auto* exportFirst = makeButton(tr("先导出"), buttonRow);
    connect(exportFirst, &QPushButton::clicked, this, &SettingsPage::onExportBackup);
    buttonLayout->addWidget(exportFirst);
    buttonLayout->addWidget(_cleanupButton);
    buttonLayout->addStretch();
    layout->addWidget(buttonRow);

    return card;
}

void SettingsPage::setActive(bool active)
{
    _active = active;
    if (!active) {
        // 离开页面时把「最近备份」这类列表控件释放掉。
        // 路径、尺寸这些一行字的标签留着，回来时不必闪一下空白。
        if (_recentLayout != nullptr) {
            clearLayout(_recentLayout);
        }
        _statusLabel->setVisible(false);
        return;
    }
    refreshAll();
}

void SettingsPage::setAutostartState(bool enabled)
{
    if (_autostartSwitch == nullptr) {
        return;
    }
    _loading = true;
    _autostartSwitch->setIsToggled(enabled);
    _loading = false;
    if (_autostartHint != nullptr) {
        _autostartHint->setText(enabled ? tr("已注册到当前用户的启动项。")
                                        : tr("未注册；打开开关会写入当前用户的启动项。"));
    }
}

void SettingsPage::setTrackingPaused(bool paused)
{
    if (_draftTrackingPaused == paused && _savedTrackingPaused == paused) {
        return;
    }
    // 托盘已经改过库：草稿和基线一起对齐，否则这里会显示成「有未保存的改动」。
    _draftTrackingPaused = paused;
    _savedTrackingPaused = paused;
    applyThreePanelDraft();
    refreshSaveBar();
}

void SettingsPage::setTitleRecordingEnabled(bool enabled)
{
    if (_draftTitleRecording == enabled && _savedTitleRecording == enabled) {
        return;
    }
    _draftTitleRecording = enabled;
    _savedTitleRecording = enabled;
    applyThreePanelDraft();
    refreshSaveBar();
}

void SettingsPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (_active) {
        refreshAll();
    }
}

void SettingsPage::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    if (_recentLayout != nullptr) {
        clearLayout(_recentLayout);
    }
}

bool SettingsPage::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == _dropArea) {
        if (event->type() == QEvent::DragEnter) {
            auto* dragEvent = static_cast<QDragEnterEvent*>(event);
            if (dragEvent->mimeData()->hasUrls()) {
                dragEvent->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto* dropEvent = static_cast<QDropEvent*>(event);
            const QList<QUrl> urls = dropEvent->mimeData()->urls();
            if (!urls.isEmpty()) {
                const QString path = urls.first().toLocalFile();
                if (!path.isEmpty()) {
                    selectBackup(path);
                    dropEvent->acceptProposedAction();
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SettingsPage::refreshAll()
{
    refreshStorage();
    refreshDiagnostics();
    refreshRecentBackups();
    refreshExportPath();
    refreshRestoreChecklist();
    refreshCleanupPreview();
    refreshImportBatches();
}

void SettingsPage::setDiagnosticsSource(std::function<DiagnosticsSnapshot()> source)
{
    _diagnosticsSource = std::move(source);
    refreshDiagnostics();
}

void SettingsPage::refreshDiagnostics()
{
    if (_workingSetLabel == nullptr) {
        return;
    }

    // 没有取数来源时（比如单独构造本页），除了缓存条数以外都显示「—」。
    const bool hasSource = static_cast<bool>(_diagnosticsSource);
    const DiagnosticsSnapshot snapshot = hasSource ? _diagnosticsSource() : DiagnosticsSnapshot();

    const auto bytesText = [hasSource](bool valid, qint64 bytes) -> QString {
        if (!hasSource) {
            return QStringLiteral("—");
        }
        return valid ? formatBytes(bytes) : tr("读取失败");
    };
    _workingSetLabel->setText(bytesText(snapshot.processValid, snapshot.workingSetBytes));
    _privateBytesLabel->setText(bytesText(snapshot.processValid, snapshot.privateBytes));
    _handleCountLabel->setText(hasSource && snapshot.processValid
                                   ? formatCount(snapshot.handleCount)
                                   : QStringLiteral("—"));

    // 图标缓存条数是本地就能读到的（两层 QCache 的 size 之和）。
    _iconCacheLabel->setText(formatCount(shared::appIconCacheEntryCount()));
    _chartCacheLabel->setText(hasSource ? formatBytes(snapshot.chartCacheBytes)
                                        : QStringLiteral("—"));
    _databaseFileLabel->setText(hasSource ? formatBytes(snapshot.databaseFileBytes)
                                          : QStringLiteral("—"));

    const auto stampText = [hasSource](bool valid, qint64 timestampMs) -> QString {
        if (!hasSource || !valid || timestampMs <= 0) {
            return tr("尚未记录");
        }
        return QDateTime::fromMSecsSinceEpoch(timestampMs).toString(QStringLiteral("HH:mm:ss"));
    };
    _healthHeartbeatLabel->setText(stampText(snapshot.healthValid, snapshot.lastHeartbeatMs));
    _healthSampleLabel->setText(stampText(snapshot.healthValid, snapshot.lastSuccessfulSampleMs));
    _healthSealLabel->setText(stampText(snapshot.healthValid, snapshot.lastWatchdogSealMs));
}

void SettingsPage::refreshStorage()
{
    if (_dataDirLabel == nullptr) {
        return;
    }

    const QString dataDirectory = SettingsConfig::dataDirectory();
    _dataDirLabel->setText(QDir::toNativeSeparators(dataDirectory.isEmpty() ? QStringLiteral("—")
                                                                          : dataDirectory));

    if (_dataDirHint != nullptr) {
        _dataDirHint->setText(SettingsConfig::hasCustomDataDirectory()
                                  ? tr("当前用的是自定义目录（记录在 config.json）。")
                                  : tr("当前用的是默认目录；配置文件里没有自定义项。"));
    }

    // 数据库大小含 -wal：只报主文件会让人以为数据丢了。
    qint64 databaseBytes = -1;
    if (_database != nullptr && _database->isOpen()) {
        const QString path = _database->filePath();
        if (!path.isEmpty() && path != QStringLiteral(":memory:")) {
            databaseBytes = QFileInfo(path).size() + QFileInfo(path + QStringLiteral("-wal")).size();
        }
    }
    if (_databaseSizeLabel != nullptr) {
        _databaseSizeLabel->setText(databaseBytes >= 0 ? formatBytes(databaseBytes)
                                                       : tr("未连接"));
    }

    if (_backupDirSizeLabel != nullptr) {
        const qint64 backupBytes = directorySize(data::BackupService::defaultBackupDirectory());
        _backupDirSizeLabel->setText(backupBytes >= 0 ? formatBytes(backupBytes) : tr("—"));
    }

    if (_sessionCountLabel != nullptr) {
        qint64 count = -1;
        if (_database != nullptr && _database->isOpen()) {
            data::BackupService service(*_database);
            // 存储卡要的是「库里一共有多少段」，含正在记录的那一条；
            // countSessionsBefore 只数已闭合段（清理口径），不能拿来当总数。
            const data::Result<qint64> result = service.countAllSessions();
            if (result.ok()) {
                count = result.value;
            }
        }
        _sessionCountLabel->setText(count >= 0 ? formatCount(count) : tr("—"));
    }
}

void SettingsPage::refreshRecentBackups()
{
    if (_recentLayout == nullptr) {
        return;
    }
    clearLayout(_recentLayout);

    const QVector<QFileInfo> files =
        listRecentBackups(data::BackupService::defaultBackupDirectory(), kRecentBackupLimit);
    const bool empty = files.isEmpty();
    if (_recentEmptyLabel != nullptr) {
        _recentEmptyLabel->setVisible(empty);
    }
    if (empty) {
        _recentLayout->addStretch();
        return;
    }

    for (const QFileInfo& file : files) {
        auto* row = new ElaPushButton(file.fileName(), _recentLayout->parentWidget());
        row->setFixedHeight(28);
        row->setBorderRadius(8);
        row->setMinimumWidth(0);
        row->setCursor(Qt::PointingHandCursor);
        const QString path = file.absoluteFilePath();
        connect(row, &QPushButton::clicked, this, [this, path]() { selectBackup(path); });
        _recentLayout->addWidget(row);
    }
    _recentLayout->addStretch();
}

void SettingsPage::refreshExportPath()
{
    if (_exportPathLabel != nullptr) {
        _exportPathLabel->setText(QDir::toNativeSeparators(_exportPath));
    }
}

void SettingsPage::refreshRestoreChecklist()
{
    if (_restoreChecks.size() < 4) {
        return;
    }

    if (_selectedBackupPath.isEmpty()) {
        // 未选择文件：四条都是「未校验」的中性灰，不是错误。
        setCheckRow(_restoreChecks.at(0).mark, _restoreChecks.at(0).text, -1, tr("还没有选择备份文件"));
        setCheckRow(_restoreChecks.at(1).mark, _restoreChecks.at(1).text, -1, tr("SQLite 完整性未校验"));
        setCheckRow(_restoreChecks.at(2).mark, _restoreChecks.at(2).text, -1, tr("schema 版本未校验"));
        setCheckRow(_restoreChecks.at(3).mark, _restoreChecks.at(3).text, -1, tr("条目数未知"));
        if (_restoreMessageLabel != nullptr) {
            _restoreMessageLabel->setText(QString());
        }
        if (_dropHintLabel != nullptr) {
            _dropHintLabel->setText(tr("把备份文件拖到这里"));
        }
        if (_confirmRestoreButton != nullptr) {
            _confirmRestoreButton->setEnabled(false);
        }
        return;
    }

    if (_dropHintLabel != nullptr) {
        _dropHintLabel->setText(QDir::toNativeSeparators(_selectedBackupPath));
    }

    // 四条校验行与 BackupInspection 的检查顺序一一对应：
    // 文件可读 → 完整性 → schema 版本 → 条目数（1=通过 0=失败）。
    const bool readable = _inspection.sizeBytes > 0;
    setCheckRow(_restoreChecks.at(0).mark, _restoreChecks.at(0).text, readable ? 1 : 0,
                readable ? tr("文件可读，大小 %1").arg(formatBytes(_inspection.sizeBytes))
                         : tr("文件读不出来"));

    const bool restorable = _inspection.restorable;
    setCheckRow(_restoreChecks.at(1).mark, _restoreChecks.at(1).text, restorable ? 1 : 0,
                _inspection.message.isEmpty() ? tr("SQLite 完整性未通过") : _inspection.message);

    const bool versionOk = _inspection.schemaVersion > 0 && restorable;
    setCheckRow(_restoreChecks.at(2).mark, _restoreChecks.at(2).text, versionOk ? 1 : 0,
                versionOk ? tr("schema 版本 %1，与当前程序一致").arg(_inspection.schemaVersion)
                          : tr("schema 版本与当前程序不一致"));

    const bool countsOk = restorable && _inspection.sessionCount >= 0 && _inspection.appCount >= 0;
    setCheckRow(_restoreChecks.at(3).mark, _restoreChecks.at(3).text, countsOk ? 1 : 0,
                countsOk ? tr("包含 %1 个活动段 / %2 个应用")
                               .arg(formatCount(_inspection.sessionCount),
                                    formatCount(_inspection.appCount))
                         : tr("条目数未读到"));

    if (_restoreMessageLabel != nullptr) {
        _restoreMessageLabel->setText(_inspection.message);
        setLabelRole(_restoreMessageLabel, restorable ? "success" : "danger");
    }
    if (_confirmRestoreButton != nullptr) {
        _confirmRestoreButton->setEnabled(restorable);
    }
}

void SettingsPage::refreshCleanupPreview()
{
    if (_cleanupPreviewLabel == nullptr) {
        return;
    }

    qint64 count = -1;
    if (_database != nullptr && _database->isOpen()) {
        data::BackupService service(*_database);
        const data::Result<qint64> result = service.countSessionsBefore(cleanupCutoffMs());
        if (result.ok()) {
            count = result.value;
        }
    }

    if (count < 0) {
        _cleanupPreviewLabel->setText(tr("数据库未就绪，无法统计。"));
    } else if (count == 0) {
        // 说清楚「为什么是 0」：正在记录的那一段不参与清理，
        // 用户选了 30 天却看到 0 条时，得知道不是功能坏了。
        _cleanupPreviewLabel->setText(
            tr("「%1」范围内没有可删除的活动段。正在记录的那一段不会被清理。")
                .arg(cleanupRangeLabel()));
    } else {
        _cleanupPreviewLabel->setText(
            tr("将删除「%1」范围内的 %2 个活动段，删除后各页对应区间会一起变短。")
                .arg(cleanupRangeLabel(), formatCount(count)));
    }
    if (_cleanupButton != nullptr) {
        _cleanupButton->setEnabled(count > 0);
    }
}

QString SettingsPage::cleanupRangeLabel() const
{
    const int index = _cleanupPivot != nullptr ? _cleanupPivot->getCurrentIndex() : 1;
    if (index == 0) {
        return tr("30 天前");
    }
    if (index == 1) {
        return tr("90 天前");
    }
    return tr("全部");
}

qint64 SettingsPage::cleanupCutoffMs() const
{
    const int index = _cleanupPivot != nullptr ? _cleanupPivot->getCurrentIndex() : 1;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (index == 0) {
        return nowMs - 30 * kDayMs;
    }
    if (index == 1) {
        return nowMs - 90 * kDayMs;
    }
    // 「全部」：截止时间放到明天，等价于把所有段都算进来。
    return nowMs + kDayMs;
}

void SettingsPage::setStatus(const QString& message, bool error)
{
    if (_statusLabel == nullptr) {
        return;
    }
    if (message.isEmpty()) {
        _statusLabel->setVisible(false);
        return;
    }
    _statusLabel->setText(message);
    setLabelRole(_statusLabel, error ? "danger" : "success");
    _statusLabel->setVisible(true);
}

void SettingsPage::selectBackup(const QString& path)
{
    _selectedBackupPath = path;
    _inspection = data::BackupInspection();

    if (_database == nullptr || !_database->isOpen()) {
        _statusLabel->setText(tr("数据库未就绪，无法校验备份。"));
        setLabelRole(_statusLabel, "danger");
        _statusLabel->setVisible(true);
        refreshRestoreChecklist();
        return;
    }

    data::BackupService service(*_database);
    const data::Result<data::BackupInspection> result = service.inspect(path);
    if (!result.ok()) {
        _inspection.message = result.error;
        setStatus(tr("校验备份失败：%1").arg(result.error), true);
    } else {
        _inspection = result.value;
        setStatus(QString(), false);
    }
    refreshRestoreChecklist();
}

void SettingsPage::onOpenDataDirectory()
{
    const QString directory = SettingsConfig::dataDirectory();
    if (directory.isEmpty()) {
        setStatus(tr("还无法确定数据目录。"), true);
        return;
    }
    if (!QDir().mkpath(directory)) {
        setStatus(tr("打不开数据目录：%1").arg(QDir::toNativeSeparators(directory)), true);
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
}

void SettingsPage::onChooseDataDirectory()
{
    const QString current = SettingsConfig::dataDirectory();
    const QString chosen = QFileDialog::getExistingDirectory(
        this, tr("选择数据目录"), current, QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (chosen.isEmpty()) {
        return;
    }
    if (QDir::cleanPath(chosen) == QDir::cleanPath(current)) {
        setStatus(QString(), false);
        return;
    }

    // 先校验目标路径，再动库。
    QString validationError =
        data::DataDirectoryMigrator::validateTarget(chosen, current);
    if (!validationError.isEmpty()) {
        setStatus(validationError, true);
        return;
    }

    // 改目录数据要跟着走。关库 → checkpoint 合并 WAL → 拷贝 → 校验行数
    // → 写 config.json；任何一步失败旧数据原地不动。
    const QString currentDatabase = current + QStringLiteral("/usetrack.db");
    const QString targetDatabase = QDir::cleanPath(chosen) + QStringLiteral("/usetrack.db");

    const bool wasOpen = _database != nullptr && _database->isOpen();
    if (wasOpen) {
        _database->closeConnectionPublic();
    }

    const data::Status migrated =
        data::DataDirectoryMigrator::migrateDatabaseFile(currentDatabase, targetDatabase);
    if (!migrated.ok()) {
        // 回滚：重开旧库，什么都没改。
        if (wasOpen) {
            _database->openAndMigrate(currentDatabase);
        }
        setStatus(migrated.error, true);
        return;
    }

    QString configError;
    if (!SettingsConfig::setDataDirectory(chosen, &configError)) {
        // config 写失败：库文件已拷走但配置没变——数据没丢，提示用户即可。
        if (wasOpen) {
            _database->openAndMigrate(currentDatabase);
        }
        setStatus(tr("库文件已拷到新目录，但写入配置失败：%1").arg(configError), true);
        return;
    }

    if (wasOpen) {
        const data::Status reopened = _database->openAndMigrate(targetDatabase);
        if (!reopened.ok()) {
            setStatus(tr("新库已就位但重开失败（重启后生效）：%1").arg(reopened.error), true);
            return;
        }
    }
    setStatus(tr("数据已搬到新目录，当前会话继续使用新库。"), false);
    refreshStorage();
    // 各页缓存的读数指向同一份数据（行数一致），不必整页重查。
    emit databaseReplaced();
}

void SettingsPage::onExportBackup()
{
    if (_database == nullptr || !_database->isOpen()) {
        setStatus(tr("数据库未就绪，无法导出。"), true);
        return;
    }

    const QString target = QFileDialog::getSaveFileName(
        this, tr("导出备份"), _exportPath, tr("SQLite 数据库 (*.db)"));
    if (target.isEmpty()) {
        return;
    }

    data::BackupService service(*_database);
    const data::Status status = service.exportTo(target);
    if (!status.ok()) {
        setStatus(tr("导出失败：%1").arg(status.error), true);
        return;
    }

    _exportPath = target;
    refreshExportPath();
    refreshStorage();
    refreshRecentBackups();
    setStatus(tr("已导出到 %1").arg(QDir::toNativeSeparators(target)), false);
}

void SettingsPage::onUseDefaultBackupPath()
{
    _exportPath = data::BackupService::defaultBackupDirectory() + QLatin1Char('/')
        + data::BackupService::defaultBackupFileName(QDateTime::currentMSecsSinceEpoch());
    refreshExportPath();
    setStatus(tr("已切回默认备份位置。"), false);
}

void SettingsPage::onPickBackupFile()
{
    const QString start = QFileInfo(_selectedBackupPath).absolutePath().isEmpty()
        ? data::BackupService::defaultBackupDirectory()
        : QFileInfo(_selectedBackupPath).absolutePath();
    const QString chosen =
        QFileDialog::getOpenFileName(this, tr("选择备份文件"), start, tr("SQLite 数据库 (*.db)"));
    if (chosen.isEmpty()) {
        return;
    }
    selectBackup(chosen);
}

void SettingsPage::onConfirmRestore()
{
    if (_selectedBackupPath.isEmpty() || !_inspection.restorable) {
        setStatus(tr("请先选择一个通过校验的备份文件。"), true);
        return;
    }
    if (_database == nullptr || !_database->isOpen()) {
        setStatus(tr("数据库未就绪，无法恢复。"), true);
        return;
    }

    data::BackupService service(*_database);
    const data::Status status = service.restoreFrom(_selectedBackupPath);
    if (!status.ok()) {
        setStatus(tr("恢复失败：%1").arg(status.error), true);
        refreshRestoreChecklist();
        return;
    }

    setStatus(tr("已从备份恢复，当前数据已被替换。"), false);
    // 整库换过了：别的页面缓存的应用名、分类覆盖项、工具状态都过期了。
    emit databaseReplaced();
    refreshAll();
}

void SettingsPage::onCleanup()
{
    if (_database == nullptr || !_database->isOpen()) {
        setStatus(tr("数据库未就绪，无法清理。"), true);
        return;
    }

    data::BackupService service(*_database);

    // 破坏性操作先确认。删除不可撤销，且这里删的不只是活动段。
    const data::Result<qint64> counted = service.countSessionsBefore(cleanupCutoffMs());
    const QString countText = counted.ok() ? formatCount(counted.value) : QStringLiteral("若干");
    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this, tr("确认清理历史"),
        tr("将永久删除「%1」范围内的 %2 个活动段，以及同一范围内的匿名活动记录与导入数据。\n"
           "正在记录的那一段不会被清理。\n"
           "此操作不可撤销，建议先导出一份备份。\n\n确定继续吗？")
            .arg(cleanupRangeLabel(), countText),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        setStatus(tr("已取消清理。"), false);
        return;
    }

    const data::Result<qint64> result = service.deleteSessionsBefore(cleanupCutoffMs());
    if (!result.ok()) {
        setStatus(tr("清理失败：%1").arg(result.error), true);
        return;
    }
    setStatus(tr("已删除 %1 个活动段。").arg(formatCount(result.value)), false);
    refreshStorage();
    refreshCleanupPreview();
    emit databaseReplaced();
}

// —— 数据导出（结构化三格式） ——

void SettingsPage::onExportData()
{
    if (_database == nullptr || !_database->isOpen()) {
        setStatus(tr("数据库未就绪，无法导出。"), true);
        return;
    }

    const QString format = _dataExportFormatBox != nullptr
        ? _dataExportFormatBox->currentData().toString()
        : QStringLiteral("csv");
    const QString extension =
        format == QLatin1String("csv") ? QStringLiteral("csv")
        : format == QLatin1String("markdown") ? QStringLiteral("md") : QStringLiteral("db");

    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString defaultDirectory =
        documents.isEmpty() ? QString() : documents + QStringLiteral("/UseTrack/exports");
    const QString suggested = defaultDirectory + QLatin1Char('/')
        + data::ExportService::defaultExportFileName(format, QDateTime::currentMSecsSinceEpoch());

    // 先选目标路径，再写文件。
    const QString target = QFileDialog::getSaveFileName(
        this, tr("导出数据"), suggested, tr("导出文件 (*.%1)").arg(extension));
    if (target.isEmpty()) {
        return;
    }

    data::ExportService service(*_database);
    const data::Result<data::ExportReport> result =
        service.exportTo(format, target, 0, 0, data::defaultExportFields());
    if (!result.ok()) {
        setStatus(tr("导出失败：%1").arg(result.error), true);
        return;
    }
    setStatus(tr("已导出 %1 条活动段到 %2")
                  .arg(formatCount(result.value.sessionCount),
                       QDir::toNativeSeparators(result.value.targetPath)),
              false);
}

// —— 导入（预览 → 提交 → 批次管理） ——

void SettingsPage::onPickImportFile()
{
    const QString chosen = QFileDialog::getOpenFileName(
        this, tr("选择导入文件"), QString(),
        tr("数据库或 CSV (*.db *.sqlite *.csv)"));
    if (chosen.isEmpty()) {
        return;
    }
    if (_database == nullptr || !_database->isOpen()) {
        setStatus(tr("数据库未就绪，无法预览导入。"), true);
        return;
    }

    data::ImportService service(*_database);
    const data::Result<data::ImportPreview> result = service.preview(chosen);
    if (!result.ok()) {
        _importFilePath.clear();
        _importFingerprint.clear();
        _importPreview = data::ImportPreview();
        if (_importPreviewLabel != nullptr) {
            _importPreviewLabel->setText(result.error);
            setLabelRole(_importPreviewLabel, "danger");
        }
        setStatus(result.error, true);
        return;
    }

    _importFilePath = chosen;
    _importFingerprint = result.value.fingerprint;
    _importPreview = result.value;

    if (_importPreviewLabel != nullptr) {
        QString text;
        if (_importPreview.validRecords == 0) {
            text = tr("没有可导入的记录。");
        } else {
            const QString kindText =
                _importPreview.sourceKind == QLatin1String("patina-csv")
                    ? tr("记录表（CSV）")
                    : tr("数据库（.db）");
            text = tr("%1：%2 条记录（精确段 %3，小时桶 %4），时间范围 %5 → %6")
                       .arg(kindText,
                            formatCount(_importPreview.validRecords),
                            formatCount(_importPreview.exactSessions),
                            formatCount(_importPreview.hourBuckets),
                            QDateTime::fromMSecsSinceEpoch(_importPreview.minStartTimeMs)
                                .toString(QStringLiteral("yyyy-MM-dd")),
                            QDateTime::fromMSecsSinceEpoch(_importPreview.maxEndTimeMs)
                                .toString(QStringLiteral("yyyy-MM-dd")));
        }
        for (const QString& warning : _importPreview.warnings) {
            text += QStringLiteral("\n· ") + warning;
        }
        _importPreviewLabel->setText(text);
        setLabelRole(_importPreviewLabel, _importPreview.validRecords > 0 ? "success" : "danger");
    }
    setStatus(tr("预览完成，确认无误后点「确认导入」。"), false);
}

void SettingsPage::onConfirmImport()
{
    if (_importFilePath.isEmpty() || _importFingerprint.isEmpty()) {
        setStatus(tr("请先选择并预览导入文件。"), true);
        return;
    }
    if (_database == nullptr || !_database->isOpen()) {
        setStatus(tr("数据库未就绪，无法导入。"), true);
        return;
    }

    data::ImportService service(*_database);
    const data::Result<data::ImportCommitReport> result =
        service.commit(_importFilePath, _importFingerprint);
    if (!result.ok()) {
        setStatus(tr("导入失败：%1").arg(result.error), true);
        return;
    }

    if (result.value.batchId.isEmpty()) {
        setStatus(tr("没有新数据可导入（全部重复）。"), false);
    } else {
        setStatus(tr("已导入 %1 条记录（重复跳过 %2），批次 %3。")
                      .arg(formatCount(result.value.importedRecords),
                           formatCount(result.value.duplicateRecords),
                           result.value.batchId.left(18) + QStringLiteral("…")),
                  false);
    }

    // 导入改了数据：清预览状态、刷批次列表、让其它页重查。
    _importFilePath.clear();
    _importFingerprint.clear();
    _importPreview = data::ImportPreview();
    if (_importPreviewLabel != nullptr) {
        _importPreviewLabel->setText(QString());
    }
    refreshImportBatches();
    emit databaseReplaced();
}

void SettingsPage::onImportBatchSelected(int row)
{
    if (row < 0 || row >= _importBatches.size()) {
        return;
    }
    if (_database == nullptr || !_database->isOpen()) {
        setStatus(tr("数据库未就绪，无法回滚。"), true);
        return;
    }

    const data::ImportBatchSummary batch = _importBatches.at(row);
    data::ImportService service(*_database);
    const data::Result<data::ImportDeleteReport> result = service.deleteBatch(batch.id);
    if (!result.ok()) {
        setStatus(tr("回滚失败：%1").arg(result.error), true);
        return;
    }
    setStatus(tr("已回滚批次：删除 %1 个导入段、%2 个小时桶。")
                  .arg(formatCount(result.value.deletedExactSessions),
                       formatCount(result.value.deletedHourBuckets)),
              false);
    refreshImportBatches();
    emit databaseReplaced();
}

void SettingsPage::refreshImportBatches()
{
    if (_importBatchLayout == nullptr) {
        return;
    }
    clearLayout(_importBatchLayout);

    _importBatches.clear();
    if (_database != nullptr && _database->isOpen()) {
        data::ImportService service(*_database);
        const data::Result<QVector<data::ImportBatchSummary>> result = service.listBatches();
        if (result.ok()) {
            _importBatches = result.value;
        }
    }

    const bool empty = _importBatches.isEmpty();
    if (_importEmptyLabel != nullptr) {
        _importEmptyLabel->setVisible(empty);
    }
    if (empty) {
        _importBatchLayout->addStretch();
        return;
    }

    int row = 0;
    for (const data::ImportBatchSummary& batch : _importBatches) {
        auto* rowWidget = new QWidget(_importBatchLayout->parentWidget());
        auto* rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);

        auto* textBox = new QWidget(rowWidget);
        auto* textLayout = new QVBoxLayout(textBox);
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(1);
        textLayout->addWidget(makeLabel(
            tr("%1 · %2 条（段 %3 / 桶 %4）")
                .arg(QDateTime::fromMSecsSinceEpoch(batch.importedAtMs)
                         .toString(QStringLiteral("MM-dd HH:mm")),
                     formatCount(batch.exactSessions + batch.hourBuckets),
                     formatCount(batch.exactSessions), formatCount(batch.hourBuckets)),
            11, textBox, true));
        textLayout->addWidget(makeLabel(batch.sourceName, 10, textBox));
        rowLayout->addWidget(textBox, 1);

        auto* rollbackButton = new ElaPushButton(tr("回滚"), rowWidget);
        rollbackButton->setFixedHeight(24);
        rollbackButton->setMinimumWidth(56);
        rollbackButton->setBorderRadius(6);
        rollbackButton->setCursor(Qt::PointingHandCursor);
        const int batchRow = row++;
        connect(rollbackButton, &QPushButton::clicked, this,
                [this, batchRow]() { onImportBatchSelected(batchRow); });
        rowLayout->addWidget(rollbackButton);

        _importBatchLayout->addWidget(rowWidget);
    }
    _importBatchLayout->addStretch();
}

} // namespace patina::ui
