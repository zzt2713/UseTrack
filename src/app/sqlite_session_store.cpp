#include "app/sqlite_session_store.h"

#include "data/database.h"

namespace patina::app
{

SqliteSessionStore::SqliteSessionStore(data::Database& database)
    : _database(database)
    , _apps(database)
    , _sessions(database)
    , _titleSamples(database)
    , _anonymousActivity(database)
    , _settings(database)
{
}

SqliteSessionStore::~SqliteSessionStore() = default;

data::Result<std::optional<engine::OpenSession>> SqliteSessionStore::loadOpenSession() const
{
    const data::Result<std::optional<data::SessionSegment>> open = _sessions.findOpen();
    if (!open.ok()) {
        return data::fail<std::optional<engine::OpenSession>>(open.error);
    }
    if (!open.value.has_value()) {
        return data::succeed(std::optional<engine::OpenSession>{});
    }

    // sessions 只存 app_id；engine 判身份要 exe 名，所以这里补一次 apps 查询。
    const data::Result<data::App> app = _apps.findById(open.value->appId);
    if (!app.ok()) {
        return data::fail<std::optional<engine::OpenSession>>(app.error);
    }

    engine::OpenSession session;
    session.id = open.value->id;
    session.exeName = app.value.exeName;
    session.windowTitle = open.value->windowTitle;
    session.startTimeMs = open.value->startTimeMs;
    session.continuityGroupStartTimeMs = open.value->continuityGroupStartTimeMs;
    return data::succeed(std::optional<engine::OpenSession>{session});
}

data::Result<bool> SqliteSessionStore::endOpenSession(qint64 endTimeMs)
{
    // closeOpen 是幂等的 UPDATE，不告诉影响了多少行；先查一次才能如实回答
    // engine 的「是否真的闭合了」（它据此决定要不要清运行态）。
    const data::Result<std::optional<data::SessionSegment>> open = _sessions.findOpen();
    if (!open.ok()) {
        return data::fail<bool>(open.error);
    }
    if (!open.value.has_value()) {
        return data::succeed(false);
    }

    const data::Status closed = _sessions.closeOpen(endTimeMs);
    if (!closed.ok()) {
        return data::fail<bool>(closed.error);
    }

    // 段封口时同步闭合标题样本。样本的终点夹在 [起点, 段终点] 之间，
    // 所以重复封口不会把已经收好的样本越改越短。
    const data::Status sampleClosed = _titleSamples.finishActive(open.value->id, endTimeMs);
    if (!sampleClosed.ok()) {
        return data::fail<bool>(sampleClosed.error);
    }
    return data::succeed(true);
}

data::Result<bool> SqliteSessionStore::startSession(const engine::SessionStart& start)
{
    if (start.exeName.trimmed().isEmpty()) {
        return data::fail<bool>(QStringLiteral("应用名为空，无法开段"));
    }

    // 先 upsert apps 拿到外键：engine 只认识 exe 名，app_id 是 data 层的事。
    const data::Result<data::App> app = _apps.touch(start.exeName, start.exePath, start.startTimeMs);
    if (!app.ok()) {
        return data::fail<bool>(app.error);
    }

    // 显示名只在库里那一格还是空的时候补：用户改过的名字不能被
    // 版本资源里的自动解析值覆盖。
    if (!start.displayName.trimmed().isEmpty() && app.value.displayName.trimmed().isEmpty()) {
        const data::Status named = _apps.ensureDisplayName(app.value.exeName, start.displayName);
        if (!named.ok()) {
            return data::fail<bool>(named.error);
        }
    }

    const data::Result<data::SessionSegment> segment = _sessions.openSegment(
        app.value.id, start.windowTitle, start.startTimeMs, start.continuityGroupStartTimeMs);
    if (!segment.ok()) {
        return data::fail<bool>(segment.error);
    }

    // 开段与它的第一条标题样本必须在同一个事务里（调用方已 begin）。
    // 关掉标题录制时 windowTitle 为空，start() 会直接跳过不写。
    const data::Status sample =
        _titleSamples.start(segment.value.id, start.windowTitle, start.startTimeMs);
    if (!sample.ok()) {
        return data::fail<bool>(sample.error);
    }
    return data::succeed(true);
}

data::Result<bool> SqliteSessionStore::refreshOpenTitle(const QString& exeName,
                                                        const QString& windowTitle,
                                                        qint64 timestampMs)
{
    Q_UNUSED(exeName)
    const data::Result<std::optional<data::SessionSegment>> open = _sessions.findOpen();
    if (!open.ok()) {
        return data::fail<bool>(open.error);
    }
    if (!open.value.has_value()) {
        return data::succeed(false);
    }

    const data::Status updated = _sessions.updateOpenTitle(windowTitle);
    if (!updated.ok()) {
        return data::fail<bool>(updated.error);
    }

    // 标题变了：闭合旧样本、开新样本。windowTitle 为空（关标题录制）时
    // replaceActive 只闭合旧样本，不开新的。
    const data::Status sample =
        _titleSamples.replaceActive(open.value->id, windowTitle, timestampMs);
    if (!sample.ok()) {
        return data::fail<bool>(sample.error);
    }
    return data::succeed(true);
}

data::Result<bool> SqliteSessionStore::clearOpenTitle(qint64 timestampMs)
{
    const data::Result<std::optional<data::SessionSegment>> open = _sessions.findOpen();
    if (!open.ok()) {
        return data::fail<bool>(open.error);
    }
    if (!open.value.has_value()) {
        return data::succeed(false);
    }

    const data::Status updated = _sessions.updateOpenTitle(QString());
    if (!updated.ok()) {
        return data::fail<bool>(updated.error);
    }
    const data::Status sampleClosed = _titleSamples.finishActive(open.value->id, timestampMs);
    if (!sampleClosed.ok()) {
        return data::fail<bool>(sampleClosed.error);
    }
    return data::succeed(true);
}

data::Result<std::optional<qint64>> SqliteSessionStore::loadTimestamp(const QString& key) const
{
    const data::Result<std::optional<QString>> raw = _settings.value(key);
    if (!raw.ok()) {
        return data::fail<std::optional<qint64>>(raw.error);
    }
    if (!raw.value.has_value()) {
        return data::succeed(std::optional<qint64>{});
    }

    bool converted = false;
    const qint64 value = raw.value->toLongLong(&converted);
    if (!converted) {
        // 时间戳坏了不该拖垮启动：当成「没有记录」，随后按段起点封口。
        return data::succeed(std::optional<qint64>{});
    }
    return data::succeed(std::optional<qint64>{value});
}

data::Status SqliteSessionStore::saveTimestamp(const QString& key, qint64 valueMs)
{
    return _settings.setValue(key, QString::number(valueMs));
}

data::Result<std::optional<QString>> SqliteSessionStore::loadSettingValue(const QString& key) const
{
    return _settings.value(key);
}

data::Status SqliteSessionStore::saveSettingValue(const QString& key, const QString& value)
{
    return _settings.setValue(key, value);
}

data::Result<bool> SqliteSessionStore::observeAnonymousActivity(qint64 nowMs)
{
    return _anonymousActivity.observe(nowMs);
}

data::Status SqliteSessionStore::sealAnonymousActivity(qint64 cutoffMs)
{
    return _anonymousActivity.seal(cutoffMs);
}

data::Status SqliteSessionStore::sealInterruptedAnonymousActivity(qint64 cutoffMs)
{
    return _anonymousActivity.sealInterrupted(cutoffMs);
}

data::Status SqliteSessionStore::begin()
{
    if (_transaction) {
        return data::Status::failure(QStringLiteral("事务已开启，不支持嵌套"));
    }

    auto transaction = std::make_unique<data::Transaction>(_database);
    if (!transaction->isActive()) {
        return data::Status::failure(QStringLiteral("无法开启事务"));
    }
    _transaction = std::move(transaction);
    return data::Status::success();
}

data::Status SqliteSessionStore::commit()
{
    if (!_transaction) {
        return data::Status::failure(QStringLiteral("事务未开启"));
    }
    // 先取回再清空：data::Transaction 的析构会回滚未提交的事务。
    const std::unique_ptr<data::Transaction> transaction = std::move(_transaction);
    _transaction.reset();
    return transaction->commit();
}

data::Status SqliteSessionStore::rollback()
{
    if (!_transaction) {
        return data::Status::success();
    }
    const std::unique_ptr<data::Transaction> transaction = std::move(_transaction);
    _transaction.reset();
    return transaction->rollback();
}

} // namespace patina::app
