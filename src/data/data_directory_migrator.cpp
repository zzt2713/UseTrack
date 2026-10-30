#include "data/data_directory_migrator.h"

#include "data/database.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace patina::data
{
namespace DataDirectoryMigrator
{
namespace
{

QString describeError(const QSqlQuery& query)
{
    const QSqlError error = query.lastError();
    return error.isValid() ? error.text() : QStringLiteral("SQL 执行失败");
}

} // namespace

QString validateTarget(const QString& targetDirectory, const QString& currentDirectory)
{
    const QString trimmed = targetDirectory.trimmed();
    if (trimmed.isEmpty()) {
        return QStringLiteral("目标目录为空");
    }
    const QDir target(QDir::cleanPath(trimmed));
    const QDir current(QDir::cleanPath(currentDirectory.trimmed()));
    if (target.absolutePath() == current.absolutePath()) {
        return QStringLiteral("新目录与当前目录相同");
    }
    // 目标里已有同名库文件：拒绝，避免覆盖别的数据。
    if (QFileInfo::exists(target.absoluteFilePath(QStringLiteral("usetrack.db")))) {
        return QStringLiteral("目标目录已有 usetrack.db，为避免覆盖请换一个目录");
    }

    if (!target.exists()) {
        if (!QDir().mkpath(target.absolutePath())) {
            return QStringLiteral("无法创建目录：%1").arg(QDir::toNativeSeparators(target.absolutePath()));
        }
    }
    // 可写探测：真写一个临时文件。
    const QString probe = target.absoluteFilePath(
        QStringLiteral(".usetrack-write-probe-%1").arg(QCoreApplication::applicationPid()));
    QFile probeFile(probe);
    if (!probeFile.open(QIODevice::WriteOnly)) {
        return QStringLiteral("目录不可写：%1").arg(QDir::toNativeSeparators(target.absolutePath()));
    }
    probeFile.close();
    QFile::remove(probe);
    return QString();
}

Status migrateDatabaseFile(const QString& currentPath, const QString& targetPath)
{
    const QString source = QDir::cleanPath(currentPath);
    const QString destination = QDir::cleanPath(targetPath);
    if (source.isEmpty() || destination.isEmpty()) {
        return Status::failure(QStringLiteral("库文件路径为空"));
    }
    if (!QFileInfo::exists(source)) {
        return Status::failure(QStringLiteral("旧库文件不存在：%1").arg(QDir::toNativeSeparators(source)));
    }
    if (QFileInfo(source).absoluteFilePath() == QFileInfo(destination).absoluteFilePath()) {
        return Status::failure(QStringLiteral("新旧路径相同"));
    }

    // WAL 处理：拷贝前把 -wal 合并进主文件（拷单文件不丢数据的关键一步）。
    {
        TemporaryConnection checkpoint(source, false);
        if (!checkpoint.isOpen()) {
            return Status::failure(QStringLiteral("无法打开旧库做 checkpoint：%1").arg(checkpoint.error()));
        }
        QSqlQuery query(checkpoint.connection());
        if (!query.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"))) {
            return Status::failure(QStringLiteral("WAL checkpoint 失败：%1").arg(describeError(query)));
        }
    }

    // 拷贝：写临时名再换名（破坏性路径上不留半截文件当真身）。
    const QString temporary = destination + QStringLiteral(".tmp-%1").arg(QCoreApplication::applicationPid());
    QFile::remove(temporary);
    if (!QFile::copy(source, temporary)) {
        QFile::remove(temporary);
        return Status::failure(QStringLiteral("拷贝库文件失败：%1 → %2")
                                   .arg(QDir::toNativeSeparators(source),
                                        QDir::toNativeSeparators(temporary)));
    }
    if (QFile::exists(destination) && !QFile::remove(destination)) {
        QFile::remove(temporary);
        return Status::failure(QStringLiteral("无法覆盖目标文件：%1").arg(QDir::toNativeSeparators(destination)));
    }
    if (!QFile::rename(temporary, destination)) {
        QFile::remove(temporary);
        return Status::failure(QStringLiteral("无法落位库文件：%1").arg(QDir::toNativeSeparators(destination)));
    }

    // 校验：行数一致才算搬完。
    const Result<bool> verified = verifyRowCountsMatch(source, destination);
    if (!verified.ok()) {
        QFile::remove(destination);
        return Status::failure(verified.error);
    }
    if (!verified.value) {
        QFile::remove(destination);
        return Status::failure(QStringLiteral("搬迁后行数校验不一致，已放弃新文件（旧数据无损）"));
    }
    return Status::success();
}

Result<bool> verifyRowCountsMatch(const QString& currentPath, const QString& targetPath)
{
    qint64 sourceCount = -1;
    qint64 targetCount = -1;
    {
        TemporaryConnection source(currentPath, true);
        if (!source.isOpen()) {
            return fail<bool>(QStringLiteral("无法只读打开旧库：%1").arg(source.error()));
        }
        sourceCount = source.sessionCount();
    }
    {
        TemporaryConnection target(targetPath, true);
        if (!target.isOpen()) {
            return fail<bool>(QStringLiteral("无法只读打开新库：%1").arg(target.error()));
        }
        targetCount = target.sessionCount();
    }
    if (sourceCount < 0 || targetCount < 0) {
        return fail<bool>(QStringLiteral("行数校验查询失败"));
    }
    return succeed(sourceCount == targetCount);
}

TemporaryConnection::TemporaryConnection(const QString& filePath, bool readOnly)
{
    _name = QStringLiteral("patina-migrate-%1")
                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), _name);
    database.setDatabaseName(filePath);
    if (readOnly) {
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    }
    _open = database.open();
    if (!_open) {
        _error = database.lastError().text();
    }
}

TemporaryConnection::~TemporaryConnection()
{
    {
        QSqlDatabase database = QSqlDatabase::database(_name, false);
        if (database.isOpen()) {
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(_name);
}

qint64 TemporaryConnection::sessionCount() const
{
    if (!_open) {
        return -1;
    }
    QSqlQuery query(connection());
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM sessions")) || !query.next()) {
        return -1;
    }
    return query.value(0).toLongLong();
}

QSqlDatabase TemporaryConnection::connection() const
{
    return QSqlDatabase::database(_name, false);
}

} // namespace DataDirectoryMigrator
} // namespace patina::data
