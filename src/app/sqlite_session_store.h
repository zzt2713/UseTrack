#ifndef PATINA_APP_SQLITE_SESSION_STORE_H
#define PATINA_APP_SQLITE_SESSION_STORE_H

#include "data/anonymous_activity_repository.h"
#include "data/app_repository.h"
#include "data/session_repository.h"
#include "data/session_title_sample_repository.h"
#include "data/settings_repository.h"
#include "engine/session_store.h"

#include <memory>

namespace patina::data
{
class Database;
class Transaction;
} // namespace patina::data

namespace patina::app
{

// engine::SessionStore 的 SQLite 实现——engine 与 data 之间唯一的接缝。
//
// 放在 app/ 而不是 engine/ 或 data/：engine/ 不写 SQL，
// data/ 不反向依赖 engine/。粘合代码只能落在拥有编排的那一层。
//
// 事务由 begin()/commit()/rollback() 显式控制，engine 用它保证
// 「写事实 + 写派生状态」原子。
class SqliteSessionStore final : public engine::SessionStore
{
public:
    explicit SqliteSessionStore(data::Database& database);
    ~SqliteSessionStore() override;

    SqliteSessionStore(const SqliteSessionStore&) = delete;
    SqliteSessionStore& operator=(const SqliteSessionStore&) = delete;

    data::Result<std::optional<engine::OpenSession>> loadOpenSession() const override;
    data::Result<bool> endOpenSession(qint64 endTimeMs) override;
    data::Result<bool> startSession(const engine::SessionStart& start) override;
    data::Result<bool> refreshOpenTitle(const QString& exeName,
                                        const QString& windowTitle,
                                        qint64 timestampMs) override;
    data::Result<bool> clearOpenTitle(qint64 timestampMs) override;

    data::Result<std::optional<qint64>> loadTimestamp(const QString& key) const override;
    data::Status saveTimestamp(const QString& key, qint64 valueMs) override;

    data::Result<std::optional<QString>> loadSettingValue(const QString& key) const override;
    data::Status saveSettingValue(const QString& key, const QString& value) override;

    data::Result<bool> observeAnonymousActivity(qint64 nowMs) override;
    data::Status sealAnonymousActivity(qint64 cutoffMs) override;
    data::Status sealInterruptedAnonymousActivity(qint64 cutoffMs) override;

    data::Status begin() override;
    data::Status commit() override;
    data::Status rollback() override;

private:
    data::Database& _database;
    data::AppRepository _apps;
    data::SessionRepository _sessions;
    data::SessionTitleSampleRepository _titleSamples;
    data::AnonymousActivityRepository _anonymousActivity;
    data::SettingsRepository _settings;

    // 同一时刻只允许一个事务；嵌套说明调用方漏了 commit。
    std::unique_ptr<data::Transaction> _transaction;
};

} // namespace patina::app

#endif // PATINA_APP_SQLITE_SESSION_STORE_H
