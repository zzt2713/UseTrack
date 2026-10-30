#ifndef PATINA_DATA_IMPORT_SERVICE_H
#define PATINA_DATA_IMPORT_SERVICE_H

#include "data/types.h"

#include <QVector>

#include <optional>

namespace patina::data
{

class Database;

// 导入预览。
//
// 两类来源：
//   - "patina-db"：旧版程序的 .db（sessions 表反规范化，app_name/exe_name
//     在行上）。
//   - "patina-csv"：canonical CSV（record_type/start_time/end_time/duration_ms/
//     exe_name/app_name/title/category 八列）。
struct ImportPreview
{
    QString filePath;
    QString sourceKind;          // "patina-db" | "patina-csv"
    qint64 validRecords = 0;     // 可导入的记录数（exact + bucket）
    qint64 exactSessions = 0;    // 精确段（导入后进 sessions）
    qint64 hourBuckets = 0;      // 小时桶（导入后留 import_time_buckets）
    qint64 errorRecords = 0;     // 解析失败/跳过的行数
    qint64 minStartTimeMs = 0;   // 时间范围（0 表示无记录）
    qint64 maxEndTimeMs = 0;
    QStringList warnings;        // 前 12 条（MAX_PREVIEW_ERRORS）
    QString fingerprint;         // 文件指纹（预览-提交一致性校验）
};

struct ImportCommitReport
{
    QString batchId;             // 空表示没有新数据可导入（全部重复）
    qint64 importedRecords = 0;
    qint64 duplicateRecords = 0;
    qint64 errorRecords = 0;
    qint64 exactSessions = 0;
    qint64 hourBuckets = 0;
};

struct ImportBatchSummary
{
    QString id;
    qint64 importedAtMs = 0;
    QString sourceName;
    QString sourceKind;
    qint64 exactSessions = 0;
    qint64 hourBuckets = 0;
};

struct ImportDeleteReport
{
    qint64 deletedExactSessions = 0;
    qint64 deletedHourBuckets = 0;
};

// 外部数据单向导入（预览 → 提交 → 批次回滚）。
//
// schema 映射（源库是反规范化，本项目是规范化）：
//   源 sessions(app_name, exe_name, ...) → C++ apps 行 + sessions(app_id)。
//   导入时按 exe_name（NOCASE）建/匹配 apps；app_name 只用来填显示名（空时不覆盖）。
// 标题样本与分类设置：
//   源库若带 session_title_samples 则一并迁移（挂到导入后的段 id 上）；
//   分类相关的 __app_override:: 设置映射到 apps.category_id / track_enabled 覆盖项，
//   已有覆盖项不覆盖。
//
// 幂等与回滚：行级指纹（sha1 语义等价：exe|start|end|duration 拼接的 hash）
// 去重，重复提交返回 0 新增；批次表级联删段可整体回滚。
class ImportService
{
public:
    explicit ImportService(Database& database) : _database(database) {}

    // 预览。大库流式扫描（分页读源库），不整库进内存（峰值窗口开销
    // 受每页行数约束）。
    Result<ImportPreview> preview(const QString& filePath) const;

    // 提交。expectedFingerprint 必须与 preview 的一致，否则拒绝
    // （文件变了要重新预览）。
    Result<ImportCommitReport> commit(const QString& filePath,
                                      const QString& expectedFingerprint);

    Result<QVector<ImportBatchSummary>> listBatches() const;

    // 按批次整体删除（回滚）：删段 + 删桶 + 删批次行；只被导入数据引用的
    // apps 行一并清理。
    Result<ImportDeleteReport> deleteBatch(const QString& batchId);

private:
    Database& _database;
};

} // namespace patina::data

#endif // PATINA_DATA_IMPORT_SERVICE_H
