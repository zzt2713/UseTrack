#ifndef PATINA_DATA_SCHEMA_H
#define PATINA_DATA_SCHEMA_H

#include "data/types.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace patina::data
{

class Database;

// 一次 schema 变更。
//
// 规则：已发布的迁移只准往后追加，不准修改或删除。
// 老库是按当时的语句升上来的，改了旧语句会让新老库结构不一致。
struct Migration
{
    int version = 0;
    QString description;
    QStringList statements; // SQLite 驱动一次只能执行一条语句，这里逐条存
};

// 全部迁移，按 version 升序。
QVector<Migration> migrations();

// 当前程序支持的最高版本。
int latestSchemaVersion();

// 读库里的当前 schema 版本；库还是空的（没有 schema_migrations 表）返回 0。
Result<int> schemaVersion(const Database& database);

// 把库前进到最新版本。
// - 已是最新：空操作。
// - 落后：按 version 升序逐个迁移，每个迁移在自己的事务里完成（DDL 也走事务，
//   失败则整个迁移回滚，不会留下半张表）。
// - 高于本程序：直接失败且不做任何写入，避免旧程序写坏新库。
Status applyMigrations(Database& database);

} // namespace patina::data

#endif // PATINA_DATA_SCHEMA_H
