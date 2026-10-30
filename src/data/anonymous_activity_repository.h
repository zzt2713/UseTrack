#ifndef PATINA_DATA_ANONYMOUS_ACTIVITY_REPOSITORY_H
#define PATINA_DATA_ANONYMOUS_ACTIVITY_REPOSITORY_H

#include "data/types.h"

namespace patina::data
{

class Database;

// 匿名活动段：只记时间，不记应用、不记标题。
//
// 用户给某个应用开了匿名统计后，那段前台时间不落 sessions，落在这里。
// 表里没有 exe / title 列——不是「写进去再藏起来」，而是根本没有这个字段。
//
// 约束：
//   - 同一时刻最多一个未闭合段（部分唯一索引保证）；
//   - observe() 只在时间往前走时延展 observed_until_ms，时钟回拨不写库；
//   - seal() 幂等，重复封口不会把已经有终点的段改掉。
class AnonymousActivityRepository
{
public:
    explicit AnonymousActivityRepository(Database& database) : _database(database) {}

    // 采样一次。返回是否产生了新的段（供上层决定要不要通知读模型）。
    Result<bool> observe(qint64 nowMs);

    // 把当前未闭合段封在 cutoffMs。幂等：已经有终点的段不会被改。
    Status seal(qint64 cutoffMs);

    // 异常退出后启动时用。段实际结束在「最后一次观察到的时刻」，
    // 只有它落在合法区间内才采信，否则退回起点。
    // 和 seal() 的区别：seal() 是「主动封在某个时刻」，这个是「推断上次断在哪」。
    Status sealInterrupted(qint64 cutoffMs);

    // 已闭合匿名段在区间内的累计时长。
    Result<qint64> totalClosedDurationMs(qint64 fromMs, qint64 toMs) const;

private:
    Database& _database;
};

} // namespace patina::data

#endif // PATINA_DATA_ANONYMOUS_ACTIVITY_REPOSITORY_H
