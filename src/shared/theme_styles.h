#ifndef PATINA_SHARED_THEME_STYLES_H
#define PATINA_SHARED_THEME_STYLES_H

#include <QString>
#include <QVector>

class QComboBox;
class QMenu;

namespace patina::shared
{

// 让 ElaComboBox 的文字色跟着明暗主题走。
//
// 为什么需要它：ElaComboBox 的边框、圆角、弹出层都是 Ela 自己画的，
// 但它没有暴露「文字色」接口，于是深色主题下会拿 Qt 默认的黑色文字
// 画在深色底上，看不清。这里只补一条 `color` 规则，
// 不碰边框和圆角，避免把 Ela 自己的绘制顶掉。
//
// 弹出列表要单独给（`QComboBox QAbstractItemView`）：它弹出时是顶层窗口，
// 继承不到 QComboBox 自身的规则。
void applyComboBoxTheme(QComboBox* box, bool dark);

// 一次处理一组，页面里 combo 多的时候省事；空指针会被跳过。
void applyComboBoxTheme(const QVector<QComboBox*>& boxes, bool dark);

// 正文文字色（与 main.cpp 里那份 QPalette 的两档取值保持一致），
// 给需要自己写样式表又不想硬编码颜色的地方用。
QString primaryTextColor(bool dark);

// 次要文字色（说明、时间戳、计数那一档）。
QString secondaryTextColor(bool dark);

// 强调色（链接、可点文字、进度条）。
QString accentColor(bool dark);

// 强调色的半透明版本（列表行的 hover 底、选中底），返回 "rgba(r,g,b,a)"。
// alpha 传 0–255。原来这类底色是写死的 rgba(79,95,215,0.14)，深色主题下偏暗。
QString accentTint(bool dark, int alpha);

// 弹出层底色（dropdown 列表的背景）。
QString popupBackgroundColor(bool dark);

// 危险色（删除、错误、会被覆盖的提示）。
QString dangerColor(bool dark);

// 成功色（校验通过、导入成功）。
QString successColor(bool dark);

// 描边色（输入框、虚线拖放框）。
QString borderColor(bool dark);

// 悬停底色（列表行 hover、菜单项 hover）。
QString hoverBackgroundColor(bool dark);

// 下面四个是直接能塞给 setStyleSheet 的完整片段。
// 页面里那些说明文字、时间戳、错误行都是 QLabel/ElaText，只能靠样式表上色；
// 把色值拼装收敛在这里，页面就不用再写死十六进制。
QString secondaryTextStyle(bool dark);
QString dangerTextStyle(bool dark);
QString successTextStyle(bool dark);

// 让 QMenu 跟着明暗主题走：底色、圆角、文字色、hover 块全部自己写样式表。
//
// 为什么不直接用 ElaMenu：ElaMenu 的 showEvent 会在布局完成前 grab 一次自身
// 再按当前 rect 拉伸重绘做展开动画，抓到的尺寸和实际尺寸对不上时文字就会错位
// 叠影；它的 hover 底色又来自 ElaMenuStyle 构造时缓存的主题模式，切到深色后
// 不会刷新，于是 hover 出来是一块近白色。这两条都在 ElaWidgetTools 内部，
// 没有开关可以关掉，只能在这个菜单上换成 Qt 原生 QMenu。
void applyMenuTheme(QMenu* menu, bool dark);

} // namespace patina::shared

#endif // PATINA_SHARED_THEME_STYLES_H
