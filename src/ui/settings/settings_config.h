#ifndef PATINA_UI_SETTINGS_CONFIG_H
#define PATINA_UI_SETTINGS_CONFIG_H

#include <QString>

namespace patina::ui
{

// 应用级配置：目前只有「数据目录」一项。
//
// 注册表存储位置控制与 staging 迁移是过度设计，
// 一个 JSON 文件记下数据目录就够了。所以这里不做 schema 版本、不做迁移、
// 不做多份配置档：全部字段读不到就用默认值。
//
// 位置：%APPDATA%/UseTrack/config.json（与数据库默认位置同目录）。
class SettingsConfig
{
public:
    // config.json 的完整路径。取不到可写目录时返回空串。
    static QString configFilePath();

    // 默认数据目录（数据库所在目录）。与 TrackingService 的默认值一致。
    static QString defaultDataDirectory();

    // 读配置里的数据目录；未配置或读不到都返回默认目录。
    static QString dataDirectory();

    // 是否用了自定义目录（界面上要区分「默认」与「改过」）。
    static bool hasCustomDataDirectory();

    // 写数据目录。传空串或默认目录会清掉这项配置，回到默认位置。
    // 返回 false 时 errorOut 里是原因。
    static bool setDataDirectory(const QString& directory, QString* errorOut = nullptr);

    // 数据库文件的完整路径（数据目录 + usetrack.db）。
    static QString databasePath();

    // 壁纸目录：配置目录下的 wallpaper/。壁纸固定存一份，刷新即覆盖。
    static QString wallpaperDirectory();

    // 当前壁纸文件的完整路径；目录里没有壁纸时返回空串。
    // 扩展名按实际文件格式走（png/jpg/webp），所以这里要真的去目录里找。
    static QString wallpaperFilePath();

private:
    static bool writeKey(const QString& key, const QString& value, QString* errorOut);
};

} // namespace patina::ui

#endif // PATINA_UI_SETTINGS_CONFIG_H
