#ifndef PATINA_UI_ABOUT_ABOUT_PAGE_H
#define PATINA_UI_ABOUT_ABOUT_PAGE_H

#include <QWidget>

class QLabel;

namespace patina::ui
{

// 关于页：版本号、简介、第三方开源许可列表。
//
// 裁掉了四个外链按钮
// （GitHub / 发行说明 / 反馈 / 赞助）、UpdateStatusPanel 与两个反馈对话框——
// 它们全部联网，已硬性裁剪。这里只展示本地可知的信息，页面不发
// 任何网络请求。
class AboutPage : public QWidget
{
    Q_OBJECT

public:
    explicit AboutPage(QWidget* parent = nullptr);
    ~AboutPage() override;

    // 切走时放掉品牌区那张预先合成的 64px 图标位图，切回来再画一张。
    // 页面里只有它是随显隐可回收的资源，其余都是文字与布局。
    void setActive(bool active);

private:
    QWidget* buildBrandCard();
    QWidget* buildLicenseCard();

    // 说明文字只能靠样式表上色，切主题时统一重刷（与设置页同一套做法）。
    void applyThemeColors(bool dark);
    QLabel* makeHintLabel(const QString& text, QWidget* parent);

    QLabel* _iconLabel = nullptr;
};

} // namespace patina::ui

#endif // PATINA_UI_ABOUT_ABOUT_PAGE_H
