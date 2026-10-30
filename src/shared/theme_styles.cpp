#include "shared/theme_styles.h"

#include <QColor>
#include <QComboBox>
#include <QMenu>

#include <algorithm>

namespace patina::shared
{
namespace
{

// 和 main.cpp 里 applyPaletteForTheme() 用的两档文字色一致。
// 两处都改的时候别忘了另一处：调色板管原生绘制，这里管样式表。
const char* const kDarkText = "#E9ECF1";
const char* const kLightText = "#1B1D22";
const char* const kDarkSecondary = "#9AA3B2";
const char* const kLightSecondary = "#8F98A8";
const char* const kDarkAccent = "#7C8AF0";
const char* const kLightAccent = "#4F5FD7";
const char* const kDarkPopup = "#23272E";
const char* const kLightPopup = "#FFFFFF";
// 菜单悬停块：跟下拉列表同一档底色做微调，深色下比弹层浅一格、浅色下比弹层深一格。
const char* const kDarkHover = "#333842";
const char* const kLightHover = "#EDEFF3";
// 菜单边框，比弹层底色深一点点即可，不要用纯黑描边。
const char* const kDarkBorder = "#3A404A";
const char* const kLightBorder = "#D2D7E0";
// 危险/成功两档。深色下要比浅色亮一截，否则压在深底上看不出是红是绿。
const char* const kDarkDanger = "#E08573";
const char* const kLightDanger = "#C07158";
const char* const kDarkSuccess = "#79C083";
const char* const kLightSuccess = "#5A9861";

} // namespace

QString primaryTextColor(bool dark)
{
    return QString::fromLatin1(dark ? kDarkText : kLightText);
}

QString secondaryTextColor(bool dark)
{
    return QString::fromLatin1(dark ? kDarkSecondary : kLightSecondary);
}

QString accentColor(bool dark)
{
    return QString::fromLatin1(dark ? kDarkAccent : kLightAccent);
}

QString accentTint(bool dark, int alpha)
{
    const QColor color(accentColor(dark));
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(std::clamp(alpha, 0, 255));
}

QString popupBackgroundColor(bool dark)
{
    return QString::fromLatin1(dark ? kDarkPopup : kLightPopup);
}

QString dangerColor(bool dark)
{
    return QString::fromLatin1(dark ? kDarkDanger : kLightDanger);
}

QString successColor(bool dark)
{
    return QString::fromLatin1(dark ? kDarkSuccess : kLightSuccess);
}

QString borderColor(bool dark)
{
    return QString::fromLatin1(dark ? kDarkBorder : kLightBorder);
}

QString hoverBackgroundColor(bool dark)
{
    return QString::fromLatin1(dark ? kDarkHover : kLightHover);
}

QString secondaryTextStyle(bool dark)
{
    return QStringLiteral("color:%1;").arg(secondaryTextColor(dark));
}

QString dangerTextStyle(bool dark)
{
    return QStringLiteral("color:%1;").arg(dangerColor(dark));
}

QString successTextStyle(bool dark)
{
    return QStringLiteral("color:%1;").arg(successColor(dark));
}

void applyComboBoxTheme(QComboBox* box, bool dark)
{
    if (box == nullptr) {
        return;
    }

    const QString text = primaryTextColor(dark);
    // 只写 color：边框、圆角、焦点圈都留给 Ela 自己画。
    // 弹出列表单独给一条，否则下拉展开后还是一列黑字。
    box->setStyleSheet(QStringLiteral("QComboBox { color:%1; }"
                                      " QComboBox QAbstractItemView { color:%1; background-color:%2; }")
                           .arg(text, popupBackgroundColor(dark)));
    // 样式表改了要强制重绘：combo 的当前项是 Ela 自己画的，
    // 不 update 会等到下一次交互才变色。
    box->update();
}

void applyComboBoxTheme(const QVector<QComboBox*>& boxes, bool dark)
{
    for (QComboBox* box : boxes) {
        applyComboBoxTheme(box, dark);
    }
}

void applyMenuTheme(QMenu* menu, bool dark)
{
    if (menu == nullptr) {
        return;
    }

    // 只写 menu 自己和 QMenu::item 两条规则：
    // item 的 padding 决定左右留白，图标靠 QAction::icon 出，不走 Ela 的字体绘制。
    //
    // 弹层本身不给 border-radius：QMenu 是原生弹出窗口、不做透明背景，
    // 圆角之外那四个角会露出窗口底色，反而比直角难看。
    menu->setStyleSheet(QStringLiteral("QMenu { background-color:%1; border:1px solid %2;"
                                       " padding:4px; }"
                                       " QMenu::item { color:%3; padding:6px 18px 6px 10px;"
                                       " border-radius:4px; }"
                                       " QMenu::item:selected { background-color:%4; color:%3; }"
                                       " QMenu::item:disabled { color:%5; }"
                                       " QMenu::separator { height:1px; background:%2;"
                                       " margin:4px 8px; }")
                            .arg(popupBackgroundColor(dark),
                                 borderColor(dark),
                                 primaryTextColor(dark),
                                 hoverBackgroundColor(dark),
                                 secondaryTextColor(dark)));
    menu->update();
}

} // namespace patina::shared
