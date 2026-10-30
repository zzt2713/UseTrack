#include "engine/process_filter.h"

#include <QSet>
#include <QStringList>

namespace patina::engine
{
namespace
{

// 系统外壳、输入法、安全中心、锁屏这类进程：出现在前台也不是“用户在用的应用”。
const QSet<QString>& excludedProcesses()
{
    static const QSet<QString> excluded = {
        QStringLiteral("un.exe"),
        QStringLiteral("taskmgr.exe"),
        QStringLiteral("regedit.exe"),
        QStringLiteral("mmc.exe"),
        QStringLiteral("control.exe"),
        QStringLiteral("searchhost.exe"),
        QStringLiteral("searchapp.exe"),
        QStringLiteral("searchindexer.exe"),
        QStringLiteral("shellhost.exe"),
        QStringLiteral("shellexperiencehost.exe"),
        QStringLiteral("startmenuexperiencehost.exe"),
        // applicationframehost.exe 不在名单里：它是 UWP 应用的壳，
        // 投影成功会被替换成真实应用；投影失败时 isTrackableSample 会按
        // 「带 AUMID 的样本才放行」把它挡掉，一刀切会误杀所有商店应用。
        QStringLiteral("textinputhost.exe"),
        QStringLiteral("runtimebroker.exe"),
        QStringLiteral("taskhostw.exe"),
        QStringLiteral("consent.exe"),
        QStringLiteral("lockapp.exe"),
        QStringLiteral("logonui.exe"),
        QStringLiteral("sihost.exe"),
        QStringLiteral("dwm.exe"),
        QStringLiteral("ctfmon.exe"),
        QStringLiteral("fontdrvhost.exe"),
        QStringLiteral("hrupdate.exe"),
        QStringLiteral("securityhealthsystray.exe"),
        QStringLiteral("smartscreen.exe"),
        QStringLiteral("winlogon.exe"),
        QStringLiteral("userinit.exe"),
        QStringLiteral("pickerhost.exe"),
        QStringLiteral("openwith.exe"),
    };
    return excluded;
}

// 按 - _ . 与空白切 token。
// 不用 QRegExp：Qt6 把它挪到了 Core5Compat，不在允许的模块清单里。
QStringList splitTokens(const QString& value)
{
    QStringList tokens;
    QString current;
    current.reserve(value.size());
    for (const QChar ch : value) {
        if (ch == QLatin1Char('-') || ch == QLatin1Char('_') || ch == QLatin1Char('.')
            || ch.isSpace()) {
            if (!current.isEmpty()) {
                tokens.append(current);
                current.clear();
            }
            continue;
        }
        current.append(ch);
    }
    if (!current.isEmpty()) {
        tokens.append(current);
    }
    return tokens;
}

QString unquote(const QString& value)
{
    QString normalized = value.trimmed();
    if (normalized.size() >= 2 && normalized.startsWith(QLatin1Char('"'))
        && normalized.endsWith(QLatin1Char('"'))) {
        normalized = normalized.mid(1, normalized.size() - 2);
    }
    return normalized;
}

bool isLikelySystemProcess(const QString& lowerName)
{
    if (lowerName.startsWith(QStringLiteral("search"))
        && lowerName.endsWith(QStringLiteral(".exe"))) {
        return true;
    }
    if (lowerName.endsWith(QStringLiteral("host.exe"))
        && (lowerName.contains(QStringLiteral("experience"))
            || lowerName.contains(QStringLiteral("runtime"))
            || lowerName.contains(QStringLiteral("task"))
            || lowerName.contains(QStringLiteral("textinput"))
            || lowerName.contains(QStringLiteral("fontdrv")))) {
        return true;
    }
    if (lowerName.endsWith(QStringLiteral("broker.exe"))
        || lowerName.endsWith(QStringLiteral("systray.exe"))) {
        return true;
    }
    return lowerName == QStringLiteral("svchost.exe") || lowerName == QStringLiteral("dllhost.exe");
}

// 独立的卸载器工具（Geek Uninstaller 之类）是用户主动使用的应用，不该被
// 生命周期规则误伤。
bool isStandaloneUninstallerApp(const QString& stem)
{
    QString compact;
    compact.reserve(stem.size());
    for (const QChar ch : stem) {
        if (ch.unicode() < 128 && ch.isLetterOrNumber()) {
            compact.append(ch);
        }
    }
    static const QSet<QString> allowed = {
        QStringLiteral("geek"),
        QStringLiteral("geekuninstaller"),
        QStringLiteral("revouninstaller"),
        QStringLiteral("revouninstallerpro"),
        QStringLiteral("iobituninstaller"),
        QStringLiteral("hibituninstaller"),
        QStringLiteral("bcuninstaller"),
        QStringLiteral("bulkcrapuninstaller"),
        QStringLiteral("uninstalr"),
    };
    return allowed.contains(compact);
}

const QSet<QString>& lifecycleMarkers()
{
    static const QSet<QString> markers = {
        QStringLiteral("setup"),
        QStringLiteral("install"),
        QStringLiteral("installer"),
        QStringLiteral("uninstall"),
        QStringLiteral("uninstaller"),
        QStringLiteral("unins"),
        QStringLiteral("unins000"),
        QStringLiteral("update"),
        QStringLiteral("updater"),
        QStringLiteral("upgrade"),
        QStringLiteral("remove"),
        QStringLiteral("maintenance"),
        QStringLiteral("maintenancetool"),
    };
    return markers;
}

bool isLifecycleUtilityProcess(const QString& lowerName)
{
    const QString normalized = unquote(lowerName);
    const QString stem = normalized.endsWith(QStringLiteral(".exe"))
        ? normalized.left(normalized.size() - 4)
        : normalized;
    if (stem.isEmpty()) {
        return false;
    }
    if (isStandaloneUninstallerApp(stem)) {
        return false;
    }

    const QSet<QString>& markers = lifecycleMarkers();
    if (markers.contains(stem)) {
        return true;
    }

    // 只有“多段名字”才按 token 判：单段普通名字不该被切开误伤。
    const QStringList tokens = splitTokens(stem);
    if (tokens.size() < 2) {
        return false;
    }
    for (const QString& token : tokens) {
        if (markers.contains(token)) {
            return true;
        }
    }
    return false;
}

bool isTemporaryExecutableProcess(const QString& lowerName)
{
    return unquote(lowerName).endsWith(QStringLiteral(".tmp"));
}

} // namespace

bool shouldTrackProcess(const QString& exeName)
{
    const QString lowerName = exeName.toLower().trimmed();
    if (lowerName.isEmpty()) {
        return false;
    }
    if (excludedProcesses().contains(lowerName)) {
        return false;
    }
    if (isLikelySystemProcess(lowerName)) {
        return false;
    }
    if (isTemporaryExecutableProcess(lowerName)) {
        return false;
    }
    if (isLifecycleUtilityProcess(lowerName)) {
        return false;
    }
    return true;
}

bool isTrackableSample(const WindowSample& sample)
{
    if (sample.isEmpty()) {
        return false;
    }
    if (sample.afk) {
        return false;
    }
    if (!shouldTrackProcess(sample.exeName)) {
        return false;
    }

    // 桌面、任务栏这类窗口是外壳表面，不是活动应用。
    static const QSet<QString> shellClasses = {
        QStringLiteral("progman"),
        QStringLiteral("workerw"),
        QStringLiteral("shelldll_defview"),
        QStringLiteral("syslistview32"),
        QStringLiteral("shell_traywnd"),
        QStringLiteral("shell_secondarytraywnd"),
    };
    if (shellClasses.contains(sample.windowClass.toLower())) {
        return false;
    }

    const QString lowerExe = sample.exeName.toLower();

    // 壁纸引擎在没有标题的窗口上等于桌面本身；有标题（比如它的设置窗口）
    // 仍按普通应用记录。
    if (sample.title.trimmed().isEmpty()
        && (lowerExe == QStringLiteral("ui32.exe") || lowerExe == QStringLiteral("wallpaper32.exe")
            || lowerExe == QStringLiteral("wallpaper64.exe")
            || lowerExe == QStringLiteral("wallpaperengine.exe"))) {
        return false;
    }

    // explorer.exe 只有文件夹窗口才算活动；桌面本身也是 explorer.exe，
    // 但它是 Progman/WorkerW，已被上面挡掉。
    if (lowerExe == QStringLiteral("explorer.exe")) {
        const QString cls = sample.windowClass.toLower();
        return cls == QStringLiteral("cabinetwclass") || cls == QStringLiteral("explorewclass");
    }

    // 壳进程只剩投影失败的场景（AUMID 没查到 / 子窗口歧义）。这种样本
    // 记下来也只会是 applicationframehost.exe，直接挡掉。真实应用在 platform
    // 层就被投影成自己的 exe 名，走不到这里。
    if (lowerExe == QStringLiteral("applicationframehost.exe")) {
        return false;
    }

    return true;
}

} // namespace patina::engine
