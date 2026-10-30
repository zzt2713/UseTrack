#include "ui/classification/classification_page.h"

#include "ui/classification/app_classification_card.h"
#include "ui/shared/quick_classification_menu.h"

#include "ElaColorDialog.h"
#include "ElaContentDialog.h"
#include "ElaIconButton.h"
#include "ElaLineEdit.h"
#include "ElaInputDialog.h"
#include "ElaMenu.h"
#include "ElaMessageBar.h"
#include "ElaPushButton.h"
#include "ElaScrollArea.h"
#include "ElaScrollBar.h"
#include "ElaScrollPageArea.h"
#include "ElaText.h"
#include "ElaTheme.h"

#include "shared/theme_styles.h"

#include <QAction>
#include <QColor>
#include <QCursor>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QLabel>
#include <QLayoutItem>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

namespace
{

constexpr int kRailWidth = 190;
constexpr int kCardColumns = 2;
constexpr int kCardGap = 10;
constexpr int kRailDotSize = 8;
// Ela 浮层提示停留时长：够读完一行中文，又不至于挡住下面的操作。
constexpr int kMessageMsec = 2600;

ElaText* makeLabel(const QString& text, int pixelSize, QWidget* parent, bool strong = false)
{
    auto* label = new ElaText(text, pixelSize, parent);
    label->setWordWrap(false);
    if (strong) {
        label->setTextStyle(ElaTextType::BodyStrong);
        // setTextStyle 会顺手改字号，这里必须复位回调用方要的像素大小。
        label->setTextPixelSize(pixelSize);
    }
    return label;
}

ElaScrollPageArea* makeCard(QWidget* parent)
{
    auto* card = new ElaScrollPageArea(parent);
    // ElaScrollPageArea 构造里写死了 setFixedHeight(75)，当容器用必须先解开。
    card->setMinimumHeight(0);
    card->setMaximumHeight(QWIDGETSIZE_MAX);
    card->setBorderRadius(14);
    return card;
}

QString displayNameFor(const QString& exeName)
{
    QString name = exeName.trimmed();
    if (name.isEmpty()) {
        return QStringLiteral("未知应用");
    }
    if (name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        name.chop(4);
    }
    if (name.isEmpty()) {
        return QStringLiteral("未知应用");
    }
    name[0] = name.at(0).toUpper();
    return name;
}

QString shownNameFor(const QString& exeName, const QString& displayName)
{
    const QString trimmed = displayName.trimmed();
    return trimmed.isEmpty() ? displayNameFor(exeName) : trimmed;
}

} // namespace

namespace patina::ui
{

ClassificationPage::ClassificationPage(data::Database* database, QWidget* parent)
    : QWidget(parent)
    , _database(database)
{
    buildUi();

    // 分类表是内置常量，先装好：页头摘要和左侧分类栏在读到数据之前就有内容。
    _categories = data::categoryTokens();
    _summaryLabel->setText(tr("正在读取应用列表…"));
    _dirtyLabel->setText(tr("已保存"));
    _saveButton->setEnabled(false);
    _discardButton->setEnabled(false);
    refreshThemeStyles(eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark);

    connect(eTheme, &ElaTheme::themeModeChanged, this, [this](ElaThemeType::ThemeMode mode) {
        refreshThemeStyles(mode == ElaThemeType::ThemeMode::Dark);
    });
}

void ClassificationPage::refreshThemeStyles(bool dark)
{
    if (_dirtyLabel == nullptr) {
        return;
    }
    // 脏标记的颜色取决于当前有没有未保存改动，不能一律按次要色刷。
    if (_patches.isEmpty()) {
        _dirtyLabel->setStyleSheet(shared::secondaryTextStyle(dark));
    } else {
        _dirtyLabel->setStyleSheet(shared::dangerTextStyle(dark));
    }
    // 侧栏行与空态提示都是随列表重建的，重建时已按当前主题上色；
    // 主题切换后重铺一遍，省得逐个记住指针。
    for (QLabel* label : findChildren<QLabel*>()) {
        if (label->property("patinaSecondary").toBool()) {
            label->setStyleSheet(shared::secondaryTextStyle(dark));
        }
    }
    // 侧栏行的 hover/选中底色写在行自己的样式表里，只能整条重建。
    // 还没读过库时不必重建（那会儿侧栏本来就是空的）。
    if (_loaded) {
        rebuildRail();
    }
}

QString ClassificationPage::secondaryStyle() const
{
    return shared::secondaryTextStyle(eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark);
}

ClassificationPage::~ClassificationPage() = default;

bool ClassificationPage::eventFilter(QObject* watched, QEvent* event)
{
    // 左栏色点：左键点击直接弹 ElaColorDialog 改分类色（dot 上带
    // patinaCategoryId 属性标记；other 的固定色不装过滤器，走不到这里）。
    if (event->type() == QEvent::MouseButtonRelease
        && watched->property("patinaCategoryId").isValid()) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            const QString categoryId = watched->property("patinaCategoryId").toString();
            const QString colorHex =
                watched->property("patinaCategoryColor").toString();
            onCategoryColor(categoryId, colorHex);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ClassificationPage::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 16, 24, 24);
    layout->setSpacing(14);

    // ---- 页头 ----
    auto* header = new QWidget(this);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(12);

    auto* titleBox = new QWidget(header);
    auto* titleLayout = new QVBoxLayout(titleBox);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(2);
    titleLayout->addWidget(makeLabel(tr("分类管理"), 19, titleBox, true));
    _summaryLabel = makeLabel(QString(), 12, titleBox);
    titleLayout->addWidget(_summaryLabel);
    headerLayout->addWidget(titleBox, 1);

    _dirtyLabel = makeLabel(QString(), 12, header);
    headerLayout->addWidget(_dirtyLabel, 0, Qt::AlignVCenter);

    // 隐藏应用找不回来就麻烦了：给一个常驻的找回入口，有隐藏时才显示。
    // 文案是「已隐藏 N 个」，比「放弃/保存」长，所以给它更大的尺寸，
    // 否则字数一多就被挤成省略号。
    _hiddenButton = new ElaPushButton(QString(), header);
    _hiddenButton->setFixedHeight(36);
    _hiddenButton->setMinimumWidth(112);
    _hiddenButton->setVisible(false);
    headerLayout->addWidget(_hiddenButton, 0, Qt::AlignVCenter);

    _discardButton = new ElaPushButton(tr("放弃"), header);
    _discardButton->setFixedHeight(32);
    _discardButton->setMinimumWidth(72);
    headerLayout->addWidget(_discardButton, 0, Qt::AlignVCenter);

    _saveButton = new ElaPushButton(tr("保存"), header);
    _saveButton->setFixedHeight(32);
    _saveButton->setMinimumWidth(72);
    headerLayout->addWidget(_saveButton, 0, Qt::AlignVCenter);

    layout->addWidget(header);


    // ---- 主体：左分类栏 + 右卡片区 ----
    auto* body = new QWidget(this);
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(14);

    auto* railCard = makeCard(body);
    railCard->setFixedWidth(kRailWidth);
    auto* railOuter = new QVBoxLayout(railCard);
    railOuter->setContentsMargins(14, 12, 14, 12);
    railOuter->setSpacing(4);

    auto* railHead = new QWidget(railCard);
    auto* railHeadLayout = new QHBoxLayout(railHead);
    railHeadLayout->setContentsMargins(0, 0, 0, 0);
    railHeadLayout->setSpacing(4);
    railHeadLayout->addWidget(makeLabel(tr("分类"), 11, railHead, true));
    railHeadLayout->addStretch();
    // 新增分类入口（即时落库）。
    _addCategoryButton = new QPushButton(tr("＋"), railHead);
    _addCategoryButton->setFixedSize(22, 22);
    _addCategoryButton->setCursor(Qt::PointingHandCursor);
    _addCategoryButton->setFlat(true);
    _addCategoryButton->setToolTip(tr("新增分类"));
    railHeadLayout->addWidget(_addCategoryButton);
    railOuter->addWidget(railHead);

    auto* railScroll = new ElaScrollArea(railCard);
    railScroll->setWidgetResizable(true);
    railScroll->setIsGrabGesture(false);
    railScroll->setFrameShape(QFrame::NoFrame);
    // QScrollArea 的视口默认按调色板 Base 刷底（浅色主题下就是纯白），
    // 深色主题里会在卡片中间留一整块白，和四周的深色卡片撞在一起。
    // 透明掉，让 ElaScrollPageArea 的底色一路透上来。
    railScroll->setStyleSheet(QStringLiteral("background:transparent;"));
    railScroll->viewport()->setAutoFillBackground(false);
    auto* railHost = new QWidget(railScroll);
    railHost->setStyleSheet(QStringLiteral("background:transparent;"));
    _railLayout = new QVBoxLayout(railHost);
    _railLayout->setContentsMargins(0, 6, 0, 0);
    _railLayout->setSpacing(4);
    _railLayout->addStretch();
    railScroll->setWidget(railHost);
    auto* railBar = new ElaScrollBar(railScroll->verticalScrollBar(), railScroll);
    railBar->setIsAnimation(true);
    railOuter->addWidget(railScroll, 1);
    bodyLayout->addWidget(railCard, 0);

    auto* mainCard = makeCard(body);
    auto* mainLayout = new QVBoxLayout(mainCard);
    mainLayout->setContentsMargins(14, 12, 14, 12);
    mainLayout->setSpacing(10);

    auto* mainHead = new QWidget(mainCard);
    auto* mainHeadLayout = new QHBoxLayout(mainHead);
    mainHeadLayout->setContentsMargins(0, 0, 0, 0);
    mainHeadLayout->setSpacing(12);
    _searchEdit = new ElaLineEdit(mainHead);
    _searchEdit->setPlaceholderText(tr("搜索应用名或 exe"));
    _searchEdit->setIsClearButtonEnable(true);
    _searchEdit->setBorderRadius(8);
    mainHeadLayout->addWidget(_searchEdit, 1);
    _countLabel = makeLabel(QString(), 12, mainHead);
    mainHeadLayout->addWidget(_countLabel, 0, Qt::AlignVCenter);

    // 双列卡片 / 单列列表切换。ElaIconButton 是透明底的图标按钮
    // （与卡片标题行的其它图标按钮一致），比文字按钮轻量。
    _layoutButton = new ElaIconButton(
        QSettings().value(QStringLiteral("classification/layoutMode"), QStringLiteral("grid"))
                .toString() == QStringLiteral("grid")
            ? ElaIconType::TableCellsLarge   // 当前双列 → 点它切单列
            : ElaIconType::BarsStaggered,    // 当前单列 → 点它切双列
        14, 26, 26, mainHead);
    _layoutButton->setToolTip(tr("切换 双列卡片 / 单列列表"));
    connect(_layoutButton, &ElaIconButton::clicked, this, [this]() {
        const QString current =
            QSettings().value(QStringLiteral("classification/layoutMode"), QStringLiteral("grid"))
                .toString();
        const QString next = current == QStringLiteral("grid") ? QStringLiteral("list")
                                                               : QStringLiteral("grid");
        QSettings().setValue(QStringLiteral("classification/layoutMode"), next);
        refreshLayoutButton();
        applyFilter();
    });
    mainHeadLayout->addWidget(_layoutButton, 0, Qt::AlignVCenter);
    mainLayout->addWidget(mainHead);

    auto* cardsScroll = new ElaScrollArea(mainCard);
    cardsScroll->setWidgetResizable(true);
    cardsScroll->setIsGrabGesture(false);
    cardsScroll->setFrameShape(QFrame::NoFrame);
    // 同上：视口底色必须透明，否则深色主题下卡片区是一大块白。
    cardsScroll->setStyleSheet(QStringLiteral("background:transparent;"));
    cardsScroll->viewport()->setAutoFillBackground(false);
    _cardsHost = new QWidget(cardsScroll);
    _cardsHost->setStyleSheet(QStringLiteral("background:transparent;"));
    _cardsLayout = new QGridLayout(_cardsHost);
    _cardsLayout->setContentsMargins(0, 0, 2, 0);
    _cardsLayout->setHorizontalSpacing(kCardGap);
    _cardsLayout->setVerticalSpacing(kCardGap);
    _cardsLayout->setAlignment(Qt::AlignTop);
    cardsScroll->setWidget(_cardsHost);
    auto* cardsBar = new ElaScrollBar(cardsScroll->verticalScrollBar(), cardsScroll);
    cardsBar->setIsAnimation(true);
    mainLayout->addWidget(cardsScroll, 1);
    bodyLayout->addWidget(mainCard, 1);

    layout->addWidget(body, 1);

    auto* note = makeLabel(
        tr("口径：关掉「计入统计」后，该应用的历史段仍保留，但不再进入今日概览与数据页的任何汇总。"
           "「不在列表显示」更进一步——所有页面都不再显示这个应用，记录本身同样不受影响。"),
        11, this);
    note->setWordWrap(true);
    layout->addWidget(note);

    connect(_searchEdit, &ElaLineEdit::textChanged, this, [this](const QString& text) {
        _searchText = text.trimmed();
        applyFilter();
    });
    connect(_discardButton, &QPushButton::clicked, this, &ClassificationPage::onDiscard);
    connect(_saveButton, &QPushButton::clicked, this, &ClassificationPage::onSave);
    connect(_hiddenButton, &QPushButton::clicked, this, &ClassificationPage::showHiddenAppsMenu);
    connect(_addCategoryButton, &QPushButton::clicked, this, &ClassificationPage::onCreateCategory);

    // 8.1 快速分类：选择只进草稿，与卡片下拉、改名走同一条 applyEdit 通路；
    // 点「保存」才落库，保住页面「攒草稿 → 保存」的一致模型。
    _quickClassification = new QuickClassificationMenu(_database, this);
    _quickClassification->setHandlers(
        [this](const QString& exeName, const QString& displayName) {
            const data::AppClassificationEntry* entry = entryByExeName(exeName);
            if (entry == nullptr) {
                return;
            }
            applyEdit(entry->appId, [&displayName](data::AppClassificationPatch& patch) {
                patch.displayName = displayName;
            });
        },
        [this](const QString& exeName, const QString& categoryId) {
            const data::AppClassificationEntry* entry = entryByExeName(exeName);
            if (entry == nullptr) {
                return;
            }
            applyEdit(entry->appId, [&categoryId](data::AppClassificationPatch& patch) {
                patch.categoryId = categoryId;
            });
        });

    // 关联应用：成员并到父应用名下后，卡片列表按归并口径重读一次。
    _quickClassification->setAppLinkHandler(
        [this](const QString& exeName, const QString& parentExeName) {
            saveAppLink(exeName, parentExeName);
        });

    // 按应用关掉标题记录。这项直接落 settings，不进分类草稿——
    // 它跟「重命名/改分类」不是同一种改动，不该跟着保存/取消按钮走。
    _quickClassification->setCaptureTitleHandler(
        [this](const QString& exeName, bool captureTitle) {
            saveAppCaptureTitle(exeName, captureTitle);
        });

    // 「不在列表显示」：隐藏后卡片从列表消失（草稿一并作废），其余页面下次
    // 刷新时也不再出现这个应用。
    _quickClassification->setHiddenHandler(
        [this](const QString& exeName, bool hidden) { saveAppHidden(exeName, hidden); });
}

void ClassificationPage::setActive(bool active)
{
    _active = active;
    if (!active) {
        // 离开页面时把卡片全释放，几百个控件的字体度量比位图更占内存。
        // 草稿（_entries/_patches）保留：用户常常是改完去概览页对一眼数字就回来，
        // 这时候把未保存的改动清掉，等于让人白改一遍。
        clearCards();
        return;
    }
    // 没有未保存的改动时重查一次库：追踪线程可能刚记下新应用。
    // 有草稿时只重建界面，绝不清空 _patches。
    if (!_loaded || _patches.isEmpty()) {
        reload();
        return;
    }
    // 切页动画先跑：Ela 的切页有过渡，同步重建十几张卡（每张含 combo/switch 等
    // 重型 Ela 控件）会让动画掉帧，体感是「切过来卡一下」。延后到事件循环
    // 下一拍，动画结束后再铺卡片。
    QTimer::singleShot(0, this, [this]() {
        if (!_active) {
            return; // 延迟期间又切走了
        }
        rebuildRail();
        applyFilter();
    });
}

void ClassificationPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!_active) {
        return;
    }
    if (!_loaded) {
        reload();
        return;
    }
    // 卡片在 hideEvent 里被释放过：重建，但不重查库，
    // 否则最小化再回来就把未保存的改动丢了。
    if (_cards.isEmpty()) {
        rebuildRail();
        applyFilter();
    }
}

void ClassificationPage::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    // 整窗隐藏（比如最小化到托盘）时也把卡片控件释放掉。
    // 只放控件，不动 _entries/_patches —— 那里面装着用户还没保存的改动。
    clearCards();
}

void ClassificationPage::reload()
{
    if (!_database || !_database->isOpen()) {
        ElaMessageBar::warning(ElaMessageBarType::Top, tr("数据库未就绪"),
                               tr("无法读取应用列表，请确认数据目录是否可写。"), kMessageMsec, this);
        return;
    }

    // 分类列表走注册表（内置 - 墓碑 + 自定义，含改名/配色覆盖）。
    // 读失败时退回内置表，页面照常可用（只是看不到自定义分类）。
    const data::Result<QVector<data::CategoryEntry>> registry =
        data::loadCategoryRegistry(*_database);
    if (registry.ok()) {
        _categoryEntries = registry.value;
        QVector<data::CategoryToken> tokens;
        tokens.reserve(registry.value.size());
        for (const data::CategoryEntry& entry : registry.value) {
            data::CategoryToken token;
            token.id = entry.id;
            token.label = entry.label;
            token.colorHex = entry.colorHex;
            tokens.append(token);
        }
        _categories = tokens;
    } else {
        _categoryEntries.clear();
        _categories = data::categoryTokens();
    }

    data::ClassificationRepository repository(*_database);
    const data::Result<QVector<data::AppClassificationEntry>> result = repository.listApps();
    if (!result.ok()) {
        ElaMessageBar::error(ElaMessageBarType::Top, tr("读取应用列表失败"), result.error,
                             kMessageMsec, this);
        return;
    }

    _loading = true;
    _entries = result.value;
    _patches.clear();
    _loading = false;
    _loaded = true;

    rebuildRail();
    applyFilter();
}

void ClassificationPage::rebuildRail()
{
    // 保留末尾 stretch，先清掉前面的所有项。
    // 用 deleteLater 而不是 delete：点击处理里就会走到这儿，直接 delete 会删掉正在发信号的按钮。
    while (_railLayout->count() > 1) {
        QLayoutItem* item = _railLayout->takeAt(0);
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    const auto makeRow = [this](const QString& categoryId, const QString& label,
                                const QString& colorHex, int count,
                                const data::CategoryEntry* entry) {
        const bool dark = eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark;
        auto* button = new QPushButton(_railLayout->parentWidget());
        button->setCheckable(true);
        button->setChecked(_categoryFilter == categoryId);
        button->setCursor(Qt::PointingHandCursor);
        button->setFlat(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setStyleSheet(QStringLiteral(
            "QPushButton { text-align:left; border:none; border-radius:8px; padding:7px 10px;"
            " background:transparent; outline:none; }"
            "QPushButton:hover { background:%1; }"
            "QPushButton:checked { background:%2; }"
            "QPushButton:focus { border:none; outline:none; }")
            .arg(shared::accentTint(dark, 20), shared::accentTint(dark, 36)));

        auto* row = new QHBoxLayout(button);
        // QSS 的 padding 管不到塞进按钮里的 layout：子控件的左右缩进必须
        // 在这里用 contentsMargins 给（与 padding:7px 10px 对齐）。
        row->setContentsMargins(10, 7, 10, 7);
        row->setSpacing(6);

        // 色点：点击直接弹 ElaColorDialog（按用户要求，不再经菜单中转）。
        // other 的固定色不可改，色点不可点。
        if (!colorHex.isEmpty()) {
            auto* dot = new QLabel(button);
            dot->setFixedSize(kRailDotSize, kRailDotSize);
            dot->setStyleSheet(
                QStringLiteral("background:%1; border-radius:3px;").arg(colorHex));
            if (categoryId != QStringLiteral("other")) {
                dot->setCursor(Qt::PointingHandCursor);
                dot->setToolTip(tr("修改颜色"));
                // QLabel 没有 clicked 信号：装 MouseListener 事件过滤最轻。
                dot->installEventFilter(this);
                dot->setProperty("patinaCategoryId", categoryId);
                dot->setProperty("patinaCategoryColor", colorHex);
            }
            row->addWidget(dot, 0, Qt::AlignVCenter);
        }

        auto* nameLabel = makeLabel(label, 12, button);
        row->addWidget(nameLabel, 0, Qt::AlignVCenter);

        // 显式 stretch 把计数推到行尾（依赖 nameLabel 的 stretch 因子在
        // ElaText 的 sizePolicy 下不可靠：数字会贴在名字旁）。
        row->addStretch(1);

        auto* countLabel = makeLabel(QString::number(count), 11, button);
        // 打上标记：主题切换时 refreshThemeStyles() 靠它找到这些次要色标签。
        countLabel->setProperty("patinaSecondary", true);
        countLabel->setStyleSheet(secondaryStyle());
        row->addWidget(countLabel, 0, Qt::AlignVCenter);

        // 编辑入口（改名/配色/删除）收敛到右键菜单，全部 ElaMenu。
        // 配色直接作为内嵌二级菜单（悬停「修改颜色」弹出 10 个预设色 +
        // 自定义颜色 + 恢复默认色），不用再二次弹窗。
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(button, &QPushButton::customContextMenuRequested, this,
                [this, button, categoryId, label, colorHex, entry](const QPoint& pos) {
                    if (entry == nullptr) {
                        return; // 「全部」行没有编辑菜单
                    }
                    auto* menu = new ElaMenu(this);
                    menu->setAttribute(Qt::WA_DeleteOnClose);
                    QAction* renameAction = menu->addAction(tr("重命名…"));
                    connect(renameAction, &QAction::triggered, this,
                            [this, categoryId, label]() {
                                onRenameCategory(categoryId, label);
                            });

                    // OTHER_CATEGORY_FIXED_COLOR：other 颜色固定，不出配色子菜单；
                    // 兜底分类的名称也是界面约定的一部分，同样不可改名。
                    const bool isOther = categoryId == QStringLiteral("other");
                    if (!isOther) {
                        // 配色子菜单只留两项（按用户要求去掉分类色板）：
                        // 自定义颜色…（ElaColorDialog）/ 恢复默认色。
                        auto* colorMenu = menu->addMenu(tr("修改颜色"));
                        QAction* customAction = colorMenu->addAction(tr("自定义颜色…"));
                        connect(customAction, &QAction::triggered, this,
                                [this, categoryId, colorHex]() {
                                    onCategoryColor(categoryId, colorHex);
                                });
                        QAction* resetAction = colorMenu->addAction(tr("恢复默认色"));
                        connect(resetAction, &QAction::triggered, this,
                                [this, categoryId]() {
                                    data::ClassificationRepository repository(*_database);
                                    const data::Status reset =
                                        repository.setCategoryColor(categoryId, QString());
                                    if (!reset.ok()) {
                                        ElaMessageBar::error(ElaMessageBarType::Top,
                                                             tr("重置失败"), reset.error,
                                                             kMessageMsec, this);
                                        return;
                                    }
                                    reloadAfterCategoryChange();
                                });
                    }

                    menu->addSeparator();
                    QAction* deleteAction =
                        (entry->deletable || entry->deletableBuiltin)
                            ? menu->addAction(tr("删除分类"))
                            : nullptr;
                    if (deleteAction != nullptr) {
                        connect(deleteAction, &QAction::triggered, this,
                                [this, categoryId, label]() {
                                    onDeleteCategory(categoryId, label);
                                });
                    } else {
                        QAction* note = menu->addAction(tr("兜底分类不可编辑或删除"));
                        note->setEnabled(false);
                    }

                    // popup 非阻塞：各动作已在上面各自 connect，菜单自管生命周期。
                    menu->popup(button->mapToGlobal(QPoint(pos.x(), pos.y())));
                });

        connect(button, &QPushButton::clicked, this, [this, categoryId]() {
            _categoryFilter = categoryId;
            rebuildRail();
            applyFilter();
        });

        _railLayout->insertWidget(_railLayout->count() - 1, button);
    };

    int visibleCount = 0;
    for (const data::AppClassificationEntry& entry : _entries) {
        if (!patchFor(entry).hidden) {
            ++visibleCount;
        }
    }
    makeRow(QString(), tr("全部"), QString(), visibleCount, nullptr);

    for (const data::CategoryToken& token : _categories) {
        int count = 0;
        for (const data::AppClassificationEntry& entry : _entries) {
            const data::AppClassificationPatch patch = patchFor(entry);
            // 隐藏的应用不参与左栏计数：否则「全部 40」点进去只有 35 张卡。
            if (patch.hidden) {
                continue;
            }
            const QString effective = data::resolveCategoryId(entry.exeName, patch.categoryId);
            if (effective == token.id) {
                ++count;
            }
        }
        // 找回带标记的完整条目（builtin/deletable 状态只存注册表里）。
        const data::CategoryEntry* entry = nullptr;
        for (const data::CategoryEntry& candidate : _categoryEntries) {
            if (candidate.id == token.id) {
                entry = &candidate;
                break;
            }
        }
        makeRow(token.id, token.label, token.colorHex, count, entry);
    }
}

void ClassificationPage::applyFilter()
{
    clearCards();

    // 布局偏好（双列卡片 / 单列列表），settings 持久化重启保持。
    // 纯本地键值，不进数据库。
    const QString layoutMode =
        QSettings().value(QStringLiteral("classification/layoutMode"), QStringLiteral("grid"))
            .toString();
    const int columns = layoutMode == QStringLiteral("list") ? 1 : kCardColumns;
    QVector<const data::AppClassificationEntry*> visible;
    visible.reserve(_entries.size());
    for (const data::AppClassificationEntry& entry : _entries) {
        const data::AppClassificationPatch patch = patchFor(entry);
        // 「不在列表显示」的应用连分类页也不出现（找回入口在页头的「已隐藏」里）。
        if (patch.hidden) {
            continue;
        }
        if (!_categoryFilter.isEmpty()) {
            const QString effective = data::resolveCategoryId(entry.exeName, patch.categoryId);
            if (effective != _categoryFilter) {
                continue;
            }
        }
        if (!_searchText.isEmpty()) {
            const QString name = shownNameFor(entry.exeName, patch.displayName);
            if (!name.contains(_searchText, Qt::CaseInsensitive)
                && !entry.exeName.contains(_searchText, Qt::CaseInsensitive)) {
                continue;
            }
        }
        visible.append(&entry);
    }

    int row = 0;
    int column = 0;
    for (const data::AppClassificationEntry* entry : visible) {
        auto* card = new AppClassificationCard(_cardsHost);
        card->setCategories(_categories);
        card->setEntry(*entry, patchFor(*entry));

        // 信号里只认 appId，不捕获 entry 指针：_entries 一旦重载，旧指针就悬空了。
        connect(card, &AppClassificationCard::nameEdited, this,
                [this](qint64 id, const QString& displayName) {
                    applyEdit(id, [&displayName](data::AppClassificationPatch& patch) {
                        patch.displayName = displayName;
                    });
                });
        connect(card, &AppClassificationCard::categoryEdited, this,
                [this](qint64 id, const QString& categoryId) {
                    applyEdit(id, [&categoryId](data::AppClassificationPatch& patch) {
                        patch.categoryId = categoryId;
                    });
                });
        connect(card, &AppClassificationCard::colorEdited, this,
                [this](qint64 id, const QString& colorHex) {
                    applyEdit(id, [&colorHex](data::AppClassificationPatch& patch) {
                        patch.colorHex = colorHex;
                    });
                });
        connect(card, &AppClassificationCard::trackEdited, this,
                [this](qint64 id, bool enabled) {
                    applyEdit(id, [enabled](data::AppClassificationPatch& patch) {
                        patch.trackEnabled = enabled;
                    });
                });

        _cardsLayout->addWidget(card, row, column);
        _cards.append(card);

        // 8.1 快速分类：卡片右键 → 弹快速菜单（选择记进草稿）。
        connect(card, &AppClassificationCard::quickMenuRequested, this,
                [this](qint64 id, const QPoint& globalPos) { openCardQuickMenu(id, globalPos); });

        ++column;
        if (column >= columns) {
            column = 0;
            ++row;
        }
    }

    if (visible.isEmpty()) {
        auto* empty = makeLabel(_entries.isEmpty() ? tr("还没有记录到任何应用。")
                                                   : tr("没有匹配的应用。"),
                                12, _cardsHost);
        empty->setProperty("patinaSecondary", true);
        empty->setStyleSheet(secondaryStyle());
        _cardsLayout->addWidget(empty, 0, 0, 1, columns);
    }

    _countLabel->setText(tr("%1 个应用").arg(static_cast<int>(visible.size())));
    refreshHeader();
}

void ClassificationPage::clearCards()
{
    for (AppClassificationCard* card : _cards) {
        _cardsLayout->removeWidget(card);
        // 先藏起来再 deleteLater：否则这一帧新卡片已经就位、旧卡片还挂在父控件上没被销毁。
        card->hide();
        card->deleteLater();
    }
    _cards.clear();

    // 卡片已 removeWidget，这里剩下的只有空态标签；不清掉会和新卡片叠在一起。
    while (QLayoutItem* item = _cardsLayout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

void ClassificationPage::refreshHeader()
{
    // Qt6 的 size() 是 qsizetype，先转 int 再参与比较，避免 qsizetype/int 混算的窄化警告。
    const int dirtyCount = static_cast<int>(_patches.size());
    const bool dark = eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark;
    if (dirtyCount == 0) {
        _dirtyLabel->setText(tr("已保存"));
        _dirtyLabel->setStyleSheet(shared::secondaryTextStyle(dark));
    } else {
        _dirtyLabel->setText(tr("有 %1 处未保存的改动").arg(dirtyCount));
        _dirtyLabel->setStyleSheet(shared::dangerTextStyle(dark));
    }
    _saveButton->setEnabled(dirtyCount > 0);
    _discardButton->setEnabled(dirtyCount > 0);

    int excluded = 0;
    int hidden = 0;
    for (const data::AppClassificationEntry& entry : _entries) {
        const data::AppClassificationPatch patch = patchFor(entry);
        if (patch.hidden) {
            ++hidden;
            continue;
        }
        if (!patch.trackEnabled) {
            ++excluded;
        }
    }
    if (_hiddenButton != nullptr) {
        _hiddenButton->setVisible(hidden > 0);
        _hiddenButton->setText(tr("已隐藏 %1 个").arg(hidden));
        _hiddenButton->setToolTip(tr("点开可以把隐藏的应用恢复显示"));
    }
    _summaryLabel->setText(tr("%1 个应用 · %2 个分类 · %3 个已排除统计")
                               .arg(static_cast<int>(_entries.size()) - hidden)
                               .arg(static_cast<int>(_categories.size()))
                               .arg(excluded));
}

data::AppClassificationPatch ClassificationPage::patchFor(
    const data::AppClassificationEntry& entry) const
{
    const auto it = _patches.constFind(entry.appId);
    if (it != _patches.constEnd()) {
        return it.value();
    }

    data::AppClassificationPatch patch;
    patch.appId = entry.appId;
    patch.displayName = entry.displayName;
    patch.categoryId = entry.categoryId;
    patch.colorHex = entry.colorHex;
    patch.trackEnabled = entry.trackEnabled;
    patch.hidden = entry.hidden;
    return patch;
}

const data::AppClassificationEntry* ClassificationPage::entryByExeName(
    const QString& exeName) const
{
    if (exeName.trimmed().isEmpty()) {
        return nullptr;
    }
    for (const data::AppClassificationEntry& entry : _entries) {
        // apps.exe_name 是 COLLATE NOCASE，这里按同一语义比对。
        if (entry.exeName.compare(exeName, Qt::CaseInsensitive) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

void ClassificationPage::openCardQuickMenu(qint64 appId, const QPoint& globalPos)
{
    if (_quickClassification == nullptr || appId == 0) {
        return;
    }
    const data::AppClassificationEntry* entry = nullptr;
    for (const data::AppClassificationEntry& candidate : _entries) {
        if (candidate.appId == appId) {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr) {
        return;
    }

    // 展示名与生效分类都取当前草稿：右键弹出的菜单要和卡片上看到的一致。
    const data::AppClassificationPatch patch = patchFor(*entry);
    const QString shownName = shownNameFor(entry->exeName, patch.displayName);
    const QString effectiveCategory =
        data::resolveCategoryId(entry->exeName, patch.categoryId);
    // 当前已关联的父应用（没有则为空），给「关联应用」子菜单打勾。
    QString currentParent;
    if (_database != nullptr && _database->isOpen()) {
        const data::Result<data::AppLinks> loadedLinks = data::loadAppLinks(*_database);
        if (loadedLinks.ok()) {
            currentParent = loadedLinks.value.value(entry->exeName.trimmed().toLower());
        }
    }
    // 这个应用当前记不记标题（没设过就是记），给开关打勾。
    bool captureTitle = true;
    if (_database != nullptr && _database->isOpen()) {
        const data::Result<data::AppOverridePatch> loadedOverride =
            data::loadAppOverride(*_database, entry->exeName);
        if (loadedOverride.ok() && loadedOverride.value.captureTitle.has_value()) {
            captureTitle = *loadedOverride.value.captureTitle;
        }
    }
    if (_quickClassification->showCategoryMenu(entry->exeName, effectiveCategory, globalPos,
                                               currentParent, captureTitle, patch.hidden)) {
        _quickClassification->showRename(entry->exeName, shownName, globalPos);
    }
}

void ClassificationPage::saveAppCaptureTitle(const QString& exeName, bool captureTitle)
{
    if (_database == nullptr || !_database->isOpen() || exeName.trimmed().isEmpty()) {
        return;
    }

    data::AppOverridePatch patch;
    patch.exeName = exeName;
    patch.captureTitle = captureTitle;

    const data::Status saved = data::saveAppOverrides(*_database, {patch});
    if (!saved.ok()) {
        ElaMessageBar::error(ElaMessageBarType::Top, tr("设置失败"), saved.error,
                             kMessageMsec, this);
        return;
    }

    // 引擎的覆盖项缓存有 5 秒 TTL，不需要主动失效。
    ElaMessageBar::success(ElaMessageBarType::Top, tr("已更新"),
                           captureTitle ? tr("「%1」已恢复记录窗口标题。").arg(exeName)
                                        : tr("「%1」不再记录窗口标题。").arg(exeName),
                           kMessageMsec, this);
}

void ClassificationPage::saveAppLink(const QString& exeName, const QString& parentExeName)
{
    if (_database == nullptr || !_database->isOpen() || exeName.trimmed().isEmpty()) {
        return;
    }

    const data::Status saved = data::setAppLink(*_database, exeName, parentExeName);
    if (!saved.ok()) {
        ElaMessageBar::error(ElaMessageBarType::Top, tr("关联失败"), saved.error,
                             kMessageMsec, this);
        return;
    }

    // 关联只改读时归并口径，不动 apps 表；重读一次让卡片列表跟上。
    // 草稿 _patches 保留：里面是应用级的展示名/分类改动，与关联无关。
    reloadAfterCategoryChange();
    ElaMessageBar::success(ElaMessageBarType::Top, tr("已更新"),
                           parentExeName.trimmed().isEmpty()
                               ? tr("「%1」已解除关联。").arg(exeName)
                               : tr("「%1」已并入「%2」。").arg(exeName, parentExeName),
                           kMessageMsec, this);
}

void ClassificationPage::saveAppHidden(const QString& exeName, bool hidden)
{
    if (_database == nullptr || !_database->isOpen() || exeName.trimmed().isEmpty()) {
        return;
    }

    // 与 DashboardPage::saveQuickOverride 同一套「整份状态」写法：读现值合成
    // 一整条 patch，只改 hidden 一列，其余原样交回去，否则会把用户改好的
    // 展示名/分类/配色一起冲掉。
    data::AppRepository apps(*_database);
    const data::Result<std::optional<data::App>> current = apps.findByExeName(exeName);
    if (!current.ok()) {
        ElaMessageBar::error(ElaMessageBarType::Top, tr("设置失败"), current.error,
                             kMessageMsec, this);
        return;
    }
    if (!current.value.has_value()) {
        ElaMessageBar::error(ElaMessageBarType::Top, tr("设置失败"),
                             tr("应用 %1 还没有记录。").arg(exeName), kMessageMsec, this);
        return;
    }

    data::AppClassificationPatch patch;
    patch.appId = current.value->id;
    patch.displayName = current.value->displayName;
    patch.categoryId = current.value->categoryId;
    patch.colorHex = current.value->colorHex;
    patch.trackEnabled = current.value->trackEnabled;
    patch.hidden = hidden;

    data::ClassificationRepository repository(*_database);
    const data::Status saved = repository.applyPatches({patch});
    if (!saved.ok()) {
        ElaMessageBar::error(ElaMessageBarType::Top, tr("设置失败"), saved.error,
                             kMessageMsec, this);
        return;
    }

    // 隐藏状态是即时落库的：草稿里这条作废（隐藏的卡片马上要消失，
    // 留着一条改不回去的草稿只会让「保存」按钮亮着没意义）。
    _patches.remove(patch.appId);
    reload();
    ElaMessageBar::success(ElaMessageBarType::Top, tr("已更新"),
                           hidden ? tr("「%1」已从各页面隐藏，可在「已隐藏」里恢复。").arg(exeName)
                                  : tr("「%1」已恢复显示。").arg(exeName),
                           kMessageMsec, this);
}

void ClassificationPage::showHiddenAppsMenu()
{
    QVector<const data::AppClassificationEntry*> hiddenEntries;
    for (const data::AppClassificationEntry& entry : _entries) {
        if (entry.hidden) {
            hiddenEntries.append(&entry);
        }
    }
    if (hiddenEntries.isEmpty()) {
        return;
    }

    // 用 ElaMenu 与右键菜单保持同一套外观；点一项就恢复该项的显示。
    auto* menu = new ElaMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(tr("已隐藏 %1 个应用").arg(static_cast<int>(hiddenEntries.size())))
        ->setEnabled(false);
    menu->addSeparator();

    QHash<QAction*, QString> restoreActions;
    for (const data::AppClassificationEntry* entry : hiddenEntries) {
        const QString name = shownNameFor(entry->exeName, entry->displayName);
        QAction* action = menu->addAction(tr("恢复「%1」").arg(name));
        restoreActions.insert(action, entry->exeName);
    }

    QAction* selected = menu->exec(_hiddenButton->mapToGlobal(QPoint(0, _hiddenButton->height())));
    if (selected == nullptr) {
        return;
    }
    const auto found = restoreActions.constFind(selected);
    if (found == restoreActions.constEnd()) {
        return;
    }
    saveAppHidden(found.value(), false);
}

void ClassificationPage::applyEdit(
    qint64 appId, const std::function<void(data::AppClassificationPatch&)>& mutate)
{
    if (_loading || appId == 0) {
        return;
    }

    const data::AppClassificationEntry* entry = nullptr;
    for (const data::AppClassificationEntry& candidate : _entries) {
        if (candidate.appId == appId) {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr) {
        return;
    }

    data::AppClassificationPatch patch = patchFor(*entry);
    mutate(patch);
    updatePatch(patch);
}

void ClassificationPage::updatePatch(const data::AppClassificationPatch& patch)
{
    const qint64 appId = patch.appId;
    if (appId == 0) {
        return;
    }

    // 与库里一致时就把这条从草稿里摘掉：脏标记才是真的脏。
    bool sameAsStored = false;
    for (const data::AppClassificationEntry& entry : _entries) {
        if (entry.appId != appId) {
            continue;
        }
        sameAsStored = entry.displayName.trimmed() == patch.displayName.trimmed()
            && entry.categoryId.trimmed() == patch.categoryId.trimmed()
            && entry.colorHex.trimmed().compare(patch.colorHex.trimmed(), Qt::CaseInsensitive) == 0
            && entry.trackEnabled == patch.trackEnabled
            && entry.hidden == patch.hidden;
        break;
    }

    if (sameAsStored) {
        _patches.remove(appId);
    } else {
        data::AppClassificationPatch stored;
        stored.appId = appId;
        stored.displayName = patch.displayName.trimmed();
        stored.categoryId = patch.categoryId.trimmed();
        stored.colorHex = patch.colorHex.trimmed();
        stored.trackEnabled = patch.trackEnabled;
        // hidden 必须带着原值走：卡片上的改动不该顺手把隐藏状态清掉。
        stored.hidden = patch.hidden;
        _patches.insert(appId, stored);
    }

    // 就地刷新这一张卡片，不做整页重建：否则改完分类卡片会在光标下跳走。
    for (AppClassificationCard* card : _cards) {
        if (card->appId() != appId) {
            continue;
        }
        for (const data::AppClassificationEntry& entry : _entries) {
            if (entry.appId == appId) {
                card->setEntry(entry, patchFor(entry));
                break;
            }
        }
        break;
    }

    rebuildRail();
    refreshHeader();
}

void ClassificationPage::onSave()
{
    if (!_database || !_database->isOpen() || _patches.isEmpty()) {
        return;
    }

    QVector<data::AppClassificationPatch> patches;
    patches.reserve(_patches.size());
    for (auto it = _patches.constBegin(); it != _patches.constEnd(); ++it) {
        patches.append(it.value());
    }

    data::ClassificationRepository repository(*_database);
    const data::Status saved = repository.applyPatches(patches);
    if (!saved.ok()) {
        // 用 Ela 的浮层提示，而不是页面里一行黑字：黑字在深色主题下几乎看不见，
        // 而且它是布局里的一个元素，出现时会把下面的卡片整体顶下去。
        ElaMessageBar::error(ElaMessageBarType::Top, tr("保存失败"), saved.error,
                             kMessageMsec, this);
        return;
    }

    reload();
    ElaMessageBar::success(ElaMessageBarType::Top, tr("已保存"), tr("改动已写入本机数据库。"),
                           kMessageMsec, this);
}

void ClassificationPage::onDiscard()
{
    _patches.clear();
    applyFilter();
    rebuildRail();
}

void ClassificationPage::refreshLayoutButton()
{
    if (_layoutButton == nullptr) {
        return;
    }
    // 图标语义：「点它会变成什么」。双列模式显示单列图标（切单列），反之亦然。
    const QString current =
        QSettings().value(QStringLiteral("classification/layoutMode"), QStringLiteral("grid"))
            .toString();
    _layoutButton->setAwesome(current == QStringLiteral("grid")
                                  ? ElaIconType::TableCellsLarge
                                  : ElaIconType::BarsStaggered);
}

// ---------------- 分类本身的增删改名 / 配色 ----------------

void ClassificationPage::reloadAfterCategoryChange()
{
    // 分类级操作后：重读注册表与应用列表（草稿 _patches 保留——里面是
    // 应用级归属改动，与分类定义无关；指向已删分类的草稿由保存时的
    // 注册表校验拦下并提示）。
    const data::Result<QVector<data::CategoryEntry>> registry =
        data::loadCategoryRegistry(*_database);
    if (registry.ok()) {
        _categoryEntries = registry.value;
        QVector<data::CategoryToken> tokens;
        tokens.reserve(registry.value.size());
        for (const data::CategoryEntry& entry : registry.value) {
            data::CategoryToken token;
            token.id = entry.id;
            token.label = entry.label;
            token.colorHex = entry.colorHex;
            tokens.append(token);
        }
        _categories = tokens;
    } else {
        _categoryEntries.clear();
        _categories = data::categoryTokens();
    }

    // 应用列表也重读一次：删除分类时成员归属已在事务里回退，草稿里的
    // 旧值要跟库对齐（patchFor 以新 _entries 为基准）。
    data::ClassificationRepository repository(*_database);
    const data::Result<QVector<data::AppClassificationEntry>> apps = repository.listApps();
    if (apps.ok()) {
        _entries = apps.value;
        _loaded = true;
    }

    rebuildRail();
    applyFilter();
    refreshHeader();
}

void ClassificationPage::onCreateCategory()
{
    if (_database == nullptr || !_database->isOpen()) {
        ElaMessageBar::warning(ElaMessageBarType::Top, tr("数据库未就绪"),
                               tr("无法新增分类。"), kMessageMsec, this);
        return;
    }

    bool accepted = false;
    const QString name = ElaInputDialog::getText(
        this, tr("新增分类"), tr("输入分类名称（最多 20 字）"), tr("名称"),
        QString(), &accepted);
    if (!accepted) {
        return;
    }

    data::ClassificationRepository repository(*_database);
    const data::Result<QString> created = repository.createCategory(name);
    if (!created.ok()) {
        ElaMessageBar::error(ElaMessageBarType::Top, tr("新增失败"), created.error,
                             kMessageMsec, this);
        return;
    }

    reloadAfterCategoryChange();
    ElaMessageBar::success(ElaMessageBarType::Top, tr("已创建"),
                           tr("分类「%1」已生效。").arg(name.trimmed().simplified()),
                           kMessageMsec, this);
}

void ClassificationPage::onRenameCategory(const QString& categoryId, const QString& currentLabel)
{
    if (_database == nullptr || !_database->isOpen()) {
        return;
    }

    bool accepted = false;
    const QString name = ElaInputDialog::getText(
        this, tr("重命名分类"), tr("将「%1」改为").arg(currentLabel), tr("名称"),
        currentLabel, &accepted);
    if (!accepted) {
        return;
    }

    data::ClassificationRepository repository(*_database);
    const data::Status saved = repository.renameCategory(categoryId, name);
    if (!saved.ok()) {
        ElaMessageBar::error(ElaMessageBarType::Top, tr("重命名失败"), saved.error,
                             kMessageMsec, this);
        return;
    }

    reloadAfterCategoryChange();
    ElaMessageBar::success(ElaMessageBarType::Top, tr("已重命名"),
                           tr("分类名已更新。"), kMessageMsec, this);
}

void ClassificationPage::onDeleteCategory(const QString& categoryId, const QString& label)
{
    if (_database == nullptr || !_database->isOpen()) {
        return;
    }

    // 二次确认：删除后成员回退 other，
    // 时长保留。ElaContentDialog 与主窗口关闭弹框同款：右键 = 接受。
    // 它没有 setTitle：标题得自己写进中央部件（主窗口那个弹框也是这么做的）。
    ElaContentDialog confirmDialog(this);
    confirmDialog.setLeftButtonText(tr("取消"));
    confirmDialog.setRightButtonText(tr("删除"));
    auto* content = new QWidget();
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(15, 25, 15, 10);
    contentLayout->setSpacing(6);
    auto* title = new ElaText(tr("删除分类"), content);
    title->setWordWrap(false);
    title->setTextStyle(ElaTextType::Subtitle);
    auto* detail = new ElaText(
        tr("确定删除「%1」吗？\n该分类下的应用将回退到「其他」，统计时长不受影响。")
            .arg(label), 13, content);
    detail->setWordWrap(true);
    contentLayout->addWidget(title);
    contentLayout->addWidget(detail);
    contentLayout->addStretch();
    confirmDialog.setCentralWidget(content);
    confirmDialog.resize(400, 190);
    if (confirmDialog.exec() != QDialog::Accepted) {
        return;
    }

    data::ClassificationRepository repository(*_database);
    const data::Status removed = repository.deleteCategory(categoryId);
    if (!removed.ok()) {
        ElaMessageBar::error(ElaMessageBarType::Top, tr("删除失败"), removed.error,
                             kMessageMsec, this);
        return;
    }

    // 当前过滤器正停在被删分类上时，先回「全部」。
    if (_categoryFilter == categoryId) {
        _categoryFilter.clear();
    }
    reloadAfterCategoryChange();
    ElaMessageBar::success(ElaMessageBarType::Top, tr("已删除"),
                           tr("「%1」已删除，成员已回退。").arg(label), kMessageMsec, this);
}

void ClassificationPage::onCategoryColor(const QString& categoryId, const QString& currentColorHex)
{
    if (_database == nullptr || !_database->isOpen()) {
        return;
    }

    // 右键菜单里的「自定义颜色…」直达这里（预设色板与恢复默认色已内嵌在
    // 一级菜单的颜色子菜单里，不再二次弹菜单）。ElaColorDialog 点色即选，
    // 用 colorSelected 收结果。
    ElaColorDialog dialog(this);
    dialog.setCurrentColor(QColor(currentColorHex));
    QColor picked;
    connect(&dialog, &ElaColorDialog::colorSelected, this, [&picked](const QColor& color) {
        if (color.isValid()) {
            picked = color;
        }
    });
    const int dialogResult = dialog.exec();
    if (!picked.isValid() && dialogResult == QDialog::Accepted) {
        picked = dialog.getCurrentColor();
    }
    if (!picked.isValid()) {
        return;
    }

    data::ClassificationRepository repository(*_database);
    const data::Status saved = repository.setCategoryColor(categoryId, picked.name().toUpper());
    if (!saved.ok()) {
        ElaMessageBar::error(ElaMessageBarType::Top, tr("配色失败"), saved.error,
                             kMessageMsec, this);
        return;
    }
    reloadAfterCategoryChange();
}

} // namespace patina::ui
