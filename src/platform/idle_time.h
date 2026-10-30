#ifndef PATINA_PLATFORM_IDLE_TIME_H
#define PATINA_PLATFORM_IDLE_TIME_H

#include <QtGlobal>

namespace patina::platform
{

// 距用户最后一次键鼠输入的毫秒数（GetLastInputInfo）。
//
// 这是 AFK 判定的唯一依据。阈值在 engine/ 的 TrackingPolicy
// 里，本函数只报原始值，不判断、不解释。
//
// 调用失败返回 0：宁可把用户当成"正在活动"，也不要因为一次 API 失败就凭空切段。
qint64 idleMillis();

} // namespace patina::platform

#endif // PATINA_PLATFORM_IDLE_TIME_H
