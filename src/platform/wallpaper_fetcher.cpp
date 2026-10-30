#include "platform/wallpaper_fetcher.h"

// winhttp.h 自身不引 windows.h，只依赖其中的基础类型；
// 走平台目录统一的入口拿 windows.h（顺带 NOMINMAX / WIN32_LEAN_AND_MEAN）。
#include "platform/win_handle.h"

#include <winhttp.h>

#include <string>

namespace patina::platform
{
namespace
{

// 单张壁纸的体积上限：1080p 到 4K 的 JPEG/WebP 都在 1~3MB，
// 32MB 是「肯定不是壁纸」的分界线，也是被恶意接口拖死内存的兜底。
constexpr DWORD kMaxWallpaperBytes = 32 * 1024 * 1024;
constexpr int kResolveTimeoutMs = 10000;
constexpr int kConnectTimeoutMs = 10000;
constexpr int kSendTimeoutMs = 15000;
constexpr int kReceiveTimeoutMs = 25000;

// HINTERNET 必须用 WinHttpCloseHandle 释放，平台目录里的通用 UniqueHandle
// （CloseHandle）在这上面是错的，所以单独包一个。
class UniqueWinHttpHandle
{
public:
    UniqueWinHttpHandle() = default;
    explicit UniqueWinHttpHandle(HINTERNET handle) : _handle(handle) {}
    ~UniqueWinHttpHandle() { reset(); }

    UniqueWinHttpHandle(const UniqueWinHttpHandle&) = delete;
    UniqueWinHttpHandle& operator=(const UniqueWinHttpHandle&) = delete;

    HINTERNET get() const { return _handle; }
    explicit operator bool() const { return _handle != nullptr; }

    void reset(HINTERNET handle = nullptr)
    {
        if (_handle != nullptr) {
            ::WinHttpCloseHandle(_handle);
        }
        _handle = handle;
    }

private:
    HINTERNET _handle = nullptr;
};

QString describeLastError()
{
    const DWORD code = ::GetLastError();
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);

    QString detail;
    if (length > 0 && buffer != nullptr) {
        detail = QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed();
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    if (detail.isEmpty()) {
        detail = QStringLiteral("错误码 %1").arg(code);
    }
    return detail;
}

} // namespace

WallpaperResponse fetchWallpaper(const QString& url)
{
    WallpaperResponse response;

    // WinHttpCrackUrl 会往输入串里写指针，必须给可写缓冲。
    std::wstring wideUrl = url.toStdWString();
    if (wideUrl.empty()) {
        response.error = QStringLiteral("壁纸地址为空");
        return response;
    }

    URL_COMPONENTS components = {};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (::WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &components)
        == FALSE) {
        response.error = QStringLiteral("壁纸地址无效：%1").arg(describeLastError());
        return response;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTPS && components.nScheme != INTERNET_SCHEME_HTTP) {
        response.error = QStringLiteral("壁纸接口只支持 http/https");
        return response;
    }

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring target(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength != 0 && components.lpszExtraInfo != nullptr) {
        target.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (target.empty()) {
        target = L"/";
    }
    const INTERNET_PORT port = components.nPort;

    // AUTOMATIC_PROXY 走系统代理设置；个别老系统上打头会失败，退回默认代理。
    UniqueWinHttpHandle session(::WinHttpOpen(L"UseTrack/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        session.reset(::WinHttpOpen(L"UseTrack/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    }
    if (!session) {
        response.error = QStringLiteral("无法初始化网络会话：%1").arg(describeLastError());
        return response;
    }
    ::WinHttpSetTimeouts(session.get(), kResolveTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs,
                         kReceiveTimeoutMs);

    UniqueWinHttpHandle connection(
        ::WinHttpConnect(session.get(), host.c_str(), port, 0));
    if (!connection) {
        response.error = QStringLiteral("无法连接壁纸服务器：%1").arg(describeLastError());
        return response;
    }

    UniqueWinHttpHandle request(::WinHttpOpenRequest(
        connection.get(), L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        response.error = QStringLiteral("无法创建请求：%1").arg(describeLastError());
        return response;
    }

    // 两个接口站都是 302 跳到 CDN，交给 WinHTTP 自己跟，省得手写 Location 解析。
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    ::WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
                       sizeof(redirectPolicy));

    if (::WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        == FALSE) {
        response.error = QStringLiteral("发送请求失败：%1").arg(describeLastError());
        return response;
    }
    if (::WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
        response.error = QStringLiteral("等待响应失败：%1").arg(describeLastError());
        return response;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (::WinHttpQueryHeaders(request.get(),
                              WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                              WINHTTP_NO_HEADER_INDEX)
        == FALSE) {
        response.error = QStringLiteral("读不到响应状态：%1").arg(describeLastError());
        return response;
    }
    if (statusCode != 200) {
        response.error = QStringLiteral("壁纸接口返回 HTTP %1").arg(statusCode);
        return response;
    }

    DWORD contentTypeSize = 0;
    if (::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_TYPE,
                              WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                              &contentTypeSize, WINHTTP_NO_HEADER_INDEX)
        && contentTypeSize > sizeof(wchar_t)) {
        std::wstring buffer(contentTypeSize / sizeof(wchar_t), L'\0');
        if (::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_TYPE,
                                  WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &contentTypeSize,
                                  WINHTTP_NO_HEADER_INDEX)) {
            response.contentType =
                QString::fromWCharArray(buffer.c_str()).trimmed().toLower();
        }
    }

    QByteArray data;
    DWORD available = 0;
    while (true) {
        if (::WinHttpQueryDataAvailable(request.get(), &available) == FALSE) {
            data.clear();
            response.error = QStringLiteral("下载壁纸中断：%1").arg(describeLastError());
            return response;
        }
        if (available == 0) {
            break;
        }
        if (static_cast<qint64>(data.size()) + static_cast<qint64>(available) > kMaxWallpaperBytes) {
            data.clear();
            response.error = QStringLiteral("壁纸超过 32MB，已放弃");
            return response;
        }
        const int offset = data.size();
        data.resize(offset + static_cast<int>(available));
        DWORD read = 0;
        if (::WinHttpReadData(request.get(), data.data() + offset, available, &read) == FALSE
            || read == 0) {
            data.clear();
            response.error = QStringLiteral("下载壁纸中断：%1").arg(describeLastError());
            return response;
        }
        data.resize(offset + static_cast<int>(read));
    }

    if (data.isEmpty()) {
        response.error = QStringLiteral("壁纸接口返回了空内容");
        return response;
    }

    response.ok = true;
    response.data = std::move(data);
    return response;
}

} // namespace patina::platform
