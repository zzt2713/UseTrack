#include "platform/packaged_app.h"

#include "platform/win_handle.h"

#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QString>

#include <appmodel.h>
#include <tlhelp32.h>
#include <cwchar>

namespace patina::platform
{
namespace
{

constexpr wchar_t kApplicationFrameHostExe[] = L"ApplicationFrameHost.exe";
constexpr quint64 kIdentityCacheTtlMs = 10'000;
constexpr quint64 kIdentityNegativeCacheTtlMs = 1'000;
constexpr int kIdentityCacheMaxEntries = 128;
constexpr int kMaxPathLength = 1024;
constexpr quint64 kProjectionCacheTtlMs = 1'000;

struct IdentityCacheEntry
{
    QString appUserModelId; // 空 = 负缓存（该进程没有包标识）
    bool resolved = false;
    quint64 cachedAtMs = 0;
};

QMutex& identityCacheMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<quint32, IdentityCacheEntry>& identityCache()
{
    static QHash<quint32, IdentityCacheEntry> cache;
    return cache;
}

quint64 nowMs()
{
    return static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
}

bool isEntryFresh(const IdentityCacheEntry& entry, quint64 now)
{
    const quint64 ttl = entry.resolved ? kIdentityCacheTtlMs : kIdentityNegativeCacheTtlMs;
    return now - entry.cachedAtMs <= ttl;
}

bool exeEquals(const QString& left, const wchar_t* right)
{
    // QLatin1String 只吃 char*，wchar_t* 走 QString::fromWCharArray 转一次。
    return left.compare(QString::fromWCharArray(right), Qt::CaseInsensitive) == 0;
}

QString fromWide(const wchar_t* text, int length)
{
    if (text == nullptr || length <= 0) {
        return QString();
    }
    return QString::fromWCharArray(text, length);
}

void insertIdentityCache(quint32 processId, const IdentityCacheEntry& entry)
{
    QMutexLocker locker(&identityCacheMutex());
    if (identityCache().size() >= kIdentityCacheMaxEntries) {
        identityCache().clear();
    }
    identityCache().insert(processId, entry);
}

QString queryProcessAppUserModelIdUncached(quint32 processId, bool* resolved)
{
    *resolved = false;

    HANDLE raw = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (raw == nullptr) {
        return QString();
    }
    detail::UniqueHandle process(raw);

    // 首查只取长度：非打包进程直接返回 APPMODEL_ERROR_NO_APPLICATION，
    // 打包进程返回 ERROR_INSUFFICIENT_BUFFER 并带出所需长度。
    quint32 length = 0;
    const LONG initialStatus = ::GetApplicationUserModelId(process.get(), &length, nullptr);
    if (initialStatus != static_cast<LONG>(ERROR_INSUFFICIENT_BUFFER) || length < 2) {
        return QString();
    }

    // 缓冲查询，最多两次 resize。
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::wstring buffer(length, L'\0');
        quint32 written = length;
        const LONG status = ::GetApplicationUserModelId(process.get(), &written, buffer.data());
        if (status == static_cast<LONG>(ERROR_SUCCESS)) {
            const int terminator = static_cast<int>(::wcsnlen(buffer.c_str(), written));
            const QString value = fromWide(buffer.c_str(), terminator).trimmed();
            if (!value.isEmpty()) {
                *resolved = true;
            }
            return value;
        }
        if (status == static_cast<LONG>(ERROR_INSUFFICIENT_BUFFER) && written > length) {
            length = written;
            continue;
        }
        break;
    }
    return QString();
}

struct DescendantCollection
{
    QList<quintptr> windows;
};

BOOL CALLBACK collectDescendantWindow(HWND hwnd, LPARAM lparam)
{
    auto* collection = reinterpret_cast<DescendantCollection*>(lparam);
    collection->windows.append(reinterpret_cast<quintptr>(hwnd));
    return TRUE;
}

struct ProjectionCacheEntry
{
    quintptr rootOwnerHwnd = 0;
    quint32 rootProcessId = 0;
    QString rootExeName;
    CachedProjection projection;
    quint64 cachedAtMs = 0;
};

QMutex& projectionCacheMutex()
{
    static QMutex mutex;
    return mutex;
}

ProjectionCacheEntry& projectionCache()
{
    static ProjectionCacheEntry cache;
    return cache;
}

} // namespace

QString queryProcessAppUserModelId(quint32 processId)
{
    if (processId == 0) {
        return QString();
    }

    const quint64 now = nowMs();
    {
        QMutexLocker locker(&identityCacheMutex());
        const auto it = identityCache().constFind(processId);
        if (it != identityCache().constEnd()) {
            if (isEntryFresh(*it, now)) {
                return it->appUserModelId;
            }
            identityCache().erase(it);
        }
    }

    bool resolved = false;
    const QString value = queryProcessAppUserModelIdUncached(processId, &resolved);
    IdentityCacheEntry entry;
    entry.resolved = resolved;
    entry.appUserModelId = value;
    entry.cachedAtMs = now;
    insertIdentityCache(processId, entry);
    return value;
}

QString processImagePathOf(quint32 processId)
{
    if (processId == 0) {
        return QString();
    }

    HANDLE raw = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (raw == nullptr) {
        return QString();
    }
    detail::UniqueHandle process(raw);

    wchar_t buffer[kMaxPathLength] = {};
    DWORD size = kMaxPathLength;
    if (!::QueryFullProcessImageNameW(process.get(), 0, buffer, &size)) {
        return QString();
    }
    return fromWide(buffer, static_cast<int>(size));
}

QString processExeNameOf(quint32 processId)
{
    const QString path = processImagePathOf(processId);
    const int separator = path.lastIndexOf(QLatin1Char('\\'));
    if (separator >= 0) {
        return path.mid(separator + 1);
    }

    // 路径拿不到（提权进程等）时退回 Toolhelp 快照。
    detail::UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        return QString();
    }
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(PROCESSENTRY32W);
    if (!::Process32FirstW(snapshot.get(), &entry)) {
        return QString();
    }
    do {
        if (entry.th32ProcessID == processId) {
            return fromWide(entry.szExeFile, static_cast<int>(::wcslen(entry.szExeFile)));
        }
    } while (::Process32NextW(snapshot.get(), &entry));
    return QString();
}

HostedAppResolution resolveHostedApp(quintptr rootOwnerHwnd,
                                     quint32 rootProcessId,
                                     const QString& rootExeName,
                                     HostedApp* hostedOut)
{
    if (hostedOut != nullptr) {
        *hostedOut = HostedApp{};
    }
    if (!exeEquals(rootExeName, kApplicationFrameHostExe)) {
        return HostedAppResolution::NotApplicable;
    }

    const HWND root = reinterpret_cast<HWND>(rootOwnerHwnd);
    if (root == nullptr) {
        return HostedAppResolution::Unavailable;
    }

    DescendantCollection collection;
    if (!::EnumChildWindows(root, collectDescendantWindow,
                            reinterpret_cast<LPARAM>(&collection))) {
        return HostedAppResolution::Unavailable;
    }

    // 按 AUMID 分组：同一包标识下的多个子窗口算同一个候选。
    struct Candidate
    {
        quint32 processId = 0;
        QString exeName;
        QString exePath;
        bool ownsVisibleWindow = false;
    };
    QHash<QString, QList<Candidate>> byIdentity;

    for (const quintptr hwndValue : collection.windows) {
        const HWND hwnd = reinterpret_cast<HWND>(hwndValue);
        DWORD processId = 0;
        ::GetWindowThreadProcessId(hwnd, &processId);
        if (processId == 0 || processId == rootProcessId) {
            continue;
        }

        const QString processPath = processImagePathOf(processId);
        const QString exeName = processExeNameOf(processId);
        if (processPath.trimmed().isEmpty() || exeName.trimmed().isEmpty()
            || exeEquals(exeName, kApplicationFrameHostExe)) {
            continue;
        }

        const QString aumid = queryProcessAppUserModelId(processId);
        if (aumid.trimmed().isEmpty()) {
            continue;
        }

        Candidate candidate;
        candidate.processId = processId;
        candidate.exeName = exeName;
        candidate.exePath = processPath;
        candidate.ownsVisibleWindow = ::IsWindowVisible(hwnd) != FALSE;
        byIdentity[aumid.toLower()].append(candidate);
    }

    if (byIdentity.isEmpty()) {
        return HostedAppResolution::Unavailable;
    }
    if (byIdentity.size() != 1) {
        return HostedAppResolution::Ambiguous;
    }

    QList<Candidate> candidates = byIdentity.cbegin().value();
    QList<Candidate> preferred;
    for (const Candidate& candidate : candidates) {
        if (candidate.ownsVisibleWindow) {
            preferred.append(candidate);
        }
    }
    if (preferred.isEmpty()) {
        preferred = candidates;
    }

    // exe 名必须一致，否则一个壳窗口里藏着多个应用，认不准就不投影。
    QString singleExe;
    for (const Candidate& candidate : preferred) {
        const QString key = candidate.exeName.toLower();
        if (singleExe.isEmpty()) {
            singleExe = key;
        } else if (key != singleExe) {
            return HostedAppResolution::Ambiguous;
        }
    }

    // pid 最小的最可能是主进程。
    const Candidate* chosen = &preferred.first();
    for (const Candidate& candidate : preferred) {
        if (candidate.processId < chosen->processId) {
            chosen = &candidate;
        }
    }

    if (hostedOut != nullptr) {
        hostedOut->processId = chosen->processId;
        hostedOut->exeName = chosen->exeName;
        hostedOut->exePath = chosen->exePath;
        // QHash 没有 firstKey()，用 begin() 拿第一个桶的 key（分组已经
        // 保证 size == 1，所以这个 key 就是唯一的那个）。
        hostedOut->appUserModelId = byIdentity.begin().key();
    }
    return HostedAppResolution::Resolved;
}

HostedAppResolution resolveHostedApp(quintptr rootOwnerHwnd,
                                     quint32 rootProcessId,
                                     const QString& rootExeName)
{
    return resolveHostedApp(rootOwnerHwnd, rootProcessId, rootExeName, nullptr);
}

CachedProjection resolveHostedAppCached(quintptr rootOwnerHwnd,
                                        quint32 rootProcessId,
                                        const QString& rootExeName)
{
    const quint64 now = nowMs();
    {
        QMutexLocker locker(&projectionCacheMutex());
        ProjectionCacheEntry& cached = projectionCache();
        // 缓存键是 root hwnd：换窗口 pid 也会跟着换，用三元组一起判。
        if (cached.rootOwnerHwnd == rootOwnerHwnd && cached.rootProcessId == rootProcessId
            && cached.rootExeName == rootExeName && now - cached.cachedAtMs <= kProjectionCacheTtlMs) {
            return cached.projection;
        }
    }

    CachedProjection result;
    result.resolution =
        resolveHostedApp(rootOwnerHwnd, rootProcessId, rootExeName, &result.hosted);

    QMutexLocker locker(&projectionCacheMutex());
    ProjectionCacheEntry& cached = projectionCache();
    cached.rootOwnerHwnd = rootOwnerHwnd;
    cached.rootProcessId = rootProcessId;
    cached.rootExeName = rootExeName;
    cached.projection = result;
    cached.cachedAtMs = now;
    return result;
}

} // namespace patina::platform
