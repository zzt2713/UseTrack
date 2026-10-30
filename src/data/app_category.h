#ifndef PATINA_DATA_APP_CATEGORY_H
#define PATINA_DATA_APP_CATEGORY_H

#include <QString>
#include <QVector>

namespace patina::data
{

// 一个分类的展示三元组。
// 只带颜色的十六进制字符串，不带 QColor：data/ 不依赖 QtGui。
struct CategoryToken
{
    QString id;      // 稳定标识，写进快照与测试，不随界面文案变
    QString label;   // 中文展示名
    QString colorHex;
};

// 内置分类表，按固定顺序返回（other 永远在最后，它是兜底）。
QVector<CategoryToken> categoryTokens();

// 取分类展示信息；未知 id 回落到 other。
CategoryToken categoryToken(const QString& categoryId);

// exe 名 → 分类 id。
//
// 内置规则表，不落库：apps.category_id 为空时按 exe 名实时归并。
// 匹配沿用 apps.exe_name 的语义——大小写不敏感（trim + 小写后查表）。
// 表里没有的进程一律归 other，不做启发式猜测。
QString classifyExecutable(const QString& exeName);

// 生效分类 id：用户覆盖（apps.category_id）优先，为空时回落到内置规则。
// 读模型与分类管理页都必须走这里，否则「改完分类统计没变」会悄悄出现。
QString resolveCategoryId(const QString& exeName, const QString& categoryIdOverride);

// 颜色是否是合法的 #RRGGBB（大写不强制，写入前由调用方归一）。
bool isValidColorHex(const QString& colorHex);

} // namespace patina::data

#endif // PATINA_DATA_APP_CATEGORY_H
