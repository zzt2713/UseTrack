#include "platform/idle_time.h"

#include "platform/win_handle.h"

namespace patina::platform
{

qint64 idleMillis()
{
    LASTINPUTINFO lastInput = {};
    lastInput.cbSize = sizeof(LASTINPUTINFO);
    if (!::GetLastInputInfo(&lastInput)) {
        return 0;
    }

    // dwTime 与 GetTickCount 同源；用无符号相减处理 49.7 天的计数器回绕。
    const DWORD now = ::GetTickCount();
    const DWORD elapsed = now - lastInput.dwTime;
    return static_cast<qint64>(elapsed);
}

} // namespace patina::platform
