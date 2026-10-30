#include "engine/sustained_identity.h"

#include <QStringList>

namespace patina::engine
{
namespace
{

// 归一化进程侧的值：去引号、去空白、转小写。
// 路径可能带引号（从命令行或注册表来的），不剥掉就匹配不上。
QString normalizeProcessValue(const QString& raw)
{
    return raw.trimmed().remove(QLatin1Char('"')).toLower();
}

// 归一化信号源标识：只留 ASCII 字母数字，转小写。
// AUMID 里有点、下划线、版本号（Microsoft.ZuneVideo_8wekyb3d8bbwe!App），
// 直接比对必然失配，去掉分隔符后按子串包含判定才稳。
QString normalizeSourceIdentifier(const QString& raw)
{
    QString result;
    result.reserve(raw.size());
    const QString lowered = raw.trimmed().toLower();
    for (const QChar character : lowered) {
        if (character.isLetterOrNumber() && character.unicode() < 128) {
            result.append(character);
        }
    }
    return result;
}

QString fileNameOf(const QString& path)
{
    const int index = path.lastIndexOf(QLatin1Char('\\'));
    if (index >= 0) {
        return path.mid(index + 1);
    }
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash >= 0 ? path.mid(slash + 1) : path;
}

bool pathEndsWith(const QString& normalizedPath, const char* suffix)
{
    return normalizedPath.endsWith(QLatin1String(suffix), Qt::CaseInsensitive);
}

} // namespace

SustainedApp appIdentityFromProcess(const QString& exeName, const QString& exePath)
{
    const QString exe = normalizeProcessValue(exeName);
    const QString path = normalizeProcessValue(exePath);
    if (exe.isEmpty() && path.isEmpty()) {
        return SustainedApp::None;
    }

    const QString stem = fileNameOf(exe.isEmpty() ? path : exe);
    const QString stemWithoutExt = stem.endsWith(QStringLiteral(".exe"))
                                       ? stem.left(stem.size() - 4)
                                       : stem;

    if (exe == QLatin1String("chrome.exe") || exe == QLatin1String("chrome")
        || pathEndsWith(path, "\\chrome.exe")) {
        return SustainedApp::Chrome;
    }
    if (exe == QLatin1String("msedge.exe") || exe == QLatin1String("msedge")
        || pathEndsWith(path, "\\msedge.exe")) {
        return SustainedApp::Edge;
    }
    if (exe == QLatin1String("firefox.exe") || exe == QLatin1String("firefox")
        || pathEndsWith(path, "\\firefox.exe")) {
        return SustainedApp::Firefox;
    }
    if (exe == QLatin1String("brave.exe") || exe == QLatin1String("brave")
        || pathEndsWith(path, "\\brave.exe")) {
        return SustainedApp::Brave;
    }
    if (exe == QLatin1String("zoom.exe") || exe == QLatin1String("zoom")
        || pathEndsWith(path, "\\zoom.exe")) {
        return SustainedApp::Zoom;
    }
    if (exe == QLatin1String("teams.exe") || exe == QLatin1String("msteams.exe")
        || pathEndsWith(path, "\\teams.exe") || pathEndsWith(path, "\\msteams.exe")) {
        return SustainedApp::Teams;
    }
    if (exe == QLatin1String("vlc.exe") || exe == QLatin1String("vlc")
        || pathEndsWith(path, "\\vlc.exe")) {
        return SustainedApp::Vlc;
    }
    // B 站客户端的安装名带版本号（bilibili_3.x.x.exe），按前缀认。
    if (stemWithoutExt.startsWith(QLatin1String("bilibili"))
        || path.contains(QLatin1String("\\bilibili\\"))) {
        return SustainedApp::Bilibili;
    }
    if (stemWithoutExt.startsWith(QLatin1String("douyin"))
        || path.contains(QLatin1String("\\douyin\\"))
        || path.contains(QLatin1String("\\bytedance\\douyin\\"))) {
        return SustainedApp::Douyin;
    }
    if (exe == QLatin1String("wemeetapp.exe") || exe == QLatin1String("tencentmeeting.exe")
        || pathEndsWith(path, "\\wemeetapp.exe") || pathEndsWith(path, "\\tencentmeeting.exe")) {
        return SustainedApp::WeMeet;
    }
    return SustainedApp::None;
}

SustainedApp appIdentityFromSourceAppId(const QString& sourceAppId)
{
    const QString normalized = normalizeSourceIdentifier(sourceAppId);
    if (normalized.isEmpty()) {
        return SustainedApp::None;
    }

    // 顺序有意义：Edge 的 AUMID 里含 "microsoftedge"，但也可能撞上别的
    // 含 "edge" 的标识，所以先判更具体的。
    if (normalized.contains(QLatin1String("msteams")) || normalized.contains(QLatin1String("teams"))) {
        return SustainedApp::Teams;
    }
    if (normalized.contains(QLatin1String("wemeet"))
        || normalized.contains(QLatin1String("tencentmeeting"))
        || normalized.contains(QLatin1String("voovmeeting"))) {
        return SustainedApp::WeMeet;
    }
    if (normalized.contains(QLatin1String("msedge"))
        || normalized.contains(QLatin1String("microsoftedge"))
        || normalized.contains(QLatin1String("edge"))) {
        return SustainedApp::Edge;
    }
    if (normalized.contains(QLatin1String("chrome"))) {
        return SustainedApp::Chrome;
    }
    if (normalized.contains(QLatin1String("firefox"))) {
        return SustainedApp::Firefox;
    }
    if (normalized.contains(QLatin1String("brave"))) {
        return SustainedApp::Brave;
    }
    if (normalized.contains(QLatin1String("zoom"))) {
        return SustainedApp::Zoom;
    }
    if (normalized.contains(QLatin1String("vlc"))) {
        return SustainedApp::Vlc;
    }
    if (normalized.contains(QLatin1String("bilibilipc"))
        || normalized.contains(QLatin1String("bilibili"))) {
        return SustainedApp::Bilibili;
    }
    if (normalized.contains(QLatin1String("douyin")) || normalized.contains(QLatin1String("aweme"))) {
        return SustainedApp::Douyin;
    }
    return SustainedApp::None;
}

QString resolveSustainedIdentityKey(const QString& exeName, const QString& exePath)
{
    const QString exe = normalizeProcessValue(exeName);
    const QString path = normalizeProcessValue(exePath);

    const SustainedApp app = appIdentityFromProcess(exe, path);
    if (app != SustainedApp::None) {
        // 认得出应用就用应用名当键：同一个应用的多个进程（Chrome 的渲染进程、
        // 主进程）应该算同一个身份，宽限期才对得上。
        static const char* const kNames[] = {
            "none", "chrome", "edge", "firefox", "brave",
            "zoom", "teams", "vlc", "bilibili", "douyin", "wemeet"
        };
        const int index = static_cast<int>(app);
        return QLatin1String(kNames[index]);
    }

    const QString pathName = fileNameOf(path);
    if (!pathName.isEmpty()) {
        return pathName;
    }
    return exe;
}

bool sourceAppIdMatchesWindow(const QString& sourceAppId,
                              const QString& exeName,
                              const QString& exePath)
{
    const QString normalizedSource = normalizeSourceIdentifier(sourceAppId);
    if (normalizedSource.isEmpty()) {
        return false;
    }

    // 四个候选：exe 名与路径各取「全值」和「文件名」。
    // 双向包含：AUMID 里可能只带产品名（"chrome"），
    // 也可能带完整包名，两个方向都得试。
    QStringList candidates;
    for (const QString& raw : {exeName, exePath}) {
        const QString normalized = normalizeSourceIdentifier(raw);
        if (normalized.isEmpty()) {
            continue;
        }
        candidates.append(normalized);
        const QString name = normalizeSourceIdentifier(fileNameOf(normalizeProcessValue(raw)));
        if (!name.isEmpty()) {
            candidates.append(name);
        }
    }

    for (const QString& candidate : candidates) {
        if (normalizedSource.contains(candidate) || candidate.contains(normalizedSource)) {
            return true;
        }
    }
    return false;
}

bool signalOriginMatchesWindow(const SustainedSignal& signal,
                               const QString& exeName,
                               const QString& exePath)
{
    const SustainedApp windowApp = appIdentityFromProcess(exeName, exePath);
    const SustainedApp signalApp = signal.exeName.isEmpty()
                                       ? appIdentityFromSourceAppId(signal.sourceAppId)
                                       : appIdentityFromProcess(signal.exeName, signal.exePath);
    if (windowApp != SustainedApp::None && windowApp == signalApp) {
        return true;
    }
    return sourceAppIdMatchesWindow(signal.sourceAppId, exeName, exePath);
}

bool signalMatchesWindow(const SustainedSignal& signal,
                         const QString& exeName,
                         const QString& exePath)
{
    return signal.active && signalOriginMatchesWindow(signal, exeName, exePath);
}

bool signalExplicitlyStopped(const SustainedSignal& signal,
                             const QString& exeName,
                             const QString& exePath)
{
    return signal.available && !signal.active
           && signalOriginMatchesWindow(signal, exeName, exePath);
}

} // namespace patina::engine
