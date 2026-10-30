#ifndef PATINA_DATA_EXPORT_SERVICE_H
#define PATINA_DATA_EXPORT_SERVICE_H

#include "data/types.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace patina::data
{

class Database;

// 导出格式的可选字段（裁剪掉网页活动专属列——本项目没有网页活动）。
struct ExportFieldSpec
{
    const char* name;      // 字段标识（CSV/SQLite 表头、Markdown 表头 key）
    const char* label;     // 中文表头
};

// 全部可选字段，顺序即默认列顺序。
QVector<ExportFieldSpec> exportFieldSpecs();
QStringList defaultExportFields();
// 校验/排序用户选择的字段：未知字段返回错误，空选择返回错误，
// 重复字段去重并按用户顺序输出。
Result<QStringList> resolveExportFields(const QStringList& selected);

struct ExportReport
{
    qint64 sessionCount = 0;
    QString targetPath;
};

// 数据导出（CSV / Markdown / SQLite 三格式）。
//
// - 时间范围 [startMs, endMs)：与区间有交叠的会话都算（与各页读模型同口径）；
//   startMs 为 0 表示不限起点，endMs 为 0 表示不限终点（开放到当前）。
// - SQLite 导出 = 结构化单库：一张 sessions 快照表（按选定字段建列），
//   与「备份」（VACUUM INTO 整库快照）是两条路。
// - 大导出流式写：逐段读（分页游标），不整表进内存。
// - 写入走临时文件 + 换名，路径校验在 writeOutput 里。
class ExportService
{
public:
    explicit ExportService(Database& database) : _database(database) {}

    Result<ExportReport> exportTo(const QString& format, const QString& targetPath,
                                  qint64 startMs, qint64 endMs,
                                  const QStringList& selectedFields) const;

    // 默认导出文件名（不含目录）。format: "csv" | "markdown" | "sqlite"
    static QString defaultExportFileName(const QString& format, qint64 nowMs);

private:
    Database& _database;
};

} // namespace patina::data

#endif // PATINA_DATA_EXPORT_SERVICE_H
