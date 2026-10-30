#include "platform/autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace patina::platform
{
namespace
{

// 值名带产品名，便于在注册表里一眼认出，也方便卸载时精确删。
const char* const kValueName = "UseTrack";
const char* const kRunKey = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// 自启启动时要带的参数：程序看到它就只驻留托盘，不弹主窗口。
// main.cpp 用的是同一个字面量，两处必须一致。
const char* const kAutostartArg = "--autostart";

QString g_lastError;

#ifdef Q_OS_WIN

// 读写注册表都要先打开同一个键，两处共用一个打开函数。
HKEY openRunKey(bool writable, REGSAM extraSam, LONG* errorOut)
{
    HKEY key = nullptr;
    const REGSAM sam = (writable ? KEY_WRITE : KEY_READ) | extraSam;
    const LONG result = RegOpenKeyExW(HKEY_CURRENT_USER,
                                      reinterpret_cast<LPCWSTR>(QString::fromLatin1(kRunKey).utf16()),
                                      0,
                                      sam,
                                      &key);
    if (result != ERROR_SUCCESS) {
        if (errorOut != nullptr) {
            *errorOut = result;
        }
        return nullptr;
    }
    return key;
}

QString describeWin32(LONG code)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                            | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr,
                                        static_cast<DWORD>(code),
                                        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                        reinterpret_cast<LPWSTR>(&buffer),
                                        0,
                                        nullptr);
    QString message;
    if (length > 0 && buffer != nullptr) {
        message = QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed();
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    if (message.isEmpty()) {
        message = QStringLiteral("Win32 错误码 %1").arg(code);
    }
    return message;
}

#endif // Q_OS_WIN

} // namespace

QString Autostart::commandLine()
{
    // 路径可能带空格（"C:\Program Files\..."），必须加引号，否则注册表里
    // 命令行会被拆成多段，Windows 会去找 "C:\Program" 这个不存在的程序。
    const QString path = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    // 带 --autostart：登录启动时只驻留托盘，不弹主窗口。
    return QStringLiteral("\"%1\" %2").arg(path, QString::fromLatin1(kAutostartArg));
}

QString Autostart::lastError()
{
    return g_lastError;
}

#ifdef Q_OS_WIN

QString Autostart::registeredCommand()
{
    LONG error = ERROR_SUCCESS;
    HKEY key = openRunKey(false, 0, &error);
    if (key == nullptr) {
        // 键不存在是正常情况（极少数精简系统），不当作错误。
        return QString();
    }

    DWORD type = 0;
    DWORD size = 0;
    LONG result = RegQueryValueExW(key,
                                   reinterpret_cast<LPCWSTR>(QString::fromLatin1(kValueName).utf16()),
                                   nullptr,
                                   &type,
                                   nullptr,
                                   &size);
    if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || size == 0) {
        RegCloseKey(key);
        return QString();
    }

    QByteArray buffer(static_cast<int>(size) + 2, 0);
    result = RegQueryValueExW(key,
                              reinterpret_cast<LPCWSTR>(QString::fromLatin1(kValueName).utf16()),
                              nullptr,
                              &type,
                              reinterpret_cast<LPBYTE>(buffer.data()),
                              &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) {
        return QString();
    }

    const QString value = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(buffer.constData()));
    return value.trimmed();
}

bool Autostart::isEnabled()
{
    const QString registered = registeredCommand();
    if (registered.isEmpty()) {
        return false;
    }

    // 只比可执行文件路径，不比参数：以后给自启加上 --autostart 之类的参数，
    // 这里不应该突然报「没启用」。
    const QString expected = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return registered.contains(expected, Qt::CaseInsensitive);
}

bool Autostart::setEnabled(bool enabled)
{
    const QString command = commandLine();
    if (command.size() <= 2) {
        g_lastError = QStringLiteral("无法确定程序路径");
        return false;
    }

    LONG error = ERROR_SUCCESS;
    HKEY key = openRunKey(true, 0, &error);
    if (key == nullptr) {
        g_lastError = QStringLiteral("无法打开注册表 Run 项：%1").arg(describeWin32(error));
        return false;
    }

    LONG result = ERROR_SUCCESS;
    // 注册表存的是 UTF-16，所以这里统一转 wstring，
    // 而不是把 QByteArray 塞进去（那样中文路径会被当字节写坏）。
    const std::wstring wideName = QString::fromLatin1(kValueName).toStdWString();
    if (enabled) {
        const std::wstring wideValue = command.toStdWString();
        result = RegSetValueExW(key,
                                wideName.c_str(),
                                0,
                                REG_SZ,
                                reinterpret_cast<const BYTE*>(wideValue.c_str()),
                                static_cast<DWORD>((wideValue.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, wideName.c_str());
        // 本来就没有这项：按成功处理，开关点两下不该报错。
        if (result == ERROR_FILE_NOT_FOUND) {
            result = ERROR_SUCCESS;
        }
    }

    RegCloseKey(key);
    if (result != ERROR_SUCCESS) {
        g_lastError = QStringLiteral("写入注册表失败：%1").arg(describeWin32(result));
        return false;
    }
    g_lastError.clear();
    return true;
}

#else

// 非 Windows 平台不提供开机自启：本项目的目标平台只有 Windows。
QString Autostart::registeredCommand()
{
    return QString();
}

bool Autostart::isEnabled()
{
    return false;
}

bool Autostart::setEnabled(bool)
{
    g_lastError = QStringLiteral("当前平台不支持开机自启");
    return false;
}

#endif // Q_OS_WIN

} // namespace patina::platform
