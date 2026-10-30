#ifndef PATINA_ENGINE_WINDOW_IDENTITY_H
#define PATINA_ENGINE_WINDOW_IDENTITY_H

#include "engine/window_sample.h"

#include <QString>

namespace patina::engine
{

// 一次采样的两层身份：
//
//   appKey      —— exe 名小写。决定“换没换应用”，换应用就换 session 段。
//   instanceKey —— appKey + pid + root hwnd + class。决定“同一个应用里换没换
//                  窗口实例”，只换实例不换应用时只刷新标题，不切段。
struct WindowIdentity
{
    QString appKey;
    QString instanceKey;

    bool isValid() const { return !appKey.isEmpty(); }
    bool isSameApp(const WindowIdentity& other) const { return appKey == other.appKey; }
    bool isSameInstance(const WindowIdentity& other) const { return instanceKey == other.instanceKey; }
};

// exe 名为空时返回无效身份。
WindowIdentity identityFromSample(const WindowSample& sample);

} // namespace patina::engine

#endif // PATINA_ENGINE_WINDOW_IDENTITY_H
