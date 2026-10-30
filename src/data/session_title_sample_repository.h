#ifndef PATINA_DATA_SESSION_TITLE_SAMPLE_REPOSITORY_H
#define PATINA_DATA_SESSION_TITLE_SAMPLE_REPOSITORY_H

#include "data/types.h"

#include <QVector>

namespace patina::data
{

class Database;

// 一段标题样本：同一个会话里窗口标题保持不变的那段时间。
//
// 为什么不全塞进 sessions.window_title：那个字段只留最终值，用户在一个应用里
// 换过的那些文档/网页就丢了。样本历史才看得出「这段时间具体在看什么」。
//
// 闭合语义：end_time_ms 为空表示样本还在延续；样本闭合时间必须
// 夹在 [start, 段终点] 之间，写小于 start 的时间只会得到 start。
class SessionTitleSampleRepository
{
public:
    explicit SessionTitleSampleRepository(Database& database) : _database(database) {}

    // 开一个标题样本。title 为空白时不记录，返回 false 且不算错误——
    // 关掉标题录制后走的就是这条路径。
    Status start(qint64 sessionId, const QString& title, qint64 startTimeMs);

    // 闭合该会话仍未闭合的样本。没有未闭合样本时空操作。
    Status finishActive(qint64 sessionId, qint64 endTimeMs);

    // 标题变了：先把旧样本闭合在当前时刻，再开新样本。
    Status replaceActive(qint64 sessionId, const QString& title, qint64 timestampMs);

    // 按 session 取样本明细（历史页标题明细用）。
    Result<QVector<SessionTitleSample>> listBySession(qint64 sessionId) const;

private:
    Database& _database;
};

} // namespace patina::data

#endif // PATINA_DATA_SESSION_TITLE_SAMPLE_REPOSITORY_H
