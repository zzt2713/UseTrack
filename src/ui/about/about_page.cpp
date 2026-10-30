#include "ui/about/about_page.h"

#include <QCoreApplication>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QVBoxLayout>
#include <QApplication>
#include <QScreen>

#include "ElaScrollPageArea.h"
#include "ElaText.h"
#include "ElaTheme.h"

#include "shared/theme_styles.h"

namespace
{

constexpr int kPanelGap = 14;
constexpr int kCardRadius = 14;
constexpr int kIconSide = 64;

// 标签语义角色：与设置页同一套约定（secondary | primary），切主题时整页重刷。
constexpr char kLabelRoleProperty[] = "patinaRole";

bool currentThemeDark()
{
    return eTheme->getThemeMode() == ElaThemeType::ThemeMode::Dark;
}

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
        label->setStyleSheet(patina::shared::secondaryTextStyle(dark));
    } else if (role == QLatin1String("primary")) {
        label->setStyleSheet(
            QStringLiteral("color:%1;").arg(patina::shared::primaryTextColor(dark)));
    } else if (role == QLatin1String("badge")) {
        // 许可类型徽标：描边小胶囊，颜色同样跟主题走。
        label->setStyleSheet(QStringLiteral("color:%1; border:1px solid %2;"
                                            " border-radius:6px; padding:1px 7px;")
                                 .arg(patina::shared::secondaryTextColor(dark),
                                      patina::shared::borderColor(dark)));
    } else if (role == QLatin1String("version")) {
        // 版本胶囊：强调色文字 + 半透明强调底。
        label->setStyleSheet(QStringLiteral("color:%1; border-radius:999px; padding:1px 8px;"
                                            " background:%2;")
                                 .arg(patina::shared::accentColor(dark),
                                      patina::shared::accentTint(dark, 40)));
    }
}

void setLabelRole(QLabel* label, const char* role)
{
    if (label != nullptr) {
        label->setProperty(kLabelRoleProperty, QString::fromLatin1(role));
        applyLabelColor(label, currentThemeDark());
    }
}

// ElaScrollPageArea 构造里写死 setFixedHeight(75)，当容器用必须先解开。
ElaScrollPageArea* makeCard(QWidget* parent)
{
    auto* card = new ElaScrollPageArea(parent);
    card->setMinimumHeight(0);
    card->setMaximumHeight(QWIDGETSIZE_MAX);
    card->setBorderRadius(kCardRadius);
    return card;
}

ElaText* makeTitle(const QString& text, int pixelSize, QWidget* parent)
{
    auto* label = new ElaText(text, pixelSize, parent);
    label->setTextStyle(ElaTextType::BodyStrong);
    // setTextStyle 会改字号，必须在它之后再设一次像素大小。
    label->setTextPixelSize(pixelSize);
    return label;
}

// 应用图标：资源里的 app.png 缩到 64px，圆角靠预先画好的一张底图。
// 不直接用 QLabel::setPixmap 铺满，是因为 64px 的方图直接放上去边角是硬的。

QPixmap makeAppIcon()
{
    const int side = kIconSide; // 64

    qreal dpr = 1.0;
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        dpr = screen->devicePixelRatio();
    }

    const QPixmap source(QStringLiteral(":/icons/app.png"));
    if (source.isNull()) {
        return QPixmap();
    }
    int targetPhysicalSize = side * dpr;
    QPixmap scaled = source.scaled(targetPhysicalSize, targetPhysicalSize,
                                   Qt::KeepAspectRatioByExpanding, // 保持比例铺满
                                   Qt::SmoothTransformation);      // 高质量平滑插值

    // 4. 创建物理大小的画布
    QPixmap canvas(targetPhysicalSize, targetPhysicalSize);
    canvas.setDevicePixelRatio(dpr); // 告诉 Qt 这是一张高清图
    canvas.fill(Qt::transparent);

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 5. 圆角裁剪（逻辑坐标 side）
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, side, side), 16, 16);
    painter.setClipPath(clip);

    // 6. 计算居中绘制位置（因为用了 KeepAspectRatioByExpanding，长宽比不同时要居中裁切）
    // 注意这里的 QRectF 是物理尺寸的，因为我们在原图图层上画
    QRectF targetRect(0, 0, side, side);

    // 使用 SmoothPixmapTransform 再次确保安全
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // 绘制缩放后的高清图
    painter.drawPixmap(targetRect, scaled, QRectF(scaled.rect()));

    painter.end();
    return canvas;
}
// 一行「键 : 值」。
QWidget* makeInfoRow(const QString& key, const QString& value, QWidget* parent)
{
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 6, 0, 6);
    layout->setSpacing(16);

    auto* keyLabel = new QLabel(key, row);
    QFont keyFont = keyLabel->font();
    keyFont.setPixelSize(13);
    keyLabel->setFont(keyFont);
    setLabelRole(keyLabel, "primary");
    layout->addWidget(keyLabel, 1, Qt::AlignLeft | Qt::AlignVCenter);

    auto* valueLabel = new QLabel(value, row);
    QFont valueFont = valueLabel->font();
    valueFont.setPixelSize(12);
    valueLabel->setFont(valueFont);
    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    setLabelRole(valueLabel, "secondary");
    layout->addWidget(valueLabel, 0, Qt::AlignRight | Qt::AlignVCenter);
    return row;
}

// 许可列表的一行：左名 + 用途，右许可类型徽标。
QWidget* makeLicenseRow(const QString& name, const QString& use, const QString& license,
                        QWidget* parent)
{
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 8, 0, 8);
    layout->setSpacing(16);

    auto* left = new QWidget(row);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(2);
    leftLayout->addWidget(makeTitle(name, 13, left));

    auto* useLabel = new QLabel(use, left);
    QFont useFont = useLabel->font();
    useFont.setPixelSize(11);
    useLabel->setFont(useFont);
    setLabelRole(useLabel, "secondary");
    leftLayout->addWidget(useLabel);
    layout->addWidget(left, 1);

    auto* badge = new QLabel(license, row);
    QFont badgeFont = badge->font();
    badgeFont.setPixelSize(10);
    badgeFont.setWeight(QFont::DemiBold);
    badge->setFont(badgeFont);
    badge->setAlignment(Qt::AlignCenter);
    setLabelRole(badge, "badge");
    layout->addWidget(badge, 0, Qt::AlignRight | Qt::AlignVCenter);
    return row;
}

} // namespace

namespace patina::ui
{

AboutPage::AboutPage(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 16, 24, 24);
    layout->setSpacing(kPanelGap);

    auto* header = new QWidget(this);
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(2);
    headerLayout->addWidget(makeTitle(tr("关于"), 19, header));
    headerLayout->addWidget(makeHintLabel(
        tr("版本、简介与第三方许可，全部内容离线内置"), header));
    layout->addWidget(header);

    layout->addWidget(buildBrandCard());
    layout->addWidget(buildLicenseCard());
    layout->addStretch();

    connect(eTheme, &ElaTheme::themeModeChanged, this, [this](ElaThemeType::ThemeMode mode) {
        applyThemeColors(mode == ElaThemeType::ThemeMode::Dark);
    });
}

AboutPage::~AboutPage() = default;

void AboutPage::setActive(bool active)
{
    if (_iconLabel == nullptr) {
        return;
    }
    // 品牌图标是构造时合成的位图，是本页唯一跟显隐有关、可以放掉的资源。
    // 切走就清掉，切回来再画一张——重画只是缩放一次 app.png，代价可忽略。
    if (active) {
        _iconLabel->setPixmap(makeAppIcon());
    } else {
        _iconLabel->clear();
    }
}

QLabel* AboutPage::makeHintLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    QFont font = label->font();
    font.setPixelSize(11);
    label->setFont(font);
    label->setWordWrap(true);
    setLabelRole(label, "secondary");
    return label;
}

QWidget* AboutPage::buildBrandCard()
{
    auto* card = makeCard(this);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);

    auto* brand = new QWidget(card);
    auto* brandLayout = new QHBoxLayout(brand);
    brandLayout->setContentsMargins(0, 0, 0, 0);
    brandLayout->setSpacing(16);

    auto* icon = new QLabel(brand);
    icon->setPixmap(makeAppIcon());
    icon->setFixedSize(kIconSide, kIconSide);
    // 非活动态要清空位图（setActive），所以得留引用。
    _iconLabel = icon;
    brandLayout->addWidget(icon, 0, Qt::AlignTop);

    auto* meta = new QWidget(brand);
    auto* metaLayout = new QVBoxLayout(meta);
    metaLayout->setContentsMargins(0, 2, 0, 0);
    metaLayout->setSpacing(4);
    metaLayout->addWidget(makeTitle(tr("UseTrack"), 20, meta));

    // 版本号来自 CMake 里写进目标文件的 VERSION；读不到就给「版本未知」。
    const QString version = QCoreApplication::applicationVersion().trimmed();
    auto* versionLabel = new QLabel(
        version.isEmpty() ? tr("版本未知") : tr("v%1").arg(version), meta);
    QFont versionFont = versionLabel->font();
    versionFont.setPixelSize(11);
    versionFont.setWeight(QFont::DemiBold);
    versionLabel->setFont(versionFont);
    setLabelRole(versionLabel, "version");
    metaLayout->addWidget(versionLabel, 0, Qt::AlignLeft);

    metaLayout->addWidget(makeHintLabel(tr("本地优先的个人桌面时间追踪工具"), meta));
    brandLayout->addWidget(meta, 1);
    layout->addWidget(brand);

    layout->addWidget(makeInfoRow(tr("运行方式"), tr("完全离线 · 不联网 · 不上传"), card));
    layout->addWidget(makeInfoRow(tr("构建环境"), tr("C++17 · Qt 6.8.3 · ElaWidgetTools"), card));
    layout->addWidget(makeInfoRow(tr("追踪引擎"), tr("本机前台窗口采样，到点封口后入库"), card));
    layout->addWidget(makeHintLabel(
        tr("版本号取自构建时的应用版本；无法读取时显示「版本未知」。"), card));
    return card;
}

QWidget* AboutPage::buildLicenseCard()
{
    auto* card = makeCard(this);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(6);

    auto* head = new QWidget(card);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 4);
    headLayout->setSpacing(8);
    headLayout->addWidget(makeTitle(tr("开源许可"), 14, head));
    auto* tag = new QLabel(tr("第三方组件"), head);
    QFont tagFont = tag->font();
    tagFont.setPixelSize(10);
    tag->setFont(tagFont);
    setLabelRole(tag, "secondary");
    headLayout->addWidget(tag, 0, Qt::AlignVCenter);
    headLayout->addStretch();
    layout->addWidget(head);

    layout->addWidget(makeLicenseRow(tr("Qt 6.8.3"),
                                     tr("界面框架（Core / Gui / Widgets / Sql）"),
                                     tr("LGPL-3.0"), card));
    layout->addWidget(makeLicenseRow(tr("ElaWidgetTools 2.0.0"), tr("窗口、导航与控件外观"),
                                     tr("MIT"), card));
    layout->addWidget(makeLicenseRow(tr("SQLite"), tr("本地数据存储（经 Qt Sql 驱动）"),
                                     tr("Public Domain"), card));

    layout->addWidget(makeHintLabel(
        tr("以上组件的完整许可文本随源码分发，可在各自的发行包中查阅。"), card));
    return card;
}

void AboutPage::applyThemeColors(bool dark)
{
    const QList<QLabel*> labels = findChildren<QLabel*>();
    for (QLabel* label : labels) {
        applyLabelColor(label, dark);
    }
}

} // namespace patina::ui
