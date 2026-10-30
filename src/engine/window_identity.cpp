#include "engine/window_identity.h"

namespace patina::engine
{

WindowIdentity identityFromSample(const WindowSample& sample)
{
    WindowIdentity identity;
    if (sample.exeName.isEmpty()) {
        return identity;
    }

    identity.appKey = sample.exeName.toLower();

    // 优先用 root owner hwnd：应用被别的进程托管时（如 UWP 外壳），
    // 真正的窗口归属由 root owner 表达，用 hwnd 会把同一实例判成两个。
    const quintptr owner = sample.rootOwnerHwnd != 0 ? sample.rootOwnerHwnd : sample.hwnd;

    // AUMID 进实例身份。UWP 窗口被 ApplicationFrameHost 托管时，
    // root hwnd 是宿主壳的，两个不同商店应用可能撞出同样的 pid/root/class，
    // 包标识才是真正的判别物。
    const QString aumid = sample.appUserModelId.trimmed().toLower();
    identity.instanceKey = QStringLiteral("%1|pid:%2|root:%3|class:%4|aumid:%5")
                               .arg(identity.appKey)
                               .arg(sample.processId)
                               .arg(owner)
                               .arg(sample.windowClass.toLower())
                               .arg(aumid);
    return identity;
}

} // namespace patina::engine
