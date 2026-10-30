#ifndef PATINA_DATA_SESSION_REPOSITORY_H
#define PATINA_DATA_SESSION_REPOSITORY_H

#include "data/types.h"

#include <QVector>

#include <optional>

namespace patina::data
{

class Database;

// sessions 表的读写。
//
// 不变式：任何时刻最多一个未闭合段（由 idx_sessions_single_open 部分唯一索引保证）。
// 所以 openSegment() 在上一个段还开着时会失败——调用方必须先 closeOpen()。
// 这是故意的：非法时序应该立刻暴露，而不是被库悄悄修好。
class SessionRepository
{
public:
    explicit SessionRepository(Database& database) : _database(database) {}

    // 开一个新段。要求当前没有未闭合段，否则返回失败。
    // continuityGroupStartTimeMs 传 0 表示「本段就是新组的起点」；非 0 时按
    // 「组起点不得晚于段起点」夹一次（短时切走再切回会复用原组起点）。
    Result<SessionSegment> openSegment(qint64 appId,
                                       const QString& windowTitle,
                                       qint64 startTimeMs,
                                       qint64 continuityGroupStartTimeMs = 0);

    // 把当前未闭合段闭合在 endTimeMs。没有未闭合段时返回 false 且不算错误
    // （engine 的“没有活跃段”路径会频繁走到这里）。
    Status closeOpen(qint64 endTimeMs);

    // 更新未闭合段的窗口标题。没有未闭合段时返回 0 行受影响，不算错误。
    Status updateOpenTitle(const QString& windowTitle);

    Result<std::optional<SessionSegment>> findOpen() const;
    Result<std::optional<SessionSegment>> findById(qint64 id) const;

    // 按 start_time_ms 升序取一页，不一次读全表。
    Result<QVector<SessionSegment>> listRange(qint64 fromMs, qint64 toMs, int limit) const;

    // 已闭合段的累计时长。未闭合段不计入——它的时长还在变，算进来会让
    // 定时刷新的 UI 数字来回跳。
    Result<qint64> totalClosedDurationMs(qint64 fromMs, qint64 toMs) const;

private:
    Database& _database;
};

} // namespace patina::data

#endif // PATINA_DATA_SESSION_REPOSITORY_H
