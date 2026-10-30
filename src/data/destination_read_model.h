#ifndef PATINA_DATA_DESTINATION_READ_MODEL_H
#define PATINA_DATA_DESTINATION_READ_MODEL_H

#include "data/types.h"

#include <QDate>
#include <QString>
#include <QVector>

#include <optional>

namespace patina::data
{

class Database;

// 详情弹窗的目标：应用维度（exeName 定位）或分类维度（categoryId 定位）。
// 网页维度已裁剪，不存在。
struct DestinationTarget
{
    enum class Mode
    {
        App,
        Category,
    };

    Mode mode = Mode::App;
    // App：exe 名（大小写不敏感）；Category：分类 id。
    QString key;
};

// 明细弹窗里的一行记录：一个标题样本持续的时段
// （无标题的空档也成行，title 为空串）。
struct DestinationRecord
{
    qint64 startTimeMs = 0;
    qint64 endTimeMs = 0;
    qint64 durationMs = 0;
    QString title;          // 空串 = 无标题时段（gap 行）
    QString exeName;        // 分类维度下用来显示来源应用
    bool isLive = false;    // 进行中的段（右端实时跳动）
};

struct DestinationDetailDay
{
    QString dateKey;        // yyyy-MM-dd
    qint64 dayStartMs = 0;
    qint64 dayEndMs = 0;    // 已按 now 封底
    qint64 totalDurationMs = 0;
    QVector<DestinationRecord> records;  // 按开始时间升序，同应用首尾相接的行已合并
};

// 详情弹窗读模型（应用维度直接查，分类维度 = 先解析出分类成员 exe
// 集合再走同一套查询）。
//
// 口径：段与 [dayStart, min(dayEnd, now)] 的交叠；标题样本从
// session_title_samples 联查，样本之间的空档成「无标题」行。
// 同一应用首尾相接（gap 为 0）的行合并，跨段续看同一文档时明细不打碎；
// 时长只累加真实时段，不含任何空隙，因此 totalDurationMs 与首页同口径。
class DestinationReadModel
{
public:
    explicit DestinationReadModel(Database& database) : _database(database) {}

    // minDurationMs 过滤只作用于明细行（时间轴仍画全部段）。
    Result<DestinationDetailDay> loadDay(const DestinationTarget& target,
                                         QDate date,
                                         qint64 minDurationMs) const;

    // 分类维度入口要用的成员集合：该分类下有记录的应用（按当日时长降序）。
    Result<QStringList> categoryMembers(const QString& categoryId, QDate date) const;

private:
    Database& _database;
};

} // namespace patina::data

#endif // PATINA_DATA_DESTINATION_READ_MODEL_H
