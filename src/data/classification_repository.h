#ifndef PATINA_DATA_CLASSIFICATION_REPOSITORY_H
#define PATINA_DATA_CLASSIFICATION_REPOSITORY_H

#include "data/category_registry.h"
#include "data/types.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace patina::data
{

class Database;

// 分类管理页看到的「一个应用 + 它的覆盖项」。
//
// 覆盖项为空表示走默认：
//   displayName 为空  → 界面显示 exe 名
//   categoryId  为空  → 内置规则表（classifyExecutable）
//   colorHex    为空  → 跟随所属分类的颜色
struct AppClassificationEntry
{
    qint64 appId = 0;
    QString exeName;
    QString exePath;
    QString displayName;
    QString categoryId;
    QString colorHex;
    bool trackEnabled = true;
    // 「不在列表显示」：见 types.h 里 App::hidden 的说明。
    bool hidden = false;
    qint64 firstSeenMs = 0;
    qint64 lastSeenMs = 0;
};

// 一次提交里对单个应用的完整覆盖项。
//
// 不是「增量」而是「整份状态」：页面点保存时把它当前看到的四列原样交回来，
// 空串/默认值即表示恢复默认。这样不需要额外表达「清空某一列」。
struct AppClassificationPatch
{
    qint64 appId = 0;
    QString displayName;
    QString categoryId;
    QString colorHex;
    bool trackEnabled = true;
    // 「不在列表显示」：与 trackEnabled 一起走整份状态提交。
    bool hidden = false;
};

// 分类管理页的写接口（ui/ 不写 SQL，写操作一律走这里）。
//
// 分类管理页是「攒改动 → 点保存」的交互，一次保存可能同时改好几个应用，
// 所以写入入口只有批量的一条：整批包在一个 Transaction 里，任何一条失败
// 全部回滚，读模型不会看到改了一半的状态。
class ClassificationRepository
{
public:
    explicit ClassificationRepository(Database& database) : _database(database) {}

    // 列表按 last_seen_ms 降序（最近用过的在前），保证界面顺序稳定。
    Result<QVector<AppClassificationEntry>> listApps() const;

    // 批量提交。空 patches 直接成功，不开事务。
    // 校验：appId 必须存在；categoryId 必须是当前生效分类（内置未删或已登记
    // 自定义）或空；colorHex 必须为空或 #RRGGBB（写入前统一转大写）。
    Status applyPatches(const QVector<AppClassificationPatch>& patches);

    // ---------------- 分类本身的增删改名 / 配色 ----------------
    // 全部落 settings 键空间（键定义在 category_registry.h），不新造表。
    //
    // 校验是「整批先校验再进事务」：非法输入不留任何痕迹。

    // 新增自定义分类。生成 custom:category_<uuid-hex> id，登记
    // __custom_category::<id>，标签写 __category_label_override::<id>，
    // 并追加到顺序键尾。返回新分类 id。
    Result<QString> createCategory(const QString& label);

    // 删除分类。
    //   - 自定义分类：清登记/标签/颜色/顺序键（真删）；
    //   - 内置分类（other 除外）：打 __deleted_category::<id> 墓碑（退出界面，
    //     历史数据里的引用仍可识别）；
    //   - other 不可删，直接失败。
    // 两种情况都在同一事务里把成员应用的 apps.category_id 覆盖清空
    // （回退到内置规则 / other），统计时长不受影响。
    Status deleteCategory(const QString& categoryId);

    // 改名。内置与自定义通用：写 __category_label_override::<id>；
    // 名字与内置默认相同时清键（恢复默认显示）。
    Status renameCategory(const QString& categoryId, const QString& label);

    // 分类配色。写 __category_color_override::<id>；colorHex 传空 = 重置
    // （清键回默认色）。批量应用到成员应用颜色由 UI 层拿分类色去算
    // （成员颜色为空本来就跟随分类，只有显式自定义过颜色的成员才需要跟着改，
    // 那是 apps.color_hex 的整份 patch，走 applyPatches）。
    Status setCategoryColor(const QString& categoryId, const QString& colorHex);

    // 持久化自定义分类顺序（完整逗号列表覆盖写）。
    Status setCustomCategoryOrder(const QStringList& orderedIds);

    // 重置全部分类配色与应用配色（危险操作，调用方必须先二次确认）：
    //   - 清掉所有 __category_color_override::<id> 键（分类回默认色）；
    //   - 清掉所有 apps.color_hex 自定义色（应用回「跟随分类色」）。
    // 不动分类定义、归属、改名键。整批一个事务，任何一步失败全回滚。
    Status resetAllColors();

private:
    Database& _database;
};

} // namespace patina::data

#endif // PATINA_DATA_CLASSIFICATION_REPOSITORY_H
