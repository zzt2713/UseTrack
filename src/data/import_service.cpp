#include "data/import_service.h"

#include "data/app_category.h"
#include "data/database.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QTextStream>
#include <QUuid>
#include <QVariant>

#include <memory>
#include <optional>

namespace patina::data
{
namespace
{

QString describeError(const QSqlQuery& query)
{
    const QSqlError error = query.lastError();
    return error.isValid() ? error.text() : QStringLiteral("SQL 执行失败");
}

constexpr int kPreviewMaxWarnings = 12;
constexpr int kMaxImportRecords = 250000;
constexpr int kScanPageSize = 2000;

// 只读探测连接（与 BackupService 的同名类同构；import 单独持有，
// 不把 backup_service.cpp 的匿名类型公开出去）。
class ProbeConnection
{
public:
    explicit ProbeConnection(const QString& filePath, bool readOnly)
    {
        _name = QStringLiteral("patina-import-probe-%1")
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

    ~ProbeConnection()
    {
        {
            QSqlDatabase database = QSqlDatabase::database(_name, false);
            if (database.isOpen()) {
                database.close();
            }
        }
        QSqlDatabase::removeDatabase(_name);
    }

    ProbeConnection(const ProbeConnection&) = delete;
    ProbeConnection& operator=(const ProbeConnection&) = delete;

    bool isOpen() const { return _open; }
    QString error() const { return _error; }
    QSqlDatabase connection() const { return QSqlDatabase::database(_name, false); }

private:
    QString _name;
    QString _error;
    bool _open = false;
};

bool tableExists(QSqlDatabase connection, const QString& table)
{
    QSqlQuery query(connection);
    query.prepare(QStringLiteral(
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?"));
    query.addBindValue(table);
    if (!query.exec()) {
        return false;
    }
    return query.next();
}

qint64 countRows(QSqlDatabase connection, const QString& table)
{
    QSqlQuery query(connection);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(table)) || !query.next()) {
        return -1;
    }
    return query.value(0).toLongLong();
}

// 文件指纹。大文件分块哈希，不整读进内存。
QString fileFingerprint(const QString& filePath, QString* errorOut)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("无法读取文件：%1").arg(file.errorString());
        }
        return QString();
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    constexpr qint64 kChunk = 1 << 20; // 1MB
    while (!file.atEnd()) {
        hash.addData(file.read(kChunk));
    }
    return QString::fromLatin1(hash.result().toHex());
}

// —— 导入行的统一内部表示 ——
struct ImportRecord
{
    bool isExact = true;      // exact_session 或 hour_bucket
    qint64 startMs = 0;
    std::optional<qint64> endMs;
    qint64 durationMs = 0;
    QString exeName;
    QString appName;
    QString title;
    QString category;
};

QString recordFingerprint(const ImportRecord& record)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QString payload = QStringLiteral("%1|%2|%3|%4|%5")
                                .arg(record.isExact ? QStringLiteral("exact_session")
                                                    : QStringLiteral("hour_bucket"))
                                .arg(record.startMs)
                                .arg(record.endMs.has_value() ? QString::number(*record.endMs)
                                                              : QString())
                                .arg(record.durationMs)
                                .arg(record.exeName.toLower());
    hash.addData(payload.toUtf8());
    if (!record.title.trimmed().isEmpty()) {
        hash.addData(record.title.trimmed().toUtf8());
    }
    return QString::fromLatin1(hash.result().toHex());
}

// —— CSV 解析 ——

// 一行 CSV 的字段切分：处理引号包裹与转义引号。
bool splitCsvLine(const QString& line, QStringList& fields)
{
    fields.clear();
    QString current;
    bool inQuotes = false;
    for (int index = 0; index < line.size(); ++index) {
        const QChar ch = line.at(index);
        if (inQuotes) {
            if (ch == QLatin1Char('"')) {
                if (index + 1 < line.size() && line.at(index + 1) == QLatin1Char('"')) {
                    current += QLatin1Char('"');
                    ++index;
                } else {
                    inQuotes = false;
                }
            } else {
                current += ch;
            }
        } else if (ch == QLatin1Char('"') && current.isEmpty()) {
            inQuotes = true;
        } else if (ch == QLatin1Char(',')) {
            fields.append(current);
            current.clear();
        } else {
            current += ch;
        }
    }
    fields.append(current);
    return !inQuotes;
}

// 时间戳解析。两种来源都要认：
//   - canonical CSV / 数据库：RFC3339（带时区）
//   - 本程序「数据」页导出的报表 CSV：本地时间 "yyyy-MM-dd HH:mm:ss"（无时区）
// 报表格式没有时区信息，按本地时间解释——它本来就是从本地时间格式化出来的。
bool parseImportTime(const QString& value, qint64* msOut)
{
    const QString text = value.trimmed();
    if (text.isEmpty()) {
        return false;
    }
    QDateTime parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (parsed.isValid()) {
        *msOut = parsed.toMSecsSinceEpoch();
        return true;
    }
    parsed = QDateTime::fromString(text, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (parsed.isValid()) {
        parsed.setTimeSpec(Qt::LocalTime);
        *msOut = parsed.toMSecsSinceEpoch();
        return true;
    }
    return false;
}

// CSV 表头 → 列索引。
//
// 两种表头都要认，靠表头内容识别而不是猜：
//   - canonical（外部交换格式，也是本程序导入的原始格式）：
//       record_type,start_time,end_time,duration_ms,exe_name,app_name,title,category
//   - report（本程序「数据」页导出的报表）：
//       record_type,category,start_time,end_time,duration_ms,app_name,exe_name,window_title
// 两者列名、列序、record_type 取值（exact_session vs session）、时间格式都不同。
// 不认这两套的话，用户「导出 CSV 再导入」永远失败。
struct CsvLayout
{
    bool canonical = false;
    int recordType = -1;
    int startTime = -1;
    int endTime = -1;
    int durationMs = -1;
    int exeName = -1;
    int appName = -1;
    int title = -1;
    int category = -1;
};

// 报表 CSV 的表头是中文标签（导出时写的是 fieldTable 的 label，
// 不是字段名），所以两套名字都要认。
const char* const kReportAliases[][2] = {
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

// 按字段名或它的中文标签找列，找不到返回 -1。
int reportColumn(const QStringList& headers, const QString& fieldName)
{
    for (const auto& alias : kReportAliases) {
        if (fieldName != QLatin1String(alias[0])) {
            continue;
        }
        const QString label = QString::fromUtf8(alias[1]);
        const int byName = headers.indexOf(fieldName);
        if (byName >= 0) {
            return byName;
        }
        return headers.indexOf(label);
    }
    return -1;
}

std::optional<CsvLayout> resolveCsvLayout(const QStringList& headers)
{
    const QStringList canonical = {
        QStringLiteral("record_type"), QStringLiteral("start_time"), QStringLiteral("end_time"),
        QStringLiteral("duration_ms"), QStringLiteral("exe_name"),  QStringLiteral("app_name"),
        QStringLiteral("title"),       QStringLiteral("category"),
    };
    if (headers == canonical) {
        CsvLayout layout;
        layout.canonical = true;
        layout.recordType = 0;
        layout.startTime = 1;
        layout.endTime = 2;
        layout.durationMs = 3;
        layout.exeName = 4;
        layout.appName = 5;
        layout.title = 6;
        layout.category = 7;
        return layout;
    }

    // 报表格式（本程序「数据」页导出）：列名可能是英文字段名，也可能是中文标签。
    // 必须同时具备这几个键，否则不是我们导出的文件。
    CsvLayout layout;
    layout.canonical = false;
    layout.recordType = reportColumn(headers, QStringLiteral("record_type"));
    layout.startTime = reportColumn(headers, QStringLiteral("start_time"));
    layout.endTime = reportColumn(headers, QStringLiteral("end_time"));
    layout.durationMs = reportColumn(headers, QStringLiteral("duration_ms"));
    layout.exeName = reportColumn(headers, QStringLiteral("exe_name"));
    layout.appName = reportColumn(headers, QStringLiteral("app_name"));
    layout.title = reportColumn(headers, QStringLiteral("window_title"));
    layout.category = reportColumn(headers, QStringLiteral("category"));
    if (layout.recordType < 0 || layout.startTime < 0 || layout.durationMs < 0
        || layout.exeName < 0) {
        return std::nullopt;
    }
    return layout;
}

struct CsvParseResult
{
    QVector<ImportRecord> records;
    QVector<QPair<int, QString>> errors; // 行号 → 消息
    bool headerOk = false;
};

// 导出的 CSV 会给「以 = + - @ 开头」的单元格加一个前导单引号（防 Excel 公式注入）。
// 导回来时要把这个引号去掉，否则窗口标题会平白多一个 '。
QString stripCsvGuard(const QString& value)
{
    if (value.size() >= 2 && value.at(0) == QLatin1Char('\'')) {
        const QChar next = value.at(1);
        if (next == QLatin1Char('=') || next == QLatin1Char('+') || next == QLatin1Char('-')
            || next == QLatin1Char('@')) {
            return value.mid(1);
        }
    }
    return value;
}

// 分类列的取值有两种来源：canonical CSV 写的是分类 id（如 "development"），
// 本程序导出的报表写的是中文标签（如「开发」）。两种都要能落成 id，
// 否则报表导回来会把「开发」当成一个 category_id 存进 apps。
QString canonicalCategoryId(const QString& value)
{
    const QString text = value.trimmed();
    if (text.isEmpty()) {
        return QString();
    }
    const QVector<CategoryToken> tokens = categoryTokens();
    for (const CategoryToken& token : tokens) {
        if (token.id.compare(text, Qt::CaseInsensitive) == 0) {
            return token.id;
        }
    }
    for (const CategoryToken& token : tokens) {
        if (token.label.compare(text, Qt::CaseInsensitive) == 0) {
            return token.id;
        }
    }
    return QString();
}

// 校验一行已切分的 CSV 单元格。合法时填充 record。
bool validateCsvRow(int lineNumber, const QStringList& cells, const CsvLayout& layout,
                    ImportRecord* recordOut, QString* errorOut)
{
    Q_UNUSED(lineNumber)
    const int needed = qMax(qMax(layout.recordType, layout.startTime),
                            qMax(qMax(layout.endTime, layout.durationMs),
                                 qMax(qMax(layout.exeName, layout.appName),
                                      qMax(layout.title, layout.category))))
                       + 1;
    if (cells.size() < needed) {
        *errorOut = QStringLiteral("列数应为 %1，实际 %2").arg(needed).arg(cells.size());
        return false;
    }

    auto cellAt = [&cells](int index) -> QString {
        return index >= 0 && index < cells.size() ? cells.at(index) : QString();
    };

    ImportRecord record;
    const QString typeText = cellAt(layout.recordType).trimmed();
    if (typeText == QLatin1String("exact_session")) {
        record.isExact = true;
    } else if (typeText == QLatin1String("hour_bucket")) {
        record.isExact = false;
    } else if (typeText == QLatin1String("session")) {
        // 本程序「数据」页导出的报表用的是 "session"，语义与 exact_session 相同。
        record.isExact = true;
    } else {
        *errorOut = QStringLiteral("record_type 必须是 exact_session、hour_bucket 或 session");
        return false;
    }

    if (!parseImportTime(cellAt(layout.startTime), &record.startMs)) {
        *errorOut = QStringLiteral("start_time 必须是 RFC3339 或 yyyy-MM-dd HH:mm:ss 时间");
        return false;
    }
    bool okDuration = false;
    record.durationMs = cellAt(layout.durationMs).trimmed().toLongLong(&okDuration);
    if (!okDuration || record.durationMs <= 0) {
        *errorOut = QStringLiteral("duration_ms 必须是正整数");
        return false;
    }
    const QString endTimeText = cellAt(layout.endTime).trimmed();
    if (!endTimeText.isEmpty()) {
        qint64 endMs = 0;
        if (!parseImportTime(endTimeText, &endMs)) {
            *errorOut = QStringLiteral("end_time 必须是 RFC3339 或 yyyy-MM-dd HH:mm:ss 时间");
            return false;
        }
        record.endMs = endMs;
    }

    if (record.isExact) {
        if (!layout.canonical) {
            // 报表格式的起止时间只到秒，duration_ms 才是权威值：用它重算终点。
            // 否则亚秒级段（1 秒内切走的窗口）会被判成「结束不晚于开始」整条丢掉，
            // 正常段也会因为两个时间戳各自向下截断而贴着容差线。
            // 报表里也可能根本没有 end_time 列（用户只勾了部分字段），
            // 那种情况下同样能靠 start + duration 还原，不该报错。
            record.endMs = record.startMs + record.durationMs;
        } else if (!record.endMs.has_value()) {
            *errorOut = QStringLiteral("exact_session 必须有 end_time");
            return false;
        } else if (*record.endMs <= record.startMs
                   || qAbs((*record.endMs - record.startMs) - record.durationMs) > 1000) {
            *errorOut = QStringLiteral("exact_session 的 duration 与时间范围不符");
            return false;
        }
    } else {
        if (record.endMs.has_value()) {
            *errorOut = QStringLiteral("hour_bucket 不能有 end_time");
            return false;
        }
        if (record.durationMs > 3600000) {
            *errorOut = QStringLiteral("hour_bucket 时长不能超过一小时");
            return false;
        }
        if (!cellAt(layout.title).trimmed().isEmpty()) {
            *errorOut = QStringLiteral("hour_bucket 的 title 必须为空");
            return false;
        }
    }

    record.exeName = cellAt(layout.exeName).trimmed().toLower();
    if (record.exeName.isEmpty()) {
        *errorOut = QStringLiteral("exe_name 不能为空");
        return false;
    }
    // canonical 格式要求 .exe 结尾；报表格式不强制——
    // 本程序的库里有 AUMID 形态的 UWP 应用名，它们同样要能导回来。
    if (layout.canonical && !record.exeName.endsWith(QLatin1String(".exe"))) {
        *errorOut = QStringLiteral("exe_name 必须以 .exe 结尾");
        return false;
    }
    record.appName = stripCsvGuard(cellAt(layout.appName).trimmed());
    record.title = stripCsvGuard(cellAt(layout.title).trimmed());
    record.category = canonicalCategoryId(cellAt(layout.category));
    *recordOut = record;
    return true;
}

CsvParseResult parseCanonicalCsv(const QString& filePath)
{
    CsvParseResult result;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.errors.append({0, QStringLiteral("无法读取文件：%1").arg(file.errorString())});
        return result;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);

    const QStringList required = {
        QStringLiteral("record_type"), QStringLiteral("start_time"), QStringLiteral("end_time"),
        QStringLiteral("duration_ms"), QStringLiteral("exe_name"),  QStringLiteral("app_name"),
        QStringLiteral("title"),       QStringLiteral("category"),
    };

    if (stream.atEnd()) {
        result.errors.append({1, QStringLiteral("文件是空的")});
        return result;
    }
    QString headerLine = stream.readLine();
    if (headerLine.startsWith(QChar(0xFEFF))) {
        headerLine.remove(0, 1);
    }
    QStringList headers;
    splitCsvLine(headerLine, headers);
    for (QString& header : headers) {
        header = header.trimmed();
    }
    const std::optional<CsvLayout> layout = resolveCsvLayout(headers);
    if (!layout.has_value()) {
        result.errors.append(
            {1, QStringLiteral("CSV 列必须是 %1（或本程序导出的报表列）")
                    .arg(required.join(QStringLiteral(",")))});
        return result;
    }
    result.headerOk = true;

    int lineNumber = 1;
    while (!stream.atEnd()) {
        ++lineNumber;
        const QString line = stream.readLine();
        if (line.trimmed().isEmpty()) {
            continue;
        }
        if (result.records.size() + result.errors.size() >= kMaxImportRecords) {
            result.errors.append({lineNumber, QStringLiteral("超出 %1 条记录安全上限")
                                                    .arg(kMaxImportRecords)});
            break;
        }

        QStringList cells;
        if (!splitCsvLine(line, cells)) {
            result.errors.append({lineNumber, QStringLiteral("CSV 行引号未闭合")});
            continue;
        }

        ImportRecord record;
        QString error;
        if (validateCsvRow(lineNumber, cells, *layout, &record, &error)) {
            result.records.append(record);
        } else {
            result.errors.append({lineNumber, error});
        }
    }

    return result;
}

// —— 外部数据库扫描（反规范化 sessions → ImportRecord）——

struct DbScanResult
{
    QVector<ImportRecord> records;
    QVector<QPair<int, QString>> warnings;
    qint64 scanned = 0;
    bool hasTitleSamples = false;
    qint64 minStartMs = 0;
    qint64 maxEndMs = 0;
};

// 时间戳解析：源库存 INTEGER 毫秒，兼容秒级旧数据（< 1e12 视为秒）。
qint64 normalizeSourceTimestamp(qint64 raw)
{
    if (raw > 0 && raw < 1000000000000LL) {
        return raw * 1000;
    }
    return raw;
}

// 源库 sessions 表的两种形态：
//   - rust：旧版 Rust 程序的库，列是 start_time/end_time/duration（INTEGER 毫秒）
//   - report：本程序「数据」页导出的 SQLite 快照，列是 start_time/end_time/
//     duration_ms（TEXT，格式 yyyy-MM-dd HH:mm:ss），还带 record_type
// 不区分的话，用户「导出 SQLite 再导入」会撞上「no such column: duration」。
enum class DbLayout { Rust, Report };

DbLayout detectDbLayout(QSqlDatabase connection)
{
    QSqlQuery query(connection);
    if (!query.exec(QStringLiteral("PRAGMA table_info(sessions)"))) {
        return DbLayout::Rust;
    }
    while (query.next()) {
        // 第 2 列是列名。
        if (query.value(1).toString().compare(QLatin1String("duration_ms"), Qt::CaseInsensitive) == 0) {
            return DbLayout::Report;
        }
    }
    return DbLayout::Rust;
}

// 报表快照的一行 → ImportRecord。失败返回空（附原因）。
std::optional<ImportRecord> reportRowToRecord(const QVariant& typeValue, const QVariant& startValue,
                                              const QVariant& endValue,
                                              const QVariant& durationValue,
                                              const QString& exeName, const QString& appName,
                                              const QString& title, const QString& category)
{
    const QString typeText = typeValue.toString().trimmed();
    const bool isExact = typeText.compare(QLatin1String("hour_bucket"), Qt::CaseInsensitive) != 0;

    qint64 startMs = 0;
    if (startValue.isNull() || !parseImportTime(startValue.toString(), &startMs)) {
        return std::nullopt;
    }
    bool okDuration = false;
    qint64 durationMs = durationValue.toString().trimmed().toLongLong(&okDuration);
    if (!okDuration || durationMs <= 0) {
        return std::nullopt;
    }

    ImportRecord record;
    record.isExact = isExact;
    record.startMs = startMs;
    record.durationMs = durationMs;
    if (!endValue.isNull() && !endValue.toString().trimmed().isEmpty()) {
        qint64 endMs = 0;
        if (!parseImportTime(endValue.toString(), &endMs)) {
            return std::nullopt;
        }
        record.endMs = endMs;
    }
    if (!isExact && record.endMs.has_value()) {
        return std::nullopt;
    }
    if (isExact) {
        // 报表快照里的起止时间只到秒，duration_ms 才是权威值：用它重算终点，
        // 亚秒级段（1 秒内切走的窗口）才不会被判成「结束不晚于开始」丢掉。
        record.endMs = record.startMs + record.durationMs;
    }

    record.exeName = exeName.trimmed().toLower();
    if (record.exeName.isEmpty()) {
        return std::nullopt;
    }
    record.appName = stripCsvGuard(appName.trimmed());
    record.title = stripCsvGuard(title.trimmed());
    record.category = canonicalCategoryId(category);
    return record;
}

DbScanResult scanRustDatabase(const QString& filePath)
{
    DbScanResult result;
    qint64 minStart = 0;
    qint64 maxEnd = 0;

    ProbeConnection probe(filePath, true);
    if (!probe.isOpen()) {
        result.warnings.append(
            {0, QStringLiteral("无法打开数据库（不是有效的 SQLite 文件）：%1").arg(probe.error())});
        return result;
    }
    const QSqlDatabase connection = probe.connection();

    if (!tableExists(connection, QStringLiteral("sessions"))) {
        result.warnings.append({0, QStringLiteral("这不是本程序的数据库（找不到 sessions 表）")});
        return result;
    }

    // 完整性检查（导入前先确认文件没坏）。
    {
        QSqlQuery check(connection);
        if (!check.exec(QStringLiteral("PRAGMA integrity_check")) || !check.next()
            || check.value(0).toString() != QLatin1String("ok")) {
            result.warnings.append({0, QStringLiteral("数据库完整性检查未通过，拒绝导入")});
            return result;
        }
    }

    const qint64 total = qMax<qint64>(0, countRows(connection, QStringLiteral("sessions")));
    if (total > kMaxImportRecords) {
        result.warnings.append({0, QStringLiteral("源库活动段超过 %1 条安全上限").arg(kMaxImportRecords)});
        return result;
    }

    const DbLayout layout = detectDbLayout(connection);

    QSqlQuery query(connection);
    // 外部库列：id, app_name, exe_name, window_title, start_time, end_time, duration,
    // continuity_group_start_time。逐页 keyset 扫描。
    // 报表快照列：id, record_type, category, start_time, end_time, duration_ms,
    // app_name, exe_name, window_title（时间与时长都是 TEXT）。
    qint64 lastId = 0;
    bool exhausted = false;
    int pageNumber = 0;
    while (!exhausted) {
        query.prepare(layout == DbLayout::Rust
                          ? QStringLiteral("SELECT id, app_name, exe_name, window_title, "
                                           "start_time, end_time, duration "
                                           "FROM sessions WHERE id > ? ORDER BY id ASC LIMIT ?")
                          : QStringLiteral("SELECT id, record_type, category, start_time, "
                                           "end_time, duration_ms, app_name, exe_name, "
                                           "window_title "
                                           "FROM sessions WHERE id > ? ORDER BY id ASC LIMIT ?"));
        query.addBindValue(lastId);
        query.addBindValue(kScanPageSize);
        if (!query.exec()) {
            result.warnings.append({pageNumber, describeError(query)});
            return result;
        }
        int fetched = 0;
        while (query.next()) {
            ++fetched;
            ++result.scanned;
            const qint64 id = query.value(0).toLongLong();
            lastId = id;

            if (layout == DbLayout::Report) {
                const std::optional<ImportRecord> converted = reportRowToRecord(
                    query.value(1), query.value(3), query.value(4), query.value(5),
                    query.value(7).toString(), query.value(6).toString(),
                    query.value(8).toString(), query.value(2).toString());
                if (!converted.has_value()) {
                    result.warnings.append(
                        {static_cast<int>(id), QStringLiteral("行数据不完整，跳过")});
                    continue;
                }
                const ImportRecord& record = *converted;
                if (minStart == 0 || record.startMs < minStart) {
                    minStart = record.startMs;
                }
                const qint64 rowEnd = record.endMs.value_or(record.startMs + record.durationMs);
                if (rowEnd > maxEnd) {
                    maxEnd = rowEnd;
                }
                result.records.append(record);
                continue;
            }

            const qint64 startMs = normalizeSourceTimestamp(query.value(4).toLongLong());
            const QVariant endValue = query.value(5);
            const QVariant durationValue = query.value(6);
            const QString exeName = query.value(2).toString().trimmed().toLower();

            if (exeName.isEmpty()) {
                result.warnings.append({static_cast<int>(id), QStringLiteral("exe_name 为空，跳过")});
                continue;
            }
            if (endValue.isNull()) {
                // 未闭合段：源库只会把除最后一条之外的未闭合段封口。
                // 导入时未闭合段没有意义，跳过。
                result.warnings.append({static_cast<int>(id), QStringLiteral("未闭合段，跳过")});
                continue;
            }
            const qint64 endMs = normalizeSourceTimestamp(endValue.toLongLong());
            if (endMs <= startMs) {
                result.warnings.append({static_cast<int>(id), QStringLiteral("结束时间不晚于开始，跳过")});
                continue;
            }

            ImportRecord record;
            record.isExact = true;
            record.startMs = startMs;
            record.endMs = endMs;
            record.durationMs = durationValue.isNull() ? (endMs - startMs)
                                                       : qMax<qint64>(0, durationValue.toLongLong());
            if (record.durationMs == 0) {
                record.durationMs = endMs - startMs;
            }
            record.exeName = exeName;
            record.appName = query.value(1).toString().trimmed();
            record.title = query.value(3).toString().trimmed();
            if (minStart == 0 || startMs < minStart) {
                minStart = startMs;
            }
            if (endMs > maxEnd) {
                maxEnd = endMs;
            }
            result.records.append(record);
        }
        if (fetched < kScanPageSize) {
            exhausted = true;
        }
        ++pageNumber;
    }

    result.hasTitleSamples = tableExists(connection, QStringLiteral("session_title_samples"));
    result.minStartMs = minStart;
    result.maxEndMs = maxEnd;
    return result;
}

} // namespace

Result<ImportPreview> ImportService::preview(const QString& filePath) const
{
    const QString path = filePath.trimmed();
    if (path.isEmpty()) {
        return fail<ImportPreview>(QStringLiteral("还没有选择导入文件"));
    }
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return fail<ImportPreview>(QStringLiteral("文件不存在：%1").arg(QDir::toNativeSeparators(path)));
    }

    const QString suffix = info.suffix().toLower();
    const bool isCsv = suffix == QLatin1String("csv");
    const bool isDb = suffix == QLatin1String("db") || suffix == QLatin1String("sqlite");
    if (!isCsv && !isDb) {
        return fail<ImportPreview>(QStringLiteral("只支持 .db / .sqlite（Rust 版数据库）或 .csv 文件"));
    }

    ImportPreview preview;
    preview.filePath = QDir::toNativeSeparators(info.absoluteFilePath());

    QString fingerprintError;
    preview.fingerprint = fileFingerprint(path, &fingerprintError);
    if (preview.fingerprint.isEmpty()) {
        return fail<ImportPreview>(fingerprintError);
    }

    QVector<QPair<int, QString>> problems;
    qint64 exact = 0;
    qint64 buckets = 0;
    qint64 minStart = 0;
    qint64 maxEnd = 0;

    if (isCsv) {
        preview.sourceKind = QStringLiteral("patina-csv");
        const CsvParseResult parsed = parseCanonicalCsv(path);
        problems = parsed.errors;
        for (const ImportRecord& record : parsed.records) {
            if (record.isExact) {
                ++exact;
            } else {
                ++buckets;
            }
            if (minStart == 0 || record.startMs < minStart) {
                minStart = record.startMs;
            }
            const qint64 rowEnd = record.endMs.value_or(record.startMs + record.durationMs);
            if (rowEnd > maxEnd) {
                maxEnd = rowEnd;
            }
        }
    } else {
        preview.sourceKind = QStringLiteral("patina-db");
        const DbScanResult scanned = scanRustDatabase(path);
        problems = scanned.warnings;
        exact = static_cast<qint64>(scanned.records.size());
        minStart = scanned.minStartMs;
        maxEnd = scanned.maxEndMs;
        // 全是精确段；标题样本是否存在提示给用户。
        if (scanned.hasTitleSamples) {
            problems.append({0, QStringLiteral("检测到 session_title_samples 表，将一并迁移")});
        }
    }

    preview.validRecords = exact + buckets;
    preview.exactSessions = exact;
    preview.hourBuckets = buckets;
    preview.minStartTimeMs = minStart;
    preview.maxEndTimeMs = maxEnd;
    preview.errorRecords = problems.size();
    for (int index = 0; index < problems.size() && index < kPreviewMaxWarnings; ++index) {
        const QPair<int, QString>& problem = problems.at(index);
        preview.warnings.append(
            problem.first > 0
                ? QStringLiteral("行 %1：%2").arg(problem.first).arg(problem.second)
                : problem.second);
    }

    if (preview.validRecords == 0 && problems.isEmpty()) {
        preview.warnings.append(QStringLiteral("文件里没有可导入的记录"));
    }
    return succeed(preview);
}

namespace
{

// 提交阶段的流式记录游标：与 preview 同源同序，逐批产出。
class RecordCursor
{
public:
    RecordCursor(const QString& path, bool isCsv) : _path(path), _isCsv(isCsv) {}

    // 取下一批。返回 false 表示读完；error() 非空表示中途失败。
    bool next(QVector<ImportRecord>& batch)
    {
        batch.clear();
        if (_error.isEmpty() && _exhausted) {
            return false;
        }

        if (_isCsv) {
            // CSV：流式逐行读（不整文件进内存）。首次调用校验表头。
            if (!_file) {
                _file = std::make_unique<QFile>(_path);
                if (!_file->open(QIODevice::ReadOnly)) {
                    _error = QStringLiteral("无法读取文件：%1").arg(_file->errorString());
                    _file.reset();
                    return false;
                }
                _stream = std::make_unique<QTextStream>(_file.get());
                _stream->setEncoding(QStringConverter::Utf8);

                if (_stream->atEnd()) {
                    _error = QStringLiteral("文件是空的");
                    return false;
                }
                QString headerLine = _stream->readLine();
                if (headerLine.startsWith(QChar(0xFEFF))) {
                    headerLine.remove(0, 1);
                }
                QStringList headers;
                splitCsvLine(headerLine, headers);
                for (QString& header : headers) {
                    header = header.trimmed();
                }
                const QStringList required = {
                    QStringLiteral("record_type"), QStringLiteral("start_time"),
                    QStringLiteral("end_time"),    QStringLiteral("duration_ms"),
                    QStringLiteral("exe_name"),    QStringLiteral("app_name"),
                    QStringLiteral("title"),       QStringLiteral("category"),
                };
                const std::optional<CsvLayout> layout = resolveCsvLayout(headers);
                if (!layout.has_value()) {
                    _error = QStringLiteral("CSV 列必须是 %1（或本程序导出的报表列）")
                                 .arg(required.join(QStringLiteral(",")));
                    return false;
                }
                _layout = *layout;
            }

            while (!_stream->atEnd() && batch.size() < kScanPageSize) {
                ++_lineNumber;
                const QString line = _stream->readLine();
                if (line.trimmed().isEmpty()) {
                    continue;
                }
                if (_seen + batch.size() >= kMaxImportRecords) {
                    _error = QStringLiteral("超出 %1 条记录安全上限").arg(kMaxImportRecords);
                    return false;
                }
                QStringList cells;
                if (!splitCsvLine(line, cells)) {
                    ++_skipped;
                    continue;
                }
                ImportRecord record;
                QString rowError;
                if (!validateCsvRow(_lineNumber, cells, _layout, &record, &rowError)) {
                    ++_skipped; // 行级问题按预览的 warning 语义跳过（计数，不中断）。
                    continue;
                }
                batch.append(record);
            }
            _seen += batch.size();
            if (_stream->atEnd()) {
                _exhausted = true;
            }
            return !batch.isEmpty();
        }

        // db 源：keyset 分页。
        if (!_probe || !_probe->isOpen()) {
            _probe = std::make_unique<ProbeConnection>(_path, true);
            if (!_probe->isOpen()) {
                _error = QStringLiteral("无法打开源数据库：%1").arg(_probe->error());
                return false;
            }
            QSqlQuery check(_probe->connection());
            if (!check.exec(QStringLiteral("PRAGMA integrity_check")) || !check.next()
                || check.value(0).toString() != QLatin1String("ok")) {
                _error = QStringLiteral("数据库完整性检查未通过");
                return false;
            }
            _dbLayout = detectDbLayout(_probe->connection());
        }

        QSqlQuery query(_probe->connection());
        query.prepare(_dbLayout == DbLayout::Rust
                          ? QStringLiteral("SELECT id, app_name, exe_name, window_title, "
                                           "start_time, end_time, duration "
                                           "FROM sessions WHERE id > ? ORDER BY id ASC LIMIT ?")
                          : QStringLiteral("SELECT id, record_type, category, start_time, "
                                           "end_time, duration_ms, app_name, exe_name, "
                                           "window_title "
                                           "FROM sessions WHERE id > ? ORDER BY id ASC LIMIT ?"));
        query.addBindValue(_lastId);
        query.addBindValue(kScanPageSize);
        if (!query.exec()) {
            _error = describeError(query);
            return false;
        }
        int fetched = 0;
        while (query.next()) {
            ++fetched;
            const qint64 id = query.value(0).toLongLong();
            _lastId = id;

            if (_dbLayout == DbLayout::Report) {
                const std::optional<ImportRecord> converted = reportRowToRecord(
                    query.value(1), query.value(3), query.value(4), query.value(5),
                    query.value(7).toString(), query.value(6).toString(),
                    query.value(8).toString(), query.value(2).toString());
                if (!converted.has_value()) {
                    ++_skipped;
                    continue;
                }
                batch.append(*converted);
                continue;
            }

            const qint64 startMs = normalizeSourceTimestamp(query.value(4).toLongLong());
            const QVariant endValue = query.value(5);
            const QString exeName = query.value(2).toString().trimmed().toLower();
            if (exeName.isEmpty() || endValue.isNull()) {
                continue;
            }
            const qint64 endMs = normalizeSourceTimestamp(endValue.toLongLong());
            if (endMs <= startMs) {
                continue;
            }
            ImportRecord record;
            record.isExact = true;
            record.startMs = startMs;
            record.endMs = endMs;
            const QVariant durationValue = query.value(6);
            record.durationMs = durationValue.isNull() ? (endMs - startMs)
                                                       : qMax<qint64>(0, durationValue.toLongLong());
            if (record.durationMs == 0) {
                record.durationMs = endMs - startMs;
            }
            record.exeName = exeName;
            record.appName = query.value(1).toString().trimmed();
            record.title = query.value(3).toString().trimmed();
            batch.append(record);
        }
        if (fetched < kScanPageSize) {
            _exhausted = true;
        }
        return !batch.isEmpty();
    }

    QString error() const { return _error; }
    qint64 skippedRows() const { return _skipped; }

private:
    QString _path;
    bool _isCsv = false;
    bool _exhausted = false;
    qint64 _lastId = 0;
    qint64 _seen = 0;
    qint64 _skipped = 0;
    int _lineNumber = 1;
    CsvLayout _layout;
    DbLayout _dbLayout = DbLayout::Rust;
    std::unique_ptr<QFile> _file;
    std::unique_ptr<QTextStream> _stream;
    std::unique_ptr<ProbeConnection> _probe;
    QString _error;
};

} // namespace

Result<ImportCommitReport> ImportService::commit(const QString& filePath,
                                                 const QString& expectedFingerprint)
{
    if (!_database.isOpen()) {
        return fail<ImportCommitReport>(QStringLiteral("数据库未打开"));
    }

    // 预览-提交一致性：文件必须与预览时是同一份，变了要重新预览。
    QString fingerprintError;
    const QString actual = fileFingerprint(filePath.trimmed(), &fingerprintError);
    if (actual.isEmpty()) {
        return fail<ImportCommitReport>(fingerprintError);
    }
    if (actual != expectedFingerprint) {
        return fail<ImportCommitReport>(QStringLiteral("导入文件与预览时不一致，请重新预览"));
    }

    const QFileInfo info(filePath.trimmed());
    const bool isCsv = info.suffix().toLower() == QLatin1String("csv");

    ImportCommitReport report;
    const QString sourceKind = isCsv ? QStringLiteral("patina-csv") : QStringLiteral("patina-db");

    RecordCursor cursor(info.absoluteFilePath(), isCsv);

    // —— 单事务提交：批次行 + 段/桶 + 应用匹配，全部原子 ——
    Transaction transaction(_database);
    if (!transaction.isActive()) {
        return fail<ImportCommitReport>(QStringLiteral("无法开启导入事务"));
    }

    const QSqlDatabase connection = _database.connection();

    const qint64 importedAtMs = QDateTime::currentMSecsSinceEpoch();
    const QString batchId = QStringLiteral("import-%1-%2")
                                .arg(QString::fromLatin1(
                                         QCryptographicHash::hash(actual.toUtf8(), QCryptographicHash::Sha256)
                                             .toHex()
                                             .left(12)),
                                    QString::number(importedAtMs));

    // 已知行级指纹（导入去重）。用 SQLite 端集合而不是哈希表：
    // 大批次下指纹集合可能很大，直接在插入时靠 UNIQUE 约束幂等，
    // 重复行用 INSERT OR IGNORE 语义跳过。这里用逐条 INSERT 检查
    // lastInsertId 失效——换成 OR IGNORE 后按 changes 计数。
    QSqlQuery insertBatch(connection);
    insertBatch.prepare(QStringLiteral(
        "INSERT INTO import_batches (id, imported_at_ms, source_name, source_kind, "
        "source_fingerprint, exact_session_count, bucket_count) VALUES (?, ?, ?, ?, ?, 0, 0)"));
    insertBatch.addBindValue(batchId);
    insertBatch.addBindValue(importedAtMs);
    insertBatch.addBindValue(info.fileName());
    insertBatch.addBindValue(sourceKind);
    insertBatch.addBindValue(actual);
    if (!insertBatch.exec()) {
        return fail<ImportCommitReport>(
            QStringLiteral("创建导入批次失败：%1").arg(describeError(insertBatch)));
    }

    // 应用匹配缓存：exe → app_id（含首次见到的时间范围）。
    QHash<QString, qint64> appIds;

    auto resolveApp = [&](const ImportRecord& record, qint64* idOut) -> Status {
        const QString key = record.exeName.toLower();
        const auto cached = appIds.constFind(key);
        if (cached != appIds.constEnd()) {
            *idOut = cached.value();
            return Status::success();
        }

        QSqlQuery find(connection);
        find.prepare(QStringLiteral("SELECT id FROM apps WHERE exe_name = ? COLLATE NOCASE"));
        find.addBindValue(record.exeName);
        if (!find.exec()) {
            return Status::failure(describeError(find));
        }
        if (find.next()) {
            *idOut = find.value(0).toLongLong();
            appIds.insert(key, *idOut);

            // 显示名只在空的时候补（用户改过的不覆盖，与 ensureDisplayName 同口径）。
            if (!record.appName.isEmpty()) {
                QSqlQuery name(connection);
                name.prepare(QStringLiteral(
                    "UPDATE apps SET display_name = ? "
                    "WHERE id = ? AND (display_name IS NULL OR display_name = '')"));
                name.addBindValue(record.appName);
                name.addBindValue(*idOut);
                if (!name.exec()) {
                    return Status::failure(describeError(name));
                }
            }
            // 分类映射：源 CSV 带 category 且目标应用还没有覆盖项时写入。
            if (!record.category.isEmpty()) {
                QSqlQuery cat(connection);
                cat.prepare(QStringLiteral(
                    "UPDATE apps SET category_id = ? "
                    "WHERE id = ? AND (category_id IS NULL OR category_id = '')"));
                cat.addBindValue(record.category);
                cat.addBindValue(*idOut);
                if (!cat.exec()) {
                    return Status::failure(describeError(cat));
                }
            }
            return Status::success();
        }

        // 不存在：建新应用行。
        QSqlQuery create(connection);
        create.prepare(QStringLiteral(
            "INSERT INTO apps (exe_name, exe_path, display_name, category_id, "
            "first_seen_ms, last_seen_ms) VALUES (?, '', ?, ?, ?, ?)"));
        create.addBindValue(record.exeName);
        create.addBindValue(record.appName);
        create.addBindValue(record.category);
        create.addBindValue(record.startMs);
        create.addBindValue(record.startMs);
        if (!create.exec()) {
            return Status::failure(describeError(create));
        }
        *idOut = create.lastInsertId().toLongLong();
        appIds.insert(key, *idOut);
        return Status::success();
    };

    QVector<ImportRecord> batch;
    while (cursor.next(batch)) {
        for (const ImportRecord& record : batch) {
            if (record.isExact) {
                qint64 appId = 0;
                const Status appStatus = resolveApp(record, &appId);
                if (!appStatus.ok()) {
                    return fail<ImportCommitReport>(appStatus.error);
                }

                const QString fingerprint = recordFingerprint(record);
                // 幂等：指纹已存在则跳过（不报错，计入 duplicate）。
                QSqlQuery exists(connection);
                exists.prepare(QStringLiteral(
                    "SELECT 1 FROM import_batch_sessions WHERE fingerprint = ?"));
                exists.addBindValue(fingerprint);
                if (!exists.exec()) {
                    return fail<ImportCommitReport>(describeError(exists));
                }
                if (exists.next()) {
                    ++report.duplicateRecords;
                    continue;
                }

                // 单活跃段约束：导入段全部已闭合，不需要 open 槽位；
                // 但 app_id 外键要求应用存在（上面已建/匹配）。
                QSqlQuery insertSession(connection);
                insertSession.prepare(QStringLiteral(
                    "INSERT INTO sessions (app_id, window_title, start_time_ms, end_time_ms, "
                    "continuity_group_start_time_ms) VALUES (?, ?, ?, ?, ?)"));
                insertSession.addBindValue(appId);
                // 空标题要落成空串：null QString 会被绑成 SQL NULL，
                // 撞上 window_title 的 NOT NULL 约束，整批导入都会失败。
                insertSession.addBindValue(record.title.isNull() ? QStringLiteral("")
                                                                 : record.title);
                insertSession.addBindValue(record.startMs);
                insertSession.addBindValue(*record.endMs);
                insertSession.addBindValue(record.startMs);
                if (!insertSession.exec()) {
                    return fail<ImportCommitReport>(
                        QStringLiteral("写入导入段失败：%1").arg(describeError(insertSession)));
                }
                const qint64 sessionId = insertSession.lastInsertId().toLongLong();

                QSqlQuery link(connection);
                link.prepare(QStringLiteral(
                    "INSERT INTO import_batch_sessions (batch_id, session_id, fingerprint, source_category) "
                    "VALUES (?, ?, ?, ?)"));
                link.addBindValue(batchId);
                link.addBindValue(sessionId);
                link.addBindValue(fingerprint);
                link.addBindValue(record.category.isEmpty()
                                      ? QVariant(QMetaType::fromType<QString>())
                                      : QVariant(record.category));
                if (!link.exec()) {
                    return fail<ImportCommitReport>(
                        QStringLiteral("登记导入段失败：%1").arg(describeError(link)));
                }
                ++report.exactSessions;
                ++report.importedRecords;
            } else {
                const QString fingerprint = recordFingerprint(record);
                QSqlQuery exists(connection);
                exists.prepare(QStringLiteral(
                    "SELECT 1 FROM import_time_buckets WHERE fingerprint = ?"));
                exists.addBindValue(fingerprint);
                if (!exists.exec()) {
                    return fail<ImportCommitReport>(describeError(exists));
                }
                if (exists.next()) {
                    ++report.duplicateRecords;
                    continue;
                }

                QSqlQuery insertBucket(connection);
                insertBucket.prepare(QStringLiteral(
                    "INSERT INTO import_time_buckets (batch_id, fingerprint, app_name, exe_name, "
                    "bucket_start_ms, duration_ms, source_category) VALUES (?, ?, ?, ?, ?, ?, ?)"));
                insertBucket.addBindValue(batchId);
                insertBucket.addBindValue(fingerprint);
                insertBucket.addBindValue(
                    record.appName.isEmpty() ? record.exeName : record.appName);
                insertBucket.addBindValue(record.exeName);
                insertBucket.addBindValue(record.startMs);
                insertBucket.addBindValue(record.durationMs);
                insertBucket.addBindValue(record.category.isEmpty()
                                              ? QVariant(QMetaType::fromType<QString>())
                                              : QVariant(record.category));
                if (!insertBucket.exec()) {
                    return fail<ImportCommitReport>(
                        QStringLiteral("写入小时桶失败：%1").arg(describeError(insertBucket)));
                }
                ++report.hourBuckets;
                ++report.importedRecords;
            }
        }
        if (report.importedRecords + report.duplicateRecords > kMaxImportRecords) {
            return fail<ImportCommitReport>(
                QStringLiteral("超出 %1 条记录安全上限").arg(kMaxImportRecords));
        }
    }
    if (!cursor.error().isEmpty()) {
        return fail<ImportCommitReport>(cursor.error());
    }

    // 没有新数据：整体回滚，不留空批次。
    if (report.importedRecords == 0) {
        transaction.rollback();
        report.batchId = QString();
        return succeed(report);
    }

    QSqlQuery updateBatch(connection);
    updateBatch.prepare(QStringLiteral(
        "UPDATE import_batches SET exact_session_count = ?, bucket_count = ? WHERE id = ?"));
    updateBatch.addBindValue(report.exactSessions);
    updateBatch.addBindValue(report.hourBuckets);
    updateBatch.addBindValue(batchId);
    if (!updateBatch.exec()) {
        return fail<ImportCommitReport>(describeError(updateBatch));
    }

    const Status committed = transaction.commit();
    if (!committed.ok()) {
        return fail<ImportCommitReport>(committed.error);
    }

    report.batchId = batchId;
    return succeed(report);
}

Result<QVector<ImportBatchSummary>> ImportService::listBatches() const
{
    if (!_database.isOpen()) {
        return fail<QVector<ImportBatchSummary>>(QStringLiteral("数据库未打开"));
    }

    QSqlQuery query(_database.connection());
    if (!query.exec(QStringLiteral(
            "SELECT b.id, b.imported_at_ms, b.source_name, b.source_kind, "
            "       b.exact_session_count, b.bucket_count "
            "FROM import_batches b ORDER BY b.imported_at_ms ASC"))) {
        return fail<QVector<ImportBatchSummary>>(describeError(query));
    }

    QVector<ImportBatchSummary> batches;
    while (query.next()) {
        ImportBatchSummary summary;
        summary.id = query.value(0).toString();
        summary.importedAtMs = query.value(1).toLongLong();
        summary.sourceName = query.value(2).toString();
        summary.sourceKind = query.value(3).toString();
        summary.exactSessions = query.value(4).toLongLong();
        summary.hourBuckets = query.value(5).toLongLong();
        batches.append(summary);
    }
    return succeed(batches);
}

Result<ImportDeleteReport> ImportService::deleteBatch(const QString& batchId)
{
    if (!_database.isOpen()) {
        return fail<ImportDeleteReport>(QStringLiteral("数据库未打开"));
    }
    const QString id = batchId.trimmed();
    if (id.isEmpty()) {
        return fail<ImportDeleteReport>(QStringLiteral("批次 id 为空"));
    }

    Transaction transaction(_database);
    if (!transaction.isActive()) {
        return fail<ImportDeleteReport>(QStringLiteral("无法开启回滚事务"));
    }
    const QSqlDatabase connection = _database.connection();

    ImportDeleteReport report;

    // 段随批次级联删（import_batch_sessions.session_id → sessions ON DELETE CASCADE
    // 的方向是 sessions 删除时删登记行；这里反过来：删登记行时手动删段）。
    QSqlQuery countExact(connection);
    countExact.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM import_batch_sessions WHERE batch_id = ?"));
    countExact.addBindValue(id);
    if (!countExact.exec() || !countExact.next()) {
        return fail<ImportDeleteReport>(describeError(countExact));
    }
    report.deletedExactSessions = countExact.value(0).toLongLong();

    // 先记下受影响的应用（删除段之前），用于孤儿清理。
    QSet<QString> affectedExes;
    {
        QSqlQuery apps(connection);
        apps.prepare(QStringLiteral(
            "SELECT DISTINCT a.exe_name FROM import_batch_sessions i "
            "JOIN sessions s ON s.id = i.session_id "
            "JOIN apps a ON a.id = s.app_id WHERE i.batch_id = ?"));
        apps.addBindValue(id);
        if (!apps.exec()) {
            return fail<ImportDeleteReport>(describeError(apps));
        }
        while (apps.next()) {
            affectedExes.insert(apps.value(0).toString().toLower());
        }
    }

    // 删段 + 删登记行（级联走 import_batch_sessions.batch_id → import_batches，
    // 但 sessions 不自动删，手动删）。
    QSqlQuery deleteSessions(connection);
    deleteSessions.prepare(QStringLiteral(
        "DELETE FROM sessions WHERE id IN "
        "(SELECT session_id FROM import_batch_sessions WHERE batch_id = ?)"));
    deleteSessions.addBindValue(id);
    if (!deleteSessions.exec()) {
        return fail<ImportDeleteReport>(
            QStringLiteral("删除导入段失败：%1").arg(describeError(deleteSessions)));
    }

    // 小时桶：随批次级联删，先数一下。
    QSqlQuery countBuckets(connection);
    countBuckets.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM import_time_buckets WHERE batch_id = ?"));
    countBuckets.addBindValue(id);
    if (!countBuckets.exec() || !countBuckets.next()) {
        return fail<ImportDeleteReport>(describeError(countBuckets));
    }
    report.deletedHourBuckets = countBuckets.value(0).toLongLong();

    // 删批次行：import_batch_sessions 与 import_time_buckets 级联清空。
    QSqlQuery deleteBatchRow(connection);
    deleteBatchRow.prepare(QStringLiteral("DELETE FROM import_batches WHERE id = ?"));
    deleteBatchRow.addBindValue(id);
    if (!deleteBatchRow.exec()) {
        return fail<ImportDeleteReport>(
            QStringLiteral("删除批次失败：%1").arg(describeError(deleteBatchRow)));
    }
    if (deleteBatchRow.numRowsAffected() != 1) {
        return fail<ImportDeleteReport>(QStringLiteral("批次不存在或已被删除"));
    }

    // 孤儿应用清理：删掉导入段后不再有任何段、也没有桶引用的应用。
    for (const QString& exeName : affectedExes) {
        QSqlQuery stillUsed(connection);
        stillUsed.prepare(QStringLiteral(
            "SELECT EXISTS(SELECT 1 FROM sessions s JOIN apps a ON a.id = s.app_id "
            "             WHERE a.exe_name = ? COLLATE NOCASE) "
            "  OR EXISTS(SELECT 1 FROM import_time_buckets b "
            "            WHERE b.exe_name = ? COLLATE NOCASE)"));
        stillUsed.addBindValue(exeName);
        stillUsed.addBindValue(exeName);
        if (!stillUsed.exec() || !stillUsed.next()) {
            return fail<ImportDeleteReport>(describeError(stillUsed));
        }
        if (stillUsed.value(0).toInt() != 0) {
            continue;
        }
        QSqlQuery deleteApp(connection);
        deleteApp.prepare(QStringLiteral("DELETE FROM apps WHERE exe_name = ? COLLATE NOCASE"));
        deleteApp.addBindValue(exeName);
        if (!deleteApp.exec()) {
            return fail<ImportDeleteReport>(describeError(deleteApp));
        }
    }

    const Status committed = transaction.commit();
    if (!committed.ok()) {
        return fail<ImportDeleteReport>(committed.error);
    }
    return succeed(report);
}

} // namespace patina::data
