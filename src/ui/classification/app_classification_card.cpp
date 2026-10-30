#include "ui/classification/app_classification_card.h"

#include "ElaColorDialog.h"
#include "ElaComboBox.h"
#include "ElaIcon.h"
#include "ElaIconButton.h"
#include "ElaLineEdit.h"
#include "ElaText.h"
#include "ElaTheme.h"
#include "ElaToggleSwitch.h"

#include "shared/app_icon_cache.h"
#include "shared/theme_styles.h"

#include <QAction>
#include <QColor>
#include <QContextMenuEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPixmap>
#include <QPoint>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace
{

constexpr int kIconSize = 32;
constexpr int kIconRadius = 9;
constexpr int kColorButtonSize = 22;

// 与 dashboard 的排行列表保持同一套显示名规则：去掉 .exe、首字母大写。
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

// 分类色 + 自定义色的合成结果：自定义色优先，为空时跟随分类。
// 分类表由页面传进来（已在 reload 里从注册表读到，含改名/配色覆盖）；
// 表里查不到时才退回内置 token 表。
QString effectiveColorHex(const QString& categoryId, const QString& colorHex,
                          const QVector<patina::data::CategoryToken>& categories)
{
    if (!colorHex.trimmed().isEmpty()) {
        return colorHex.trimmed();
    }
    for (const patina::data::CategoryToken& token : categories) {
        if (token.id == categoryId) {
            return token.colorHex;
        }
    }
    return patina::data::categoryToken(categoryId).colorHex;
}

} // namespace

namespace patina::ui
{

AppClassificationCard::AppClassificationCard(QWidget* parent)
    : QWidget(parent)
{
    buildUi();
}

AppClassificationCard::~AppClassificationCard() = default;

void AppClassificationCard::buildUi()
{
    setObjectName(QStringLiteral("appClassificationCard"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 11, 12, 11);
    layout->setSpacing(9);

    // ---- 顶部：图标 + 名称 / exe ----
    auto* top = new QWidget(this);
    auto* topLayout = new QHBoxLayout(top);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(9);

    _iconLabel = new QLabel(top);
    _iconLabel->setFixedSize(kIconSize, kIconSize);
    _iconLabel->setAlignment(Qt::AlignCenter);
    topLayout->addWidget(_iconLabel, 0, Qt::AlignVCenter);

    auto* identity = new QWidget(top);
    auto* identityLayout = new QVBoxLayout(identity);
    identityLayout->setContentsMargins(0, 0, 0, 0);
    identityLayout->setSpacing(1);

    auto* nameRow = new QWidget(identity);
    auto* nameLayout = new QHBoxLayout(nameRow);
    nameLayout->setContentsMargins(0, 0, 0, 0);
    nameLayout->setSpacing(4);

    _nameLabel = makeLabel(QString(), 13, nameRow, true);
    _nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    nameLayout->addWidget(_nameLabel, 0, Qt::AlignVCenter);

    _renameButton = new ElaIconButton(ElaIconType::Pencil, 12, nameRow);
    _renameButton->setFixedSize(22, 22);
    _renameButton->setToolTip(tr("重命名"));
    nameLayout->addWidget(_renameButton, 0, Qt::AlignVCenter);

    _excludedBadge = makeLabel(tr("不计入统计"), 10, nameRow);
    _excludedBadge->setVisible(false);
    nameLayout->addWidget(_excludedBadge, 0, Qt::AlignVCenter);
    nameLayout->addStretch();

    _nameEdit = new ElaLineEdit(nameRow);
    _nameEdit->setVisible(false);
    _nameEdit->setMinimumWidth(120);
    nameLayout->addWidget(_nameEdit, 1);

    identityLayout->addWidget(nameRow);

    _exeLabel = makeLabel(QString(), 11, identity);
    identityLayout->addWidget(_exeLabel);

    topLayout->addWidget(identity, 1);
    layout->addWidget(top);

    // ---- 底部：分类下拉 + 色块 + 统计开关 ----
    auto* controls = new QWidget(this);
    auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(8);

    _categoryBox = new ElaComboBox(controls);
    _categoryBox->setBorderRadius(8);
    _categoryBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _categoryBox->setMinimumWidth(96);
    // 分类下拉的文字色：Ela 没给接口，只能补一条 color 规则，
    // 否则深色主题下是黑字压在深色底上。
    shared::applyComboBoxTheme(_categoryBox, eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark);
    controlsLayout->addWidget(_categoryBox, 1);

    _colorButton = new QPushButton(controls);
    _colorButton->setFixedSize(kColorButtonSize, kColorButtonSize);
    _colorButton->setCursor(Qt::PointingHandCursor);
    _colorButton->setFlat(true);
    _colorButton->setToolTip(tr("颜色"));
    controlsLayout->addWidget(_colorButton, 0, Qt::AlignVCenter);

    _trackSwitch = new ElaToggleSwitch(controls);
    _trackSwitch->setToolTip(tr("计入统计"));
    controlsLayout->addWidget(_trackSwitch, 0, Qt::AlignVCenter);

    layout->addWidget(controls);

    connect(_renameButton, &ElaIconButton::clicked, this, &AppClassificationCard::beginRename);
    // 改名提交走显式路径：回车提交（returnPressed）+ 失焦提交（FocusOut 事件）。
    // 不用 editingFinished：ElaLineEdit 内部有代理控件时，点击输入框自己的
    // 某些区域也会先走一轮失焦→ editingFinished → 收起输入框
    //（「再点击输入框就自动退出编辑」）。FocusOut 在 eventFilter 里判目标。
    connect(_nameEdit, &QLineEdit::returnPressed, this, &AppClassificationCard::commitRename);
    connect(_categoryBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (_updating || index < 0 || index >= static_cast<int>(_categories.size())) {
                    return;
                }
                emit categoryEdited(_appId, _categories.at(index).id);
            });
    connect(_colorButton, &QPushButton::clicked, this, &AppClassificationCard::pickColor);
    connect(_trackSwitch, &ElaToggleSwitch::toggled, this, [this](bool checked) {
        if (_updating) {
            return;
        }
        emit trackEdited(_appId, checked);
    });

    // Esc 放弃改名，别把半截输入当提交。
    _nameEdit->installEventFilter(this);
    // 输入过程中就更新图标首字，不然要等提交才变。
    connect(_nameEdit, &QLineEdit::textChanged, this, &AppClassificationCard::refreshIcon);

    // 主题切换时单独补一次下拉文字色：这个 combo 是每张卡片各建一个，
    // 页面上同时活着十几二十个，页面那边统一扫太绕，各自听一次更省事。
    connect(eTheme, &ElaTheme::themeModeChanged, _categoryBox, [this](ElaThemeType::ThemeMode mode) {
        const bool dark = mode == ElaThemeType::ThemeMode::Dark;
        shared::applyComboBoxTheme(_categoryBox, dark);
        refreshThemeStyles(dark);
    });

    refreshThemeStyles(eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark);
}

void AppClassificationCard::setCategories(const QVector<data::CategoryToken>& categories)
{
    _categories = categories;

    _updating = true;
    _categoryBox->clear();
    for (const data::CategoryToken& token : _categories) {
        _categoryBox->addItem(token.label, token.id);
    }
    _updating = false;
}

void AppClassificationCard::setEntry(const data::AppClassificationEntry& entry,
                                     const data::AppClassificationPatch& patch)
{
    // 正在改名时别把输入框收起来：改别的字段（分类/颜色/开关）会走这条路径，
    // 一收就把用户敲了一半的名字吞掉了。
    // 用 isHidden() 而不是 isVisible()：卡片还没显示到屏幕上时 isVisible() 也是 false。
    const bool renaming = !_nameEdit->isHidden();

    _updating = true;

    _appId = entry.appId;
    _exeName = entry.exeName;
    _exePath = entry.exePath;
    _displayName = patch.displayName.trimmed();
    _categoryId = patch.categoryId.trimmed();
    _colorHex = patch.colorHex.trimmed();
    _trackEnabled = patch.trackEnabled;

    // 生效分类：有覆盖用覆盖，否则用内置规则。分类下拉要显示「生效的那一档」。
    const QString effectiveCategory = data::resolveCategoryId(_exeName, _categoryId);
    _effectiveColorHex = effectiveColorHex(effectiveCategory, _colorHex, _categories);

    const QString shownName = _displayName.isEmpty() ? displayNameFor(_exeName) : _displayName;
    _nameLabel->setText(shownName);
    _exeLabel->setText(_exeName);

    const int categoryIndex = _categoryBox->findData(effectiveCategory);
    _categoryBox->setCurrentIndex(categoryIndex >= 0 ? categoryIndex : 0);

    _trackSwitch->setIsToggled(_trackEnabled);

    if (!renaming) {
        _nameEdit->setVisible(false);
        _nameLabel->setVisible(true);
    }

    refreshColorButton();
    refreshIcon();
    refreshTrackBadge();

    _updating = false;
}

void AppClassificationCard::beginRename()
{
    if (_appId == 0) {
        return;
    }
    // 输入框里放「当前显示名」，不是 exe 名：用户想微调的话从看得见的名字改起更顺。
    _nameEdit->setText(_nameLabel->text());
    _nameLabel->setVisible(false);
    _nameEdit->setVisible(true);
    _nameEdit->setFocus();
    // 不能 selectAll：ElaAppBar 在 WM_LBUTTONDOWN 的原生处理里 post
    // "WMWindowClicked"，ElaLineEditPrivate 收到后对「有选中文字且有焦点」的
    // 输入框直接 clearFocus（库给搜索框「点外收起」设计的行为）——一点输入框
    // 就触发 FocusOut → 提交 → 输入框消失。
    // 光标放末尾，全选交给双击 / Ctrl+A。
    _nameEdit->setCursorPosition(_nameEdit->text().length());
}

void AppClassificationCard::commitRename()
{
    if (_nameEdit->isHidden()) {
        return;
    }
    _nameEdit->setVisible(false);
    _nameLabel->setVisible(true);

    const QString text = _nameEdit->text().trimmed();
    const QString defaultName = displayNameFor(_exeName);
    // 和「当前显示的名字」比，不是和覆盖项比：默认名的应用 _displayName 是空的，
    // 拿空串比会把「点一下铅笔再点别处」当成改成默认名，白记一条覆盖项。
    const QString current = _displayName.isEmpty() ? defaultName : _displayName;
    if (text == current) {
        return;
    }
    // 改回默认名时发空串，把覆盖项清掉，而不是存一条和默认值相同的记录。
    emit nameEdited(_appId, text == defaultName ? QString() : text);
}

void AppClassificationCard::cancelRename()
{
    _nameEdit->setVisible(false);
    _nameLabel->setVisible(true);
}

void AppClassificationCard::pickColor()
{
    // 按用户要求：点击色块直接弹 ElaColorDialog（不再经菜单中转），
    // 标题改成「自定义颜色」。库把标题写死为「编辑颜色」的 ElaText
    //（ElaText 继承 QLabel），构造后按文本找到它改文字——不改库源码。
    ElaColorDialog dialog(this);
    dialog.setCurrentColor(QColor(_effectiveColorHex));
    for (ElaText* text : dialog.findChildren<ElaText*>()) {
        if (text->text() == QStringLiteral("编辑颜色")) {
            text->setText(tr("自定义颜色"));
            break;
        }
    }

    // 靠 colorSelected 拿颜色，不靠 exec() 的返回值：
    // ElaColorDialog 点色即选，不一定会走 accept。
    QColor picked;
    connect(&dialog, &ElaColorDialog::colorSelected, this, [&picked](const QColor& color) {
        if (color.isValid()) {
            picked = color;
        }
    });
    const int dialogResult = dialog.exec();

    // 只是关掉对话框时不要拿初值当「用户选了这个色」：那会把「跟随分类色」
    // 悄悄改成同色的自定义色，还平白多出一条未保存的改动。
    if (!picked.isValid() && dialogResult == QDialog::Accepted) {
        picked = dialog.getCurrentColor();
    }
    if (!picked.isValid()) {
        return;
    }
    emit colorEdited(_appId, picked.name().toUpper());
}

void AppClassificationCard::refreshColorButton()
{
    // 色块用圆角方块画：QPushButton 的默认外框在深浅色下都不太合适。
    _colorButton->setStyleSheet(
        QStringLiteral("QPushButton { background:%1; border:1px solid rgba(128,128,128,0.45);"
                       " border-radius:%2px; }"
                       "QPushButton:hover { border-color:rgba(128,128,128,0.8); }")
            .arg(_effectiveColorHex)
            .arg(kColorButtonSize / 2 - 2));
}

void AppClassificationCard::refreshIcon()
{
    // 优先用 exe 真图标：分类管理页一眼扫过去靠图标认应用，首字母方块认不出来。
    // 带 dpr 取图，高缩放屏上才不会被拉糊。
    const QPixmap icon = shared::appIcon(_exePath, kIconSize, devicePixelRatioF());
    if (!icon.isNull()) {
        _iconLabel->setPixmap(icon);
        // 真图标自带形状，不要再加底色和圆角，否则会在四周留一圈色边。
        _iconLabel->setStyleSheet(QString());
        return;
    }

    _iconLabel->setPixmap(QPixmap());

    QColor background(_effectiveColorHex);
    background.setAlpha(38);

    _iconLabel->setStyleSheet(
        QStringLiteral("QLabel { background:rgba(%1,%2,%3,%4); border-radius:%5px;"
                       " color:%6; font-size:13px; font-weight:600; }")
            .arg(background.red())
            .arg(background.green())
            .arg(background.blue())
            .arg(background.alpha())
            .arg(kIconRadius)
            .arg(_effectiveColorHex));

    // 正在改名时用输入框里的字：否则每改一个别的字段，图标上的首字都会闪回旧名字。
    const QString typed = _nameEdit->text().trimmed();
    const QString name = (!_nameEdit->isHidden() && !typed.isEmpty()) ? typed : _nameLabel->text();
    _iconLabel->setText(name.isEmpty() ? QStringLiteral("?") : name.left(1).toUpper());
}

void AppClassificationCard::refreshTrackBadge()
{
    _excludedBadge->setVisible(!_trackEnabled);
}

void AppClassificationCard::refreshThemeStyles(bool dark)
{
    if (_excludedBadge != nullptr) {
        _excludedBadge->setStyleSheet(shared::dangerTextStyle(dark));
    }
    if (_exeLabel != nullptr) {
        _exeLabel->setStyleSheet(shared::secondaryTextStyle(dark));
    }
}

bool AppClassificationCard::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == _nameEdit) {
        if (event->type() == QEvent::KeyPress) {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->key() == Qt::Key_Escape) {
                cancelRename();
                return true;
            }
        } else if (event->type() == QEvent::FocusOut) {
            // 失焦提交（与回车同路径）。延迟一拍再查：库的 WMWindowClicked
            // 处理在失焦后还会把焦点塞回来（同库 clearFocus 的时序竞争），
            // 立即提交会把「库强制失焦但焦点实际还在」的输入框误收。
            // 输入框还显示着才提交：commitRename 自身会收起输入框，
            // Esc 路径已提前收起时不重复。
            if (!_nameEdit->isHidden()) {
                QTimer::singleShot(0, this, [this]() {
                    // 一拍之后仍没有焦点才真的提交。
                    if (!_nameEdit->isHidden() && !_nameEdit->hasFocus()) {
                        commitRename();
                    }
                });
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AppClassificationCard::contextMenuEvent(QContextMenuEvent* event)
{
    if (_appId == 0) {
        QWidget::contextMenuEvent(event);
        return;
    }
    // 宿主页在 quickMenuRequested 里弹 QuickClassificationMenu。
    emit quickMenuRequested(_appId, event->globalPos());
}

} // namespace patina::ui
