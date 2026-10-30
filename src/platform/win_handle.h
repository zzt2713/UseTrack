#ifndef PATINA_PLATFORM_WIN_HANDLE_H
#define PATINA_PLATFORM_WIN_HANDLE_H

// platform/ 内部的 Win32 句柄包装。
// windows.h 只允许出现在 platform/ 的实现文件里，所以本头文件不对外暴露。

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace patina::platform::detail
{

// 内核对象句柄：OpenProcess / CreateToolhelp32Snapshot 这一类。
class UniqueHandle
{
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : _handle(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : _handle(other.release()) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            _handle = other.release();
        }
        return *this;
    }

    HANDLE get() const { return _handle; }
    bool isValid() const { return _handle != nullptr && _handle != INVALID_HANDLE_VALUE; }
    explicit operator bool() const { return isValid(); }

    void reset()
    {
        if (isValid()) {
            ::CloseHandle(_handle);
        }
        _handle = nullptr;
    }

    HANDLE release()
    {
        HANDLE handle = _handle;
        _handle = nullptr;
        return handle;
    }

private:
    HANDLE _handle = nullptr;
};

// 自己创建的图标（ExtractIconExW 返回的那一类），析构时 DestroyIcon。
// 注意：WM_GETICON / GetClassLongPtrW 返回的 HICON 归窗口所有，不能销毁，
// 只能用裸 HICON 传值，不要包进本类型。
class UniqueIcon
{
public:
    UniqueIcon() = default;
    explicit UniqueIcon(HICON icon) : _icon(icon) {}
    ~UniqueIcon()
    {
        if (_icon != nullptr) {
            ::DestroyIcon(_icon);
        }
    }

    UniqueIcon(const UniqueIcon&) = delete;
    UniqueIcon& operator=(const UniqueIcon&) = delete;

    HICON get() const { return _icon; }
    explicit operator bool() const { return _icon != nullptr; }

private:
    HICON _icon = nullptr;
};

// HBITMAP：GetIconInfo 输出的 hbmMask / hbmColor 由调用方负责释放。
class UniqueBitmap
{
public:
    UniqueBitmap() = default;
    explicit UniqueBitmap(HBITMAP bitmap) : _bitmap(bitmap) {}
    ~UniqueBitmap()
    {
        if (_bitmap != nullptr) {
            ::DeleteObject(_bitmap);
        }
    }

    UniqueBitmap(const UniqueBitmap&) = delete;
    UniqueBitmap& operator=(const UniqueBitmap&) = delete;

    HBITMAP get() const { return _bitmap; }
    explicit operator bool() const { return _bitmap != nullptr; }

private:
    HBITMAP _bitmap = nullptr;
};

// GetDC 配对的屏幕 DC。
class ScreenDc
{
public:
    explicit ScreenDc(HDC dc) : _dc(dc) {}
    ~ScreenDc()
    {
        if (_dc != nullptr) {
            ::ReleaseDC(nullptr, _dc);
        }
    }

    ScreenDc(const ScreenDc&) = delete;
    ScreenDc& operator=(const ScreenDc&) = delete;

    HDC get() const { return _dc; }
    explicit operator bool() const { return _dc != nullptr; }

private:
    HDC _dc = nullptr;
};

// CreateCompatibleDC 配对的内存 DC。
class MemoryDc
{
public:
    explicit MemoryDc(HDC dc) : _dc(dc) {}
    ~MemoryDc()
    {
        if (_dc != nullptr) {
            ::DeleteDC(_dc);
        }
    }

    MemoryDc(const MemoryDc&) = delete;
    MemoryDc& operator=(const MemoryDc&) = delete;

    HDC get() const { return _dc; }
    explicit operator bool() const { return _dc != nullptr; }

private:
    HDC _dc = nullptr;
};

} // namespace patina::platform::detail

#endif // PATINA_PLATFORM_WIN_HANDLE_H
