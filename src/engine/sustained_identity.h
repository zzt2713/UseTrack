#ifndef PATINA_ENGINE_SUSTAINED_IDENTITY_H
#define PATINA_ENGINE_SUSTAINED_IDENTITY_H

#include <QString>

namespace patina::engine
{

// 能产出「持续参与」信号的应用。
//
// 为什么要枚举而不是通配：媒体/音频信号只能拿到 AUMID 或进程名，
// 拿不到窗口句柄，没法直接和前台窗口对齐。只有先认出「这是 Chrome 在放视频」，
// 才能判断它和当前前台的 chrome.exe 是不是同一件事。
enum class SustainedApp
{
    None,
    Chrome,
    Edge,
    Firefox,
    Brave,
    Zoom,
    Teams,
    Vlc,
    Bilibili,
    Douyin,
    WeMeet
};

// 信号的来源。媒体优先、音频兜底：SMTC 能明确区分「播放中/已暂停」，
// WASAPI 只知道「这个进程有活跃音频会话」，粒度更粗，所以放后面。
enum class SustainedSignalSource
{
    None,
    SystemMedia,
    AudioSession
};

// 探测结果的中立表示。platform/ 只填事实，判定留给 engine/。
struct SustainedSignal
{
    // false 表示这一路信号源当前不可用（探测失败/超时/未初始化），
    // 与「可用但没在放」是两件事，状态机要区分。
    bool available = false;
    bool active = false;
    SustainedSignalSource source = SustainedSignalSource::None;
    // 媒体：AUMID（形如 Microsoft.ZuneVideo_...）；音频：进程 exe 名。
    QString sourceAppId;
    // 音频会话能拿到真实进程，媒体会话拿不到。
    QString exeName;
    QString exePath;
};

SustainedApp appIdentityFromProcess(const QString& exeName, const QString& exePath);
SustainedApp appIdentityFromSourceAppId(const QString& sourceAppId);

// 身份键：认得出应用就用应用名，认不出就退回进程路径的文件名，再退回 exe 名。
// 用来判断「信号丢失时前台还是不是原来那个应用」——只有同一身份才给宽限。
QString resolveSustainedIdentityKey(const QString& exeName, const QString& exePath);

bool sourceAppIdMatchesWindow(const QString& sourceAppId,
                              const QString& exeName,
                              const QString& exePath);

// 信号确实来自当前前台窗口对应的那个应用。
bool signalOriginMatchesWindow(const SustainedSignal& signal,
                               const QString& exeName,
                               const QString& exePath);

// 正在播放，且是这个窗口在播。
bool signalMatchesWindow(const SustainedSignal& signal,
                         const QString& exeName,
                         const QString& exePath);

// 明确停了（不是「探测不到」）：可用 + 非活跃 + 来源对得上。
// 这一条会跳过宽限期——用户按了暂停，没必要再等 12 秒。
bool signalExplicitlyStopped(const SustainedSignal& signal,
                             const QString& exeName,
                             const QString& exePath);

} // namespace patina::engine

#endif // PATINA_ENGINE_SUSTAINED_IDENTITY_H
