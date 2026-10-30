#ifndef PATINA_ENGINE_SESSION_STORE_H
#define PATINA_ENGINE_SESSION_STORE_H

#include "data/types.h"

#include <QString>

#include <optional>

namespace patina::engine
{

// 时间戳键名。
// 存在 settings 表里（key/value 文本），engine 只负责写，不解释来源。
inline QString lastHeartbeatKey()
{
    return QStringLiteral("__tracker_last_heartbeat_ms");
}

inline QString lastSuccessfulSampleKey()
{
    return QStringLiteral("__tracker_last_successful_sample_ms");
}

// 当前未闭合段。engine 判断“要不要切段”时用它对齐库里的实际情况。
struct OpenSession
{
    qint64 id = 0;
    QString exeName;
    QString windowTitle;
    qint64 startTimeMs = 0;
    // 连续性组起点：同一组内的段落共享这个值，用来把「切走几十秒又
    // 切回来」的碎片归到同一段连续使用里。库里保证不晚于 startTimeMs。
    qint64 continuityGroupStartTimeMs = 0;
};

// 开新段所需的信息。appId 不在这里：由实现方先 upsert apps 表拿到外键。
struct SessionStart
{
    QString exeName;
    QString exePath;
    // 已经过标题策略过滤：关掉标题录制时这里就是空串，实现方直接落库。
    QString windowTitle;
    // 版本资源里解析出的显示名。为空表示没解析到，不动库里已有的值。
    QString displayName;
    qint64 startTimeMs = 0;
    // 0 表示本段自成一个新组；非 0 表示复用已有组的起点（短时切回）。
    qint64 continuityGroupStartTimeMs = 0;
};

// engine 与持久化之间的唯一接缝。
//
// 只在 engine/ 定义、由 app/ 用 data/ 的 repository 实现，测试用内存假实现。
// 这样 engine 的边界语义可以脱离 SQLite 单独验证，也不会让 data/ 反向依赖
// engine/（data/ 只管存取，engine/ 只管语义）。
class SessionStore
{
public:
    virtual ~SessionStore() = default;

    virtual data::Result<std::optional<OpenSession>> loadOpenSession() const = 0;

    // 返回是否真的闭合了一段（没有未闭合段时返回 false，不算错误）。
    virtual data::Result<bool> endOpenSession(qint64 endTimeMs) = 0;

    virtual data::Result<bool> startSession(const SessionStart& start) = 0;

    // 同一应用内换窗口实例或换标题：只刷新标题，不切段。
    // timestampMs 是标题样本的切换时刻——样本历史要按时间切，不能用「现在」。
    virtual data::Result<bool> refreshOpenTitle(const QString& exeName,
                                                const QString& windowTitle,
                                                qint64 timestampMs) = 0;

    // 关掉标题录制时调用：把当前段已持久化的标题清空，并闭合它的
    // 标题样本。时长统计不动——关掉标题不该让已经记下的时间消失。
    virtual data::Result<bool> clearOpenTitle(qint64 timestampMs) = 0;

    virtual data::Result<std::optional<qint64>> loadTimestamp(const QString& key) const = 0;
    virtual data::Status saveTimestamp(const QString& key, qint64 valueMs) = 0;

    // settings 表的通用读写。引擎参数、暂停状态、标题开关、
    // 按应用覆盖都走这里。
    virtual data::Result<std::optional<QString>> loadSettingValue(const QString& key) const = 0;
    virtual data::Status saveSettingValue(const QString& key, const QString& value) = 0;

    // 匿名活动：只记时长，不记 exe 身份与标题。
    // observe 返回是否新开了一个匿名段；seal 幂等。
    virtual data::Result<bool> observeAnonymousActivity(qint64 nowMs) = 0;
    virtual data::Status sealAnonymousActivity(qint64 cutoffMs) = 0;

    // 异常退出后启动时收口匿名段——按「最后观察到的时刻」推断终点，
    // 观察记录不可信时退回起点。
    virtual data::Status sealInterruptedAnonymousActivity(qint64 cutoffMs) = 0;

    // 事务：「写事实」与「写派生状态」在同一事务内。
    // 一次采样的所有写入都必须落在一个 begin/commit 之间。
    virtual data::Status begin() = 0;
    virtual data::Status commit() = 0;
    virtual data::Status rollback() = 0;
};

// RAII 事务作用域：析构时未提交则回滚。
// 与其依赖的 data::Transaction 一样不支持嵌套。
class ScopedStoreTransaction
{
public:
    explicit ScopedStoreTransaction(SessionStore& store);
    ~ScopedStoreTransaction();

    ScopedStoreTransaction(const ScopedStoreTransaction&) = delete;
    ScopedStoreTransaction& operator=(const ScopedStoreTransaction&) = delete;

    bool isActive() const { return _active; }

    data::Status commit();

private:
    SessionStore& _store;
    bool _active = false;
};

} // namespace patina::engine

#endif // PATINA_ENGINE_SESSION_STORE_H
