#include "ui/settings/settings_config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStandardPaths>

namespace patina::ui
{
namespace
{

const char* const kDataDirectoryKey = "dataDirectory";

QString configDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

// 读整个 config.json。文件不存在或坏掉都返回空对象：
// 配置读不出来时程序该照常跑，退回到默认数据目录，而不是拒绝启动。
QJsonObject readConfig()
{
    const QString path = SettingsConfig::configFilePath();
    if (path.isEmpty()) {
        return {};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    return document.object();
}

} // namespace

QString SettingsConfig::configFilePath()
{
    const QString directory = configDirectory();
    if (directory.isEmpty()) {
        return QString();
    }
    return directory + QStringLiteral("/config.json");
}

QString SettingsConfig::defaultDataDirectory()
{
    // 与 TrackingService 的默认值保持一致：都在 AppDataLocation 下。
    return configDirectory();
}

QString SettingsConfig::dataDirectory()
{
    const QJsonObject config = readConfig();
    const QString configured = config.value(QString::fromLatin1(kDataDirectoryKey)).toString().trimmed();
    if (configured.isEmpty()) {
        return defaultDataDirectory();
    }
    return QDir::fromNativeSeparators(configured);
}

bool SettingsConfig::hasCustomDataDirectory()
{
    const QJsonObject config = readConfig();
    return !config.value(QString::fromLatin1(kDataDirectoryKey)).toString().trimmed().isEmpty();
}

QString SettingsConfig::databasePath()
{
    const QString directory = dataDirectory();
    if (directory.isEmpty()) {
        return QString();
    }
    return directory + QStringLiteral("/usetrack.db");
}

QString SettingsConfig::wallpaperDirectory()
{
    const QString directory = configDirectory();
    if (directory.isEmpty()) {
        return QString();
    }
    return directory + QStringLiteral("/wallpaper");
}

QString SettingsConfig::wallpaperFilePath()
{
    const QDir directory(wallpaperDirectory());
    if (directory.isEmpty() || !directory.exists()) {
        return QString();
    }
    // 固定基名 + 按内容嗅探出的扩展名，所以刷新后如果格式变了（jpg→png），
    // 旧文件已经被删掉，这里总能拿到唯一那一份。
    const QStringList candidates = directory.entryList(
        {QStringLiteral("wallpaper.png"), QStringLiteral("wallpaper.jpg"),
         QStringLiteral("wallpaper.webp")},
        QDir::Files, QDir::Name);
    if (candidates.isEmpty()) {
        return QString();
    }
    return directory.absoluteFilePath(candidates.first());
}

bool SettingsConfig::writeKey(const QString& key, const QString& value, QString* errorOut)
{
    const QString path = configFilePath();
    if (path.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("无法确定配置文件路径");
        }
        return false;
    }

    const QDir directory = QFileInfo(path).absoluteDir();
    if (!directory.exists() && !QDir().mkpath(directory.absolutePath())) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("无法创建配置目录：%1").arg(directory.absolutePath());
        }
        return false;
    }

    // 读一次再改：文件里以后要放别的设置，不能整份覆盖掉。
    QJsonObject config = readConfig();
    if (value.isEmpty()) {
        config.remove(key);
    } else {
        config.insert(key, value);
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("无法写入配置文件：%1").arg(file.errorString());
        }
        return false;
    }
    const QJsonDocument document(config);
    const qint64 written = file.write(document.toJson(QJsonDocument::Indented));
    file.close();
    if (written <= 0) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("写入配置文件失败：%1").arg(path);
        }
        return false;
    }
    return true;
}

bool SettingsConfig::setDataDirectory(const QString& directory, QString* errorOut)
{
    const QString trimmed = directory.trimmed();
    // 传默认目录等于「不自定义」，把这项清掉，config.json 里不留无意义的记录。
    if (trimmed.isEmpty() || QDir::cleanPath(trimmed) == QDir::cleanPath(defaultDataDirectory())) {
        return writeKey(QString::fromLatin1(kDataDirectoryKey), QString(), errorOut);
    }
    return writeKey(QString::fromLatin1(kDataDirectoryKey), QDir::cleanPath(trimmed), errorOut);
}

} // namespace patina::ui
