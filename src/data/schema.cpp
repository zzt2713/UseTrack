#include "data/schema.h"

#include "data/database.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace patina::data
{
namespace
{

// 迁移记录表：自己不受版本管理，第一次 applyMigrations 时按需建出来。
const char* const kMigrationTableSql =
    "CREATE TABLE IF NOT EXISTS schema_migrations ("
    "    version INTEGER PRIMARY KEY,"
    "    description TEXT NOT NULL,"
    "    applied_ms INTEGER NOT NULL"
    ")";

QString describeError(const QSqlQuery& query)
{
    const QSqlError error = query.lastError();
    if (!error.isValid()) {
        return QStringLiteral("SQL 执行失败");
    }
    return error.text();
}

} // namespace

QVector<Migration> migrations()
{
    QVector<Migration> list;

    // v1：三张基础表。
    // sessions.app_id 指向 apps，删应用会连带删掉它的事实段——
    // 应用是被引用方，不存在“留着段却没有应用”的中间态。
    // 时间戳一律 epoch 毫秒。end_time 为 NULL 表示段还开着。
    list.append(Migration{
        1,
        QStringLiteral("create_baseline_tables"),
        QStringList{
            QStringLiteral(
                "CREATE TABLE apps ("
                "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "    exe_name TEXT NOT NULL COLLATE NOCASE UNIQUE,"
                "    exe_path TEXT NOT NULL DEFAULT '',"
                "    display_name TEXT NOT NULL DEFAULT '',"
                "    first_seen_ms INTEGER NOT NULL,"
                "    last_seen_ms INTEGER NOT NULL"
                ")"),
            QStringLiteral(
                "CREATE TABLE sessions ("
                "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "    app_id INTEGER NOT NULL REFERENCES apps(id) ON DELETE CASCADE,"
                "    window_title TEXT NOT NULL DEFAULT '',"
                "    start_time_ms INTEGER NOT NULL,"
                "    end_time_ms INTEGER,"
                "    CHECK (end_time_ms IS NULL OR end_time_ms >= start_time_ms)"
                ")"),
            QStringLiteral(
                "CREATE TABLE settings ("
                "    key TEXT PRIMARY KEY,"
                "    value TEXT NOT NULL"
                ")"),
        },
    });

    // v2：查询索引 + 单活跃段约束。
    // 部分唯一索引保证任何时刻最多只有一个未闭合段：崩溃/断电重启后
    // engine 必须先封口再开新段，写错会在这里被库挡住。
    list.append(Migration{
        2,
        QStringLiteral("add_session_indexes_and_single_open_guard"),
        QStringList{
            QStringLiteral(
                "CREATE INDEX idx_sessions_app_start ON sessions(app_id, start_time_ms)"),
            QStringLiteral(
                "CREATE INDEX idx_sessions_range ON sessions(start_time_ms, end_time_ms)"),
            QStringLiteral(
                "CREATE UNIQUE INDEX idx_sessions_single_open ON sessions((1)) "
                "WHERE end_time_ms IS NULL"),
        },
    });

    // v3：应用级的用户覆盖项（分类管理页写，读模型消费）。
    // 三个列都是「空/默认 = 走内置规则」：
    //   category_id 为空   → 用 classifyExecutable() 的内置规则表
    //   color_hex   为空   → 跟随所属分类的颜色
    //   track_enabled = 0  → 历史段仍留在 sessions 里，但不进任何汇总
    // display_name 在 v1 就建好了，这次才接上读写的活。
    list.append(Migration{
        3,
        QStringLiteral("add_app_classification_overrides"),
        QStringList{
            QStringLiteral("ALTER TABLE apps ADD COLUMN category_id TEXT NOT NULL DEFAULT ''"),
            QStringLiteral("ALTER TABLE apps ADD COLUMN color_hex TEXT NOT NULL DEFAULT ''"),
            QStringLiteral("ALTER TABLE apps ADD COLUMN track_enabled INTEGER NOT NULL DEFAULT 1"),
        },
    });

    // v4：continuity 组起点 + 追踪参数默认值。
    // continuity_group_start_time_ms 是「同一段连续使用」的起点：短暂切走再切回
    // 窗口内复用原起点，超窗才开新组。老数据按段起点回填。
    // 设置项用 ON CONFLICT DO NOTHING：升级用户的既有配置不会被默认值覆盖。
    list.append(Migration{
        4,
        QStringLiteral("add_session_continuity_and_tracking_settings"),
        QStringList{
            QStringLiteral("ALTER TABLE sessions ADD COLUMN continuity_group_start_time_ms INTEGER"),
            QStringLiteral("UPDATE sessions SET continuity_group_start_time_ms = start_time_ms "
                           "WHERE continuity_group_start_time_ms IS NULL"),
            QStringLiteral("INSERT INTO settings (key, value) VALUES "
                           "('timeline_merge_gap_secs', '180') ON CONFLICT(key) DO NOTHING"),
            QStringLiteral("INSERT INTO settings (key, value) VALUES "
                           "('idle_timeout_secs', '900') ON CONFLICT(key) DO NOTHING"),
            QStringLiteral("INSERT INTO settings (key, value) VALUES "
                           "('title_recording_enabled', 'true') ON CONFLICT(key) DO NOTHING"),
            QStringLiteral("INSERT INTO settings (key, value) VALUES "
                           "('tracking_paused', 'false') ON CONFLICT(key) DO NOTHING"),
        },
    });

    // v5：标题样本历史。段标题只留最终值，取样历史才看得出用户在同一应用里
    // 读过哪些文档/网页。
    // end_time_ms 为空表示这个标题样本还在延续，闭合语义由 repository 负责。
    list.append(Migration{
        5,
        QStringLiteral("add_session_title_samples"),
        QStringList{
            QStringLiteral(
                "CREATE TABLE session_title_samples ("
                "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "    session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
                "    title TEXT NOT NULL,"
                "    start_time_ms INTEGER NOT NULL,"
                "    end_time_ms INTEGER,"
                "    CHECK (end_time_ms IS NULL OR end_time_ms >= start_time_ms)"
                ")"),
            QStringLiteral("CREATE INDEX idx_session_title_samples_session_time "
                           "ON session_title_samples(session_id, start_time_ms)"),
            QStringLiteral("CREATE INDEX idx_session_title_samples_time "
                           "ON session_title_samples(start_time_ms, end_time_ms)"),
        },
    });

    // v6：匿名活动段。只记时长，不含任何应用身份与标题——用户在某个应用上
    // 开了匿名统计后，前台时间落到这张表，sessions 里不留痕迹。
    // 单活跃段约束与 sessions 同理，用部分唯一索引保证。
    list.append(Migration{
        6,
        QStringLiteral("add_anonymous_activity"),
        QStringList{
            QStringLiteral(
                "CREATE TABLE anonymous_activity ("
                "    id TEXT PRIMARY KEY NOT NULL,"
                "    start_time_ms INTEGER NOT NULL,"
                "    end_time_ms INTEGER,"
                "    observed_until_ms INTEGER NOT NULL,"
                "    CHECK (observed_until_ms >= start_time_ms),"
                "    CHECK (end_time_ms IS NULL OR end_time_ms >= start_time_ms)"
                ")"),
            QStringLiteral("CREATE UNIQUE INDEX idx_anonymous_activity_single_open "
                           "ON anonymous_activity((1)) WHERE end_time_ms IS NULL"),
            QStringLiteral("CREATE INDEX idx_anonymous_activity_range "
                           "ON anonymous_activity(COALESCE(end_time_ms, observed_until_ms), "
                           "start_time_ms)"),
        },
    });

    // v7：工具专用表。
    // 早期把工具状态整份塞在 settings 的 JSON 里，圈次与每日统计没法落库；
    // 这里拆成六张表。同时把 settings 里 4 个 JSON 键的一次性迁移也放进这个版本：
    // 旧键保留（迁移后旧键保留一轮再清），运行时读新表成功后不再回读旧键。
    // 列结构：状态用 TEXT，时间一律 epoch 毫秒。
    list.append(Migration{
        7,
        QStringLiteral("create_tool_tables_and_migrate_json_state"),
        QStringList{
            QStringLiteral(
                "CREATE TABLE tool_reminders ("
                "    id TEXT PRIMARY KEY,"
                "    label TEXT NOT NULL,"
                "    scheduled_at_ms INTEGER NOT NULL,"
                "    created_at_ms INTEGER NOT NULL,"
                "    status TEXT NOT NULL,"
                "    fired_at_ms INTEGER"
                ")"),
            QStringLiteral("CREATE INDEX idx_tool_reminders_status_schedule "
                           "ON tool_reminders(status, scheduled_at_ms)"),
            QStringLiteral(
                "CREATE TABLE tool_timers ("
                "    id INTEGER PRIMARY KEY CHECK(id = 1),"
                "    mode TEXT NOT NULL,"
                "    label TEXT NOT NULL DEFAULT '',"
                "    duration_ms INTEGER NOT NULL DEFAULT 0,"
                "    accumulated_ms INTEGER NOT NULL DEFAULT 0,"
                "    started_at_ms INTEGER NOT NULL DEFAULT 0,"
                "    completed_at_ms INTEGER NOT NULL DEFAULT 0,"
                "    status TEXT NOT NULL,"
                "    updated_at_ms INTEGER NOT NULL DEFAULT 0"
                ")"),
            QStringLiteral(
                "CREATE TABLE tool_timer_laps ("
                "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "    lap_index INTEGER NOT NULL,"
                "    started_at_ms INTEGER NOT NULL,"
                "    ended_at_ms INTEGER NOT NULL,"
                "    duration_ms INTEGER NOT NULL"
                ")"),
            QStringLiteral(
                "CREATE TABLE tool_pomodoro_runs ("
                "    id INTEGER PRIMARY KEY CHECK(id = 1),"
                "    phase TEXT NOT NULL,"
                "    status TEXT NOT NULL,"
                "    cycle_index INTEGER NOT NULL DEFAULT 1,"
                "    completed_focus_count INTEGER NOT NULL DEFAULT 0,"
                "    long_break_every INTEGER NOT NULL DEFAULT 4,"
                "    focus_ms INTEGER NOT NULL,"
                "    short_break_ms INTEGER NOT NULL,"
                "    long_break_ms INTEGER NOT NULL,"
                "    phase_started_at_ms INTEGER NOT NULL DEFAULT 0,"
                "    phase_remaining_at_start_ms INTEGER NOT NULL DEFAULT 0"
                ")"),
            QStringLiteral(
                "CREATE TABLE tool_daily_stats ("
                "    date_key TEXT PRIMARY KEY,"
                "    completed_focus_count INTEGER NOT NULL DEFAULT 0"
                ")"),
            QStringLiteral(
                "CREATE TABLE tool_activity_reminder_rules ("
                "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "    target_kind TEXT NOT NULL CHECK(target_kind IN ('app', 'category')),"
                "    app_name TEXT,"
                "    exe_name TEXT,"
                "    category_id TEXT,"
                "    label_snapshot TEXT NOT NULL,"
                "    limit_ms INTEGER NOT NULL CHECK(limit_ms BETWEEN 60000 AND 86400000),"
                "    message TEXT NOT NULL,"
                "    created_at_ms INTEGER NOT NULL,"
                "    updated_at_ms INTEGER NOT NULL,"
                "    disabled_at_ms INTEGER,"
                "    last_fired_date_key TEXT,"
                "    CHECK("
                "        (target_kind = 'app' AND app_name IS NOT NULL AND TRIM(app_name) <> ''"
                "            AND category_id IS NULL)"
                "        OR"
                "        (target_kind = 'category' AND app_name IS NULL AND exe_name IS NULL"
                "            AND category_id IS NOT NULL AND TRIM(category_id) <> '')"
                "    )"
                ")"),
            // 活动提醒按目标查询；规则数量是个位数，覆盖查询就够。
            QStringLiteral("CREATE INDEX idx_tool_activity_reminder_rules_active "
                           "ON tool_activity_reminder_rules(disabled_at_ms, target_kind)"),
            // —— 一次性数据迁移：settings 里的 4 个 JSON 键 → 新表 ——
            // JSON 损坏的键跳过（工具状态坏掉不该卡死迁移），写进 tool_* 后旧键原样保留。
            // json_valid 过滤放在子查询里：json_each 遇到非法 JSON 会直接报错，
            // 不能让它先于校验求值。
            QStringLiteral(
                "INSERT INTO tool_reminders (id, label, scheduled_at_ms, created_at_ms, status, fired_at_ms) "
                "SELECT "
                "    json_extract(je.value, '$.id'),"
                "    COALESCE(json_extract(je.value, '$.label'), ''),"
                "    COALESCE(json_extract(je.value, '$.scheduledAtMs'), 0),"
                "    COALESCE(json_extract(je.value, '$.createdAtMs'), 0),"
                "    COALESCE(json_extract(je.value, '$.status'), 'scheduled'),"
                "    COALESCE(json_extract(je.value, '$.firedAtMs'), 0) "
                "FROM (SELECT value FROM settings WHERE key = 'tools.reminders' "
                "      AND json_valid(value)) s, json_each(s.value) je"),
            QStringLiteral(
                "INSERT INTO tool_timers (id, mode, label, duration_ms, accumulated_ms, started_at_ms, "
                "completed_at_ms, status, updated_at_ms) "
                "SELECT 1, "
                "    COALESCE(json_extract(value, '$.mode'), 'stopwatch'), "
                "    COALESCE(json_extract(value, '$.label'), ''), "
                "    COALESCE(json_extract(value, '$.durationMs'), 0), "
                "    COALESCE(json_extract(value, '$.accumulatedMs'), 0), "
                "    COALESCE(json_extract(value, '$.startedAtMs'), 0), "
                "    COALESCE(json_extract(value, '$.completedAtMs'), 0), "
                "    COALESCE(json_extract(value, '$.status'), 'idle'), "
                "    strftime('%s','now') * 1000 "
                "FROM settings WHERE key = 'tools.timer' AND json_valid(value)"),
            QStringLiteral(
                "INSERT INTO tool_pomodoro_runs (id, phase, status, cycle_index, completed_focus_count, "
                "long_break_every, focus_ms, short_break_ms, long_break_ms, phase_started_at_ms, "
                "phase_remaining_at_start_ms) "
                "SELECT 1, "
                "    COALESCE(json_extract(value, '$.phase'), 'focus'), "
                "    COALESCE(json_extract(value, '$.status'), 'idle'), "
                "    COALESCE(json_extract(value, '$.cycleIndex'), 1), "
                "    COALESCE(json_extract(value, '$.completedFocusCount'), 0), "
                "    COALESCE(json_extract(value, '$.longBreakEvery'), 4), "
                "    COALESCE(json_extract(value, '$.focusMs'), 1500000), "
                "    COALESCE(json_extract(value, '$.shortBreakMs'), 300000), "
                "    COALESCE(json_extract(value, '$.longBreakMs'), 900000), "
                "    COALESCE(json_extract(value, '$.phaseStartedAtMs'), 0), "
                "    COALESCE(json_extract(value, '$.phaseRemainingAtStartMs'), 0) "
                "FROM settings WHERE key = 'tools.pomodoro' AND json_valid(value)"),
            QStringLiteral(
                "INSERT INTO tool_daily_stats (date_key, completed_focus_count) "
                "SELECT COALESCE(json_extract(value, '$.statDateKey'), ''), "
                "       COALESCE(json_extract(value, '$.completedFocusToday'), 0) "
                "FROM settings WHERE key = 'tools.pomodoro' AND json_valid(value) "
                "  AND json_extract(value, '$.statDateKey') IS NOT NULL"),
        },
    });

    // v8：图标持久化 + 导入批次表
    //（列名对齐本项目的 snake_case_ms 风格）。
    list.append(Migration{
        8,
        QStringLiteral("create_icon_cache_and_import_batches"),
        QStringList{
            QStringLiteral(
                "CREATE TABLE icon_cache ("
                "    exe_name TEXT PRIMARY KEY COLLATE NOCASE,"
                "    icon_png BLOB NOT NULL,"
                "    last_updated_ms INTEGER NOT NULL"
                ")"),
            QStringLiteral(
                "CREATE TABLE import_batches ("
                "    id TEXT PRIMARY KEY,"
                "    imported_at_ms INTEGER NOT NULL,"
                "    source_name TEXT NOT NULL,"
                "    source_kind TEXT NOT NULL,"
                "    source_fingerprint TEXT NOT NULL,"
                "    exact_session_count INTEGER NOT NULL DEFAULT 0,"
                "    bucket_count INTEGER NOT NULL DEFAULT 0"
                ")"),
            QStringLiteral("CREATE INDEX idx_import_batches_imported_at "
                           "ON import_batches(imported_at_ms, id)"),
            // 批次内的精确段：直接落 sessions（规范化 app_id 外键），行级指纹支持
            // 幂等提交；batch_id 级联删除是回滚的抓手（sessions.app_id 级联删应用，
            // 但这里删段不动 apps——应用可能被原生数据引用）。
            QStringLiteral(
                "CREATE TABLE import_batch_sessions ("
                "    batch_id TEXT NOT NULL,"
                "    session_id INTEGER NOT NULL UNIQUE,"
                "    fingerprint TEXT NOT NULL UNIQUE,"
                "    source_category TEXT,"
                "    PRIMARY KEY(batch_id, session_id),"
                "    FOREIGN KEY(batch_id) REFERENCES import_batches(id) ON DELETE CASCADE,"
                "    FOREIGN KEY(session_id) REFERENCES sessions(id) ON DELETE CASCADE"
                ")"),
            QStringLiteral("CREATE INDEX idx_import_batch_sessions_batch "
                           "ON import_batch_sessions(batch_id, session_id)"),
            // 批次内的小时桶：聚合数据不进 sessions，单独留表，
            // 各页聚合（趋势/排行）查询时 UNION 进来。
            QStringLiteral(
                "CREATE TABLE import_time_buckets ("
                "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "    batch_id TEXT NOT NULL,"
                "    fingerprint TEXT NOT NULL UNIQUE,"
                "    app_name TEXT NOT NULL,"
                "    exe_name TEXT NOT NULL COLLATE NOCASE,"
                "    bucket_start_ms INTEGER NOT NULL,"
                "    duration_ms INTEGER NOT NULL CHECK(duration_ms > 0 AND duration_ms <= 3600000),"
                "    source_category TEXT,"
                "    FOREIGN KEY(batch_id) REFERENCES import_batches(id) ON DELETE CASCADE"
                ")"),
            QStringLiteral("CREATE INDEX idx_import_time_buckets_time "
                           "ON import_time_buckets(bucket_start_ms, duration_ms)"),
            QStringLiteral("CREATE INDEX idx_import_time_buckets_exe_time "
                           "ON import_time_buckets(exe_name, bucket_start_ms)"),
            QStringLiteral("CREATE INDEX idx_import_time_buckets_batch "
                           "ON import_time_buckets(batch_id, id)"),
        },
    });

    // v9：清洗存量显示名。版本资源字符串尾部带的 \0 / 零宽字符已经被写进了
    // apps.display_name（界面上应用名后的「□」）。SQLite 的 TRIM 只去空格，
    // 这里用替换 + 截断的组合把常见脏字符洗掉：
    //   char(0)        —— NUL
    //   char(65279)    —— U+FEFF BOM
    //   char(8203)     —— U+200B zero-width space
    // 再 TRIM 收尾。新写入路径（resolveDisplayName）已同步清洗，这是一次性兜底。
    list.append(Migration{
        9,
        QStringLiteral("sanitize_display_names"),
        QStringList{
            QStringLiteral(
                "UPDATE apps SET display_name = TRIM("
                "    REPLACE(REPLACE(REPLACE(display_name,"
                "        char(0), ''),"
                "        char(65279), ''),"
                "        char(8203), ''))"
                " WHERE display_name <> ''"),
        },
    });

    // v10：图标缓存整体失效。
    //
    // v8 首版按「首次请求的尺寸」存图（34px 小图），之后改成统一 64px
    // 高清源——但已缓存的旧条目会被持久层直接命中，永远不会重提。
    // 清空一次，重启后全部按 64px 重解（一次性代价）。
    //
    // 显示名清洗原本也想放这里，但实测：版本资源带的尾部 NUL 会被 SQLite
    // 按 C 字符串语义吞掉——substr/unicode/replace 全都看不见它，
    // SQL 侧检测不到就无法选中这些行。净化改在 C++ 读出侧做
    // （AppRepository / ClassificationRepository / DashboardReadModel 的
    // sanitizeDisplayName），存储里的脏值无害，读出来永远是干净的。
    list.append(Migration{
        10,
        QStringLiteral("invalidate_icon_cache"),
        QStringList{
            QStringLiteral("DELETE FROM icon_cache"),
        },
    });

    // v11：应用级「不在列表显示」。
    //
    // 与 track_enabled 的分工：track_enabled = 0 是「不进汇总」，历史段照留；
    // hidden = 1 是「用户不想再看到这个应用」——所有页面的列表、图例、排行、
    // 提醒规则统计都不出现。历史行仍然一个字节不动，取消隐藏后原样回来。
    //
    // 列名用最直白的 hidden。
    list.append(Migration{
        11,
        QStringLiteral("add_app_hidden"),
        QStringList{
            QStringLiteral("ALTER TABLE apps ADD COLUMN hidden INTEGER NOT NULL DEFAULT 0"),
        },
    });

    return list;
}

int latestSchemaVersion()
{
    const QVector<Migration> list = migrations();
    return list.isEmpty() ? 0 : list.last().version;
}

Result<int> schemaVersion(const Database& database)
{
    if (!database.isOpen()) {
        return fail<int>(QStringLiteral("数据库未打开"));
    }

    QSqlDatabase connection = database.connection();

    // 还没有迁移记录表 = 空库。先确认表存在，不能直接 SELECT 让查询报错。
    QSqlQuery existsQuery(connection);
    if (!existsQuery.exec(QStringLiteral(
            "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'schema_migrations'"))
        || !existsQuery.next()) {
        return succeed(0);
    }

    QSqlQuery versionQuery(connection);
    if (!versionQuery.exec(QStringLiteral("SELECT COALESCE(MAX(version), 0) FROM schema_migrations"))) {
        return fail<int>(describeError(versionQuery));
    }
    if (!versionQuery.next()) {
        return succeed(0);
    }
    return succeed(versionQuery.value(0).toInt());
}

Status applyMigrations(Database& database)
{
    if (!database.isOpen()) {
        return Status::failure(QStringLiteral("数据库未打开"));
    }

    QSqlDatabase connection = database.connection();

    const Result<int> versionResult = schemaVersion(database);
    if (!versionResult.ok()) {
        return Status::failure(versionResult.error);
    }
    const int current = versionResult.value;

    const QVector<Migration> list = migrations();
    if (list.isEmpty()) {
        return Status::success();
    }
    const int latest = list.last().version;

    // 库比程序新：只读不动。降级会让程序看不懂新结构，甚至写坏数据。
    if (current > latest) {
        return Status::failure(
            QStringLiteral("数据库 schema 版本 %1 高于本程序支持的 %2，请升级程序")
                .arg(current)
                .arg(latest));
    }
    if (current == latest) {
        return Status::success();
    }

    {
        QSqlQuery createTable(connection);
        if (!createTable.exec(QString::fromLatin1(kMigrationTableSql))) {
            return Status::failure(describeError(createTable));
        }
    }

    const qint64 appliedMs = QDateTime::currentMSecsSinceEpoch();

    for (const Migration& migration : list) {
        if (migration.version <= current) {
            continue;
        }

        // 每个迁移一个事务：DDL 在 SQLite 里也走事务，中途失败整步回滚，
        // 不会留下“表建了一半、版本号却已经加过”的库。
        Transaction transaction(database);
        if (!transaction.isActive()) {
            return Status::failure(
                QStringLiteral("无法开启迁移事务（v%1）").arg(migration.version));
        }

        for (const QString& statement : migration.statements) {
            QSqlQuery query(connection);
            if (!query.exec(statement)) {
                const QString message = describeError(query);
                transaction.rollback();
                return Status::failure(QStringLiteral("迁移 v%1 (%2) 失败：%3")
                                           .arg(migration.version)
                                           .arg(migration.description, message));
            }
        }

        QSqlQuery record(connection);
        record.prepare(QStringLiteral(
            "INSERT INTO schema_migrations (version, description, applied_ms) VALUES (?, ?, ?)"));
        record.addBindValue(migration.version);
        record.addBindValue(migration.description);
        record.addBindValue(appliedMs);
        if (!record.exec()) {
            const QString message = describeError(record);
            transaction.rollback();
            return Status::failure(QStringLiteral("记录迁移 v%1 失败：%2")
                                       .arg(migration.version)
                                       .arg(message));
        }

        const Status committed = transaction.commit();
        if (!committed.ok()) {
            return Status::failure(QStringLiteral("提交迁移 v%1 失败：%2")
                                       .arg(migration.version)
                                       .arg(committed.error));
        }
    }

    return Status::success();
}

} // namespace patina::data
