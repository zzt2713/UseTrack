#include "data/export_service.h"

#include "data/app_category.h"
#include "data/database.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QTextStream>
#include <QUuid>
#include <QVariant>

namespace patina::data
{
namespace
{

QString describeError(const QSqlQuery& query)
{
    const QSqlError error = query.lastError();
    return error.isValid() ? error.text() : QStringLiteral("SQL 执行失败");
}

// —— 字段表（裁剪掉了网页活动专属列）——

const ExportFieldSpec* fieldTable()
{
    static const ExportFieldSpec table[] = {
        {"record_type", "记录类型"},
        {"category", "分类"},
        {"category_id", "分类ID"},
        {"start_time", "开始时间"},
        {"end_time", "结束时间"},
        {"duration_ms", "时长(毫秒)"},
        {"duration_minutes", "时长(分钟)"},
        {"app_name", "应用"},
        {"exe_name", "进程"},
        {"window_title", "窗口标题"},
        {"session_id", "会话ID"},
        {"local_date", "本地日期"},
        {"start_hour", "起始小时"},
        {"source_key", "来源键"},
        {"source_name", "来源名"},
    };
    static const int count = static_cast<int>(sizeof(table) / sizeof(table[0]));
    Q_UNUSED(count)
    return table;
}

constexpr int kFieldCount = 15;

int fieldIndex(const QString& name)
{
    for (int index = 0; index < kFieldCount; ++index) {
        if (name == QLatin1String(fieldTable()[index].name)) {
            return index;
        }
    }
    return -1;
}

// 一行导出记录的字段值。
struct ExportRow
{
    qint64 sessionId = 0;
    QString appName;
    QString exeName;
    QString categoryId;
    QString windowTitle;
    qint64 startTimeMs = 0;
    bool hasEndTime = false;
    qint64 endTimeMs = 0;
};

QDateTime moment(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms);
}

QString timeText(qint64 ms)
{
    return moment(ms).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString fieldValue(const QString& name, const ExportRow& row, qint64 nowMs)
{
    const qint64 durationMs = row.hasEndTime ? (row.endTimeMs - row.startTimeMs) : 0;
    const CategoryToken token = categoryToken(
        resolveCategoryId(row.exeName, row.categoryId));

    if (name == QLatin1String("record_type")) {
        return QStringLiteral("session");
    }
    if (name == QLatin1String("category")) {
        return token.label;
    }
    if (name == QLatin1String("category_id")) {
        return token.id;
    }
    if (name == QLatin1String("start_time")) {
        return timeText(row.startTimeMs);
    }
    if (name == QLatin1String("end_time")) {
        // 未闭合段按查询时刻封底：导出的区间统计与界面口径一致。
        return row.hasEndTime ? timeText(row.endTimeMs) : timeText(nowMs);
    }
    if (name == QLatin1String("duration_ms")) {
        return QString::number(row.hasEndTime ? durationMs : qMax<qint64>(0, nowMs - row.startTimeMs));
    }
    if (name == QLatin1String("duration_minutes")) {
        const qint64 ms = row.hasEndTime ? durationMs : qMax<qint64>(0, nowMs - row.startTimeMs);
        return QString::number(ms / 60000.0, 'f', 3);
    }
    if (name == QLatin1String("app_name")) {
        return row.appName;
    }
    if (name == QLatin1String("exe_name")) {
        return row.exeName;
    }
    if (name == QLatin1String("window_title")) {
        return row.windowTitle;
    }
    if (name == QLatin1String("session_id")) {
        return QString::number(row.sessionId);
    }
    if (name == QLatin1String("local_date")) {
        return moment(row.startTimeMs).date().toString(QStringLiteral("yyyy-MM-dd"));
    }
    if (name == QLatin1String("start_hour")) {
        return QString::number(moment(row.startTimeMs).time().hour());
    }
    if (name == QLatin1String("source_key")) {
        return row.exeName.toLower();
    }
    if (name == QLatin1String("source_name")) {
        return row.appName;
    }
    return QString();
}

// CSV 文本防公式注入。
QString sanitizeCsvText(const QString& value)
{
    const QString trimmed = value;
    qsizetype first = 0;
    while (first < trimmed.size() && trimmed.at(first).isSpace()) {
        ++first;
    }
    if (first < trimmed.size()) {
        const QChar head = trimmed.at(first);
        if (head == QLatin1Char('=') || head == QLatin1Char('+') || head == QLatin1Char('-')
            || head == QLatin1Char('@')) {
            return QLatin1Char('\'') + trimmed;
        }
    }
    return trimmed;
}

QString csvCell(const QString& value)
{
    QString cell = sanitizeCsvText(value);
    if (cell.contains(QLatin1Char('"')) || cell.contains(QLatin1Char(','))
        || cell.contains(QLatin1Char('\n')) || cell.contains(QLatin1Char('\r'))) {
        cell = QLatin1Char('"') + cell;
        cell.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        cell += QLatin1Char('"');
    }
    return cell;
}

QString markdownCell(const QString& value)
{
    QString cell = value;
    cell.replace(QLatin1Char('\r'), QLatin1Char(' '));
    cell.replace(QLatin1Char('\n'), QLatin1Char(' '));
    cell.replace(QLatin1Char('|'), QStringLiteral("\\|"));
    return cell;
}

// 分页读区间内的会话（不整表进内存）。每页 fetch 一批，
// 读完返回 false。字段顺序固定，按 start_time_ms + id keyset 分页。
class SessionCursor
{
public:
    SessionCursor(Database& database, qint64 startMs, qint64 endMs, int pageSize)
        : _database(database), _startMs(startMs), _endMs(endMs), _pageSize(pageSize)
    {
    }

    // 取下一批。false = 没有（更多）行了。
    bool next(QVector<ExportRow>& batch)
    {
        if (_exhausted) {
            return false;
        }

        // 交叠条件与各页读模型同口径：段起点早于区间终点，且
        // （已闭合段的终点、或未闭合段按当前时刻封底）晚于区间起点。
        // 终点/起点不限时对应条件直接省掉。
        QString sql = QStringLiteral(
            "SELECT s.id, a.display_name, a.exe_name, a.category_id, s.window_title, "
            "       s.start_time_ms, s.end_time_ms "
            "FROM sessions s JOIN apps a ON a.id = s.app_id "
            "WHERE 1 = 1 ");
        if (_endMs > 0) {
            sql += QStringLiteral(" AND s.start_time_ms < :end ");
        }
        if (_startMs > 0) {
            sql += QStringLiteral(" AND COALESCE(s.end_time_ms, :now) > :start ");
        }
        if (_lastKey > 0) {
            sql += QStringLiteral(
                " AND (s.start_time_ms > :key_start "
                "      OR (s.start_time_ms = :key_start2 AND s.id > :key_id)) ");
        }
        sql += QStringLiteral(" ORDER BY s.start_time_ms ASC, s.id ASC LIMIT :limit");

        QSqlQuery query(_database.connection());
        query.prepare(sql);
        if (_endMs > 0) {
            query.bindValue(QStringLiteral(":end"), _endMs);
        }
        if (_startMs > 0) {
            query.bindValue(QStringLiteral(":now"), QDateTime::currentMSecsSinceEpoch());
            query.bindValue(QStringLiteral(":start"), _startMs);
        }
        if (_lastKey > 0) {
            query.bindValue(QStringLiteral(":key_start"), _lastKeyStart);
            query.bindValue(QStringLiteral(":key_start2"), _lastKeyStart);
            query.bindValue(QStringLiteral(":key_id"), _lastKey);
        }
        query.bindValue(QStringLiteral(":limit"), _pageSize);
        if (!query.exec()) {
            _error = describeError(query);
            return false;
        }

        batch.clear();
        while (query.next()) {
            ExportRow row;
            row.sessionId = query.value(0).toLongLong();
            row.appName = query.value(1).toString();
            row.exeName = query.value(2).toString();
            row.categoryId = query.value(3).toString();
            row.windowTitle = query.value(4).toString();
            row.startTimeMs = query.value(5).toLongLong();
            if (!query.value(6).isNull()) {
                row.hasEndTime = true;
                row.endTimeMs = query.value(6).toLongLong();
            }
            batch.append(row);
            _lastKeyStart = row.startTimeMs;
            _lastKey = row.sessionId;
        }
        if (batch.size() < _pageSize) {
            _exhausted = true;
        }
        return !batch.isEmpty();
    }

    QString error() const { return _error; }

private:
    Database& _database;
    qint64 _startMs = 0;
    qint64 _endMs = 0;
    int _pageSize = 500;
    qint64 _lastKey = 0;
    qint64 _lastKeyStart = 0;
    bool _exhausted = false;
    QString _error;
};

// 临时文件 + 换名：先写同目录临时文件，成功后再替换目标，
// 中途失败不会留下半份文件。
class OutputFile
{
public:
    OutputFile(const QString& target)
        : _target(target)
        , _temp(target + QStringLiteral(".tmp-%1").arg(QCoreApplication::applicationPid()))
    {
    }

    ~OutputFile()
    {
        if (!_published && _tempCreated) {
            QFile::remove(_temp);
        }
    }

    QString tempPath() const { return _temp; }
    QString error() const { return _error; }

    // 写入前校验目标路径（父目录可建、可写），再建临时文件。
    bool begin()
    {
        const QString trimmed = _target.trimmed();
        if (trimmed.isEmpty()) {
            _error = QStringLiteral("导出路径为空");
            return false;
        }
        const QFileInfo info(trimmed);
        if (info.fileName().isEmpty()) {
            _error = QStringLiteral("导出路径缺少文件名：%1").arg(trimmed);
            return false;
        }
        // 目标本身不能是目录。
        if (info.isDir()) {
            _error = QStringLiteral("目标路径是目录：%1").arg(QDir::toNativeSeparators(trimmed));
            return false;
        }
        const QDir parent = info.absoluteDir();
        if (!parent.exists() && !QDir().mkpath(parent.absolutePath())) {
            _error = QStringLiteral("无法创建导出目录：%1").arg(parent.absolutePath());
            return false;
        }

        QFile::remove(_temp);
        _tempCreated = true;
        return true;
    }

    // 临时文件 → 目标。覆盖同名文件（Windows rename 不覆盖，先删）。
    bool publish()
    {
        if (QFile::exists(_target) && !QFile::remove(_target)) {
            _error = QStringLiteral("无法覆盖已有文件：%1").arg(QDir::toNativeSeparators(_target));
            return false;
        }
        if (!QFile::rename(_temp, _target)) {
            _error = QStringLiteral("无法写入导出文件：%1").arg(QDir::toNativeSeparators(_target));
            return false;
        }
        _published = true;
        return true;
    }

private:
    QString _target;
    QString _temp;
    bool _tempCreated = false;
    bool _published = false;
    QString _error;
};

} // namespace

QVector<ExportFieldSpec> exportFieldSpecs()
{
    return QVector<ExportFieldSpec>(fieldTable(), fieldTable() + kFieldCount);
}

QStringList defaultExportFields()
{
    // 默认列：去掉网页活动相关列之后的全集。
    return QStringList{
        QStringLiteral("record_type"), QStringLiteral("category"),
        QStringLiteral("start_time"),  QStringLiteral("end_time"),
        QStringLiteral("duration_ms"), QStringLiteral("app_name"),
        QStringLiteral("exe_name"),    QStringLiteral("window_title"),
    };
}

Result<QStringList> resolveExportFields(const QStringList& selected)
{
    if (selected.isEmpty()) {
        return fail<QStringList>(QStringLiteral("至少选择一个导出字段"));
    }

    QStringList ordered;
    for (const QString& name : selected) {
        if (fieldIndex(name) < 0) {
            return fail<QStringList>(QStringLiteral("未知的导出字段：%1").arg(name));
        }
        if (!ordered.contains(name)) {
            ordered.append(name);
        }
    }
    return succeed(ordered);
}

QString ExportService::defaultExportFileName(const QString& format, qint64 nowMs)
{
    const QDateTime now = QDateTime::fromMSecsSinceEpoch(nowMs);
    const QString stamp = now.toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString extension = format == QLatin1String("csv")
        ? QStringLiteral("csv")
        : format == QLatin1String("markdown") ? QStringLiteral("md") : QStringLiteral("db");
    return QStringLiteral("UseTrack-export-%1.%2").arg(stamp, extension);
}

Result<ExportReport> ExportService::exportTo(const QString& format, const QString& targetPath,
                                             qint64 startMs, qint64 endMs,
                                             const QStringList& selectedFields) const
{
    if (!_database.isOpen()) {
        return fail<ExportReport>(QStringLiteral("数据库未打开"));
    }
    if (startMs > 0 && endMs > 0 && startMs >= endMs) {
        return fail<ExportReport>(QStringLiteral("导出开始时间必须早于结束时间"));
    }

    const Result<QStringList> resolved = resolveExportFields(selectedFields);
    if (!resolved.ok()) {
        return fail<ExportReport>(resolved.error);
    }
    const QStringList fields = resolved.value;

    if (format != QLatin1String("csv") && format != QLatin1String("markdown")
        && format != QLatin1String("sqlite")) {
        return fail<ExportReport>(QStringLiteral("不支持的导出格式：%1").arg(format));
    }

    OutputFile output(targetPath);
    if (!output.begin()) {
        return fail<ExportReport>(output.error());
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    SessionCursor cursor(_database, startMs, endMs, 500);
    ExportReport report;
    report.targetPath = targetPath;

    if (format == QLatin1String("sqlite")) {
        // —— 结构化单库：新开一条到临时文件的连接，建表 + 流式插入 ——
        const QString outputDbName =
            QStringLiteral("patina-export-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        {
            QSqlDatabase out = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), outputDbName);
            out.setDatabaseName(output.tempPath());
            if (!out.open()) {
                const QString message = out.lastError().text();
                QSqlDatabase::removeDatabase(outputDbName);
                return fail<ExportReport>(QStringLiteral("无法创建导出数据库：%1").arg(message));
            }

            QString columns;
            for (const QString& name : fields) {
                if (!columns.isEmpty()) {
                    columns += QStringLiteral(", ");
                }
                const bool isInteger = name == QLatin1String("duration_ms")
                    || name == QLatin1String("session_id") || name == QLatin1String("start_hour");
                const bool isReal = name == QLatin1String("duration_minutes");
                columns += QStringLiteral("%1 %2").arg(
                    name, isInteger ? QStringLiteral("INTEGER") : isReal ? QStringLiteral("REAL")
                                                                         : QStringLiteral("TEXT"));
            }
            QSqlQuery create(out);
            if (!create.exec(QStringLiteral(
                    "CREATE TABLE sessions (id INTEGER PRIMARY KEY AUTOINCREMENT, %1)").arg(columns))) {
                const QString message = describeError(create);
                out.close();
                QSqlDatabase::removeDatabase(outputDbName);
                return fail<ExportReport>(QStringLiteral("建导出表失败：%1").arg(message));
            }

            QString placeholders;
            for (int index = 0; index < fields.size(); ++index) {
                if (!placeholders.isEmpty()) {
                    placeholders += QStringLiteral(", ");
                }
                placeholders += QStringLiteral("?");
            }
            QSqlQuery insert(out);
            insert.prepare(QStringLiteral("INSERT INTO sessions (%1) VALUES (%2)")
                               .arg(fields.join(QStringLiteral(", ")), placeholders));

            QVector<ExportRow> batch;
            QSqlQuery transaction(out);
            transaction.exec(QStringLiteral("BEGIN"));
            while (cursor.next(batch)) {
                for (const ExportRow& row : batch) {
                    for (int index = 0; index < fields.size(); ++index) {
                        const QString& name = fields.at(index);
                        if (name == QLatin1String("duration_ms")
                            || name == QLatin1String("session_id")
                            || name == QLatin1String("start_hour")) {
                            insert.addBindValue(fieldValue(name, row, nowMs).toLongLong());
                        } else if (name == QLatin1String("duration_minutes")) {
                            insert.addBindValue(fieldValue(name, row, nowMs).toDouble());
                        } else {
                            insert.addBindValue(fieldValue(name, row, nowMs));
                        }
                    }
                    if (!insert.exec()) {
                        const QString message = describeError(insert);
                        QSqlQuery rollback(out);
                        rollback.exec(QStringLiteral("ROLLBACK"));
                        out.close();
                        QSqlDatabase::removeDatabase(outputDbName);
                        return fail<ExportReport>(QStringLiteral("写导出数据失败：%1").arg(message));
                    }
                    ++report.sessionCount;
                }
            }
            if (!cursor.error().isEmpty()) {
                QSqlQuery rollback(out);
                rollback.exec(QStringLiteral("ROLLBACK"));
                out.close();
                QSqlDatabase::removeDatabase(outputDbName);
                return fail<ExportReport>(cursor.error());
            }
            QSqlQuery commit(out);
            commit.exec(QStringLiteral("COMMIT"));
            out.close();
        }
        QSqlDatabase::removeDatabase(outputDbName);
    } else {
        // —— CSV / Markdown：QTextStream 流式写 ——
        // Windows 文本导出口径：UTF-8 + BOM + CRLF。BOM 缺失时 Excel 和
        // 一些按 ANSI 猜编码的编辑器（记事本旧版、部分 IDE 默认值）会把
        // 中文读成乱码；LF 行尾在 Excel 的导入向导里也常被吞掉。
        QFile file(output.tempPath());
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return fail<ExportReport>(
                QStringLiteral("无法创建导出文件：%1").arg(file.errorString()));
        }
        QTextStream stream(&file);
        stream.setEncoding(QStringConverter::Utf8);
        stream.setGenerateByteOrderMark(true);

        const bool isCsv = format == QLatin1String("csv");
        // 统一 CRLF 行尾（Windows 文本口径）。
        const QLatin1String eol("\r\n");

        if (!isCsv) {
            // Markdown 文档头。
            const qint64 rangeStart = startMs > 0 ? startMs : 0;
            stream << "# UseTrack 活动导出" << eol << eol;
            stream << QStringLiteral("- 范围：%1 → %2")
                          .arg(startMs > 0
                                   ? moment(rangeStart).toString(QStringLiteral("yyyy-MM-dd"))
                                   : QStringLiteral("全部"),
                               endMs > 0
                                   ? moment(endMs - 1).toString(QStringLiteral("yyyy-MM-dd"))
                                   : QStringLiteral("至今"))
                   << eol;
            stream << QStringLiteral("- 导出时间：%1").arg(timeText(nowMs)) << eol;
        }

        QStringList headerLabels;
        for (const QString& name : fields) {
            const int index = fieldIndex(name);
            // label 是源文件里的中文字面量（/utf-8 下是 UTF-8 字节）：
            // 必须用 QString::fromUtf8 显式解码。之前包 QLatin1String——
            // Latin-1 没有这些字符，UTF-8 的每个字节被当成一个独立字符
            // （è®°å½ç±»åï¼ï¼è¿å°±æ¯ä¹±ç çæ ¹æºã
            headerLabels.append(QString::fromUtf8(fieldTable()[index].label));
        }
        if (isCsv) {
            QStringList cells;
            for (const QString& label : headerLabels) {
                cells.append(csvCell(label));
            }
            stream << cells.join(QStringLiteral(",")) << eol;
        } else {
            stream << eol << "| " << headerLabels.join(QStringLiteral(" | ")) << " |" << eol;
            for (int index = 0; index < fields.size(); ++index) {
                stream << (index == 0 ? QStringLiteral("| --- ") : QStringLiteral("| --- "));
            }
            stream << "|" << eol;
        }

        // 行：分页读，写完一批丢一批。
        QVector<ExportRow> batch;
        QString currentDay;
        while (cursor.next(batch)) {
            for (const ExportRow& row : batch) {
                if (isCsv) {
                    QStringList cells;
                    for (const QString& name : fields) {
                        cells.append(csvCell(fieldValue(name, row, nowMs)));
                    }
                    stream << cells.join(QStringLiteral(",")) << eol;
                } else {
                    const QString day = moment(row.startTimeMs).date()
                                            .toString(QStringLiteral("yyyy-MM-dd"));
                    if (day != currentDay) {
                        currentDay = day;
                        stream << eol << QStringLiteral("## %1").arg(day) << eol << eol;
                        // Markdown 按天分节，节内重复表头方便单独读。
                        stream << "| " << headerLabels.join(QStringLiteral(" | ")) << " |" << eol << "| ";
                        for (int index = 0; index < fields.size(); ++index) {
                            stream << QStringLiteral("--- | ");
                        }
                        stream << eol;
                    }
                    QStringList cells;
                    for (const QString& name : fields) {
                        cells.append(markdownCell(fieldValue(name, row, nowMs)));
                    }
                    stream << "| " << cells.join(QStringLiteral(" | ")) << " |" << eol;
                }
                ++report.sessionCount;
            }
        }
        if (!cursor.error().isEmpty()) {
            file.close();
            return fail<ExportReport>(cursor.error());
        }
        stream.flush();
        file.close();
        if (file.error() != QFileDevice::NoError) {
            return fail<ExportReport>(
                QStringLiteral("写导出文件失败：%1").arg(file.errorString()));
        }
    }

    if (!output.publish()) {
        return fail<ExportReport>(output.error());
    }
    return succeed(report);
}

} // namespace patina::data
