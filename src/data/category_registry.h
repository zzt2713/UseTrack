#ifndef PATINA_DATA_CATEGORY_REGISTRY_H
#define PATINA_DATA_CATEGORY_REGISTRY_H

// 分类定义的运行时注册表。
//
// 分类不再只有 app_category.cpp 的 11 项内置表，用户可以新增/删除/改名/配色。
// 持久化走 settings 键空间：
//
//   __category_label_override::<id>   分类显示名（内置与自定义通用）
//   __category_color_override::<id>   分类颜色覆盖（#RRGGGB 大写）
//   __custom_category::<id>           自定义分类登记（值 "1"；id 形如 custom:category_<hex>）
//   __deleted_category::<id>          内置分类的「已删除」墓碑（自定义分类删除时直接清键）
//   __category_order::<scope>         自定义分类顺序（逗号分隔 id 列表，scope 固定 custom）
//
// 内置分类永远存在；「删除」内置分类只是打墓碑——它退出界面，但历史数据里
// 引用它的 id 仍能被 categoryToken() 识别并落回兜底显示，统计不丢时长。
// 删除自定义分类是真删：登记键直接清掉（apps.category_id 里的悬空引用
// 读取时统一按 other 兜底）。

#include "data/app_category.h"
#include "data/types.h"

#include <QVector>

namespace patina::data
{

class Database;

// 一个可见分类的三元组 + 用户覆盖状态。
struct CategoryEntry
{
    QString id;        // 内置 id（如 "development"）或 custom:category_<hex>
    QString label;     // 生效显示名（覆盖优先）
    QString colorHex;  // 生效颜色（覆盖优先）
    bool builtin = false;      // 是否内置分类（内置不可删）
    bool deletable = false;    // 可否删除（other 与内置都不可删；自定义可删）
    bool deletableBuiltin = false; // 内置但可打墓碑（other 除外）
};

// 设置键前缀。写操作走 ClassificationRepository；这里公开给它用。
QString categoryLabelOverrideKey(const QString& categoryId);
QString categoryColorOverrideKey(const QString& categoryId);
QString customCategoryKey(const QString& categoryId);
QString deletedCategoryKey(const QString& categoryId);
QString categoryOrderKey();

// 从 settings 键空间装载生效分类列表（内置 - 墓碑 + 自定义，按展示顺序）。
// 数据库未打开或读失败返回错误；键缺失时就是纯内置表。
Result<QVector<CategoryEntry>> loadCategoryRegistry(Database& database);

// 单个分类的生效显示名（找不到该分类时回退内置表，再回退 other）。
// 给读模型做轻量取用：不需要整个列表时省一次全量装载。
Result<QString> loadCategoryLabel(Database& database, const QString& categoryId);

// 单个分类的生效颜色。回退链与 label 相同。
Result<QString> loadCategoryColor(Database& database, const QString& categoryId);

// ---------------- 快照式解析（读模型用） ----------------
//
// 读模型在一次查询里会对几千个段落反复归分类，逐条走 settings 查询不可接受；
// 正确姿势是查询前装载一次注册表快照，循环里查内存。
struct CategoryRegistrySnapshot
{
    // 生效分类（内置 - 墓碑 + 自定义），展示顺序与 loadCategoryRegistry 一致。
    QVector<CategoryEntry> entries;

    // id → label/color 查找；未命中按内置表回落（墓碑/悬空 id → other 的显示值）。
    QString labelFor(const QString& categoryId) const;
    QString colorFor(const QString& categoryId) const;
    // 生效 id 解析：悬空 id（自定义被真删 / 内置被墓碑）落到 other。
    // 可见 id 原样返回。
    QString resolveId(const QString& categoryId) const;
};

// 装载快照。数据库未打开时返回带 entries 为空的失败结果。
Result<CategoryRegistrySnapshot> loadCategoryRegistrySnapshot(Database& database);

} // namespace patina::data

#endif // PATINA_DATA_CATEGORY_REGISTRY_H
