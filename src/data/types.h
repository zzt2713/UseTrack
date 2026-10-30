#ifndef PATINA_DATA_TYPES_H
#define PATINA_DATA_TYPES_H

#include <QString>
#include <QtGlobal>

#include <optional>
#include <utility>

namespace patina::data
{

// 写操作的统一返回：error 为空即成功。
// 不用异常：调用方（engine/、ui/）必须显式处理失败，不能靠 catch 掩盖。
struct Status
{
    QString error;

    bool ok() const { return error.isEmpty(); }
    explicit operator bool() const { return ok(); }

    static Status success() { return Status{}; }
    static Status failure(QString message) { return Status{std::move(message)}; }
};

// 读操作的统一返回：error 非空时 value 无意义。
// 查不到数据用空 optional 表达，与“出错”区分开。
template <typename T>
struct Result
{
    T value{};
    QString error;

    bool ok() const { return error.isEmpty(); }
    explicit operator bool() const { return ok(); }
};

template <typename T>
Result<T> succeed(T value)
{
    Result<T> result;
    result.value = std::move(value);
    return result;
}

template <typename T>
Result<T> fail(const QString& message)
{
    Result<T> result;
    result.error = message;
    return result;
}

// 一个应用。身份是 exe_name（COLLATE NOCASE）：
// 同进程名视为同一应用，exe_path 只记录最近一次看到的路径。
//
// displayName / categoryId / colorHex / trackEnabled 是分类管理页写的用户覆盖项，
// 三者的「空值」都表示走默认：displayName 空 → 用 exe 名推导的显示名；
// categoryId 空 → 用 classifyExecutable() 的内置规则；colorHex 空 → 跟随分类色。
struct App
{
    qint64 id = 0;
    QString exeName;
    QString exePath;
    QString displayName;
    QString categoryId;
    QString colorHex;
    bool trackEnabled = true;
    // 「不在列表显示」：用户不想再看到这个应用。与 trackEnabled 不同——
    // 后者只是不进汇总，隐藏则连列表、图例、排行、提醒规则统计都不出现。
    // 历史行不动，取消隐藏后原样回来。
    bool hidden = false;
    qint64 firstSeenMs = 0;
    qint64 lastSeenMs = 0;

    bool isValid() const { return id != 0; }
};

// 一段前台活动事实。endTimeMs 为空表示这一段还开着（正在进行）。
// 时间戳一律 epoch 毫秒，与采样定时器同一口径。
//
// continuityGroupStartTimeMs 是「同一段连续使用」的起点：短时间内切走再切回
// 同一应用会复用原起点，几个碎段因此能按一段时间来看。
// 它不晚于 startTimeMs，老数据按段起点回填过。
struct SessionSegment
{
    qint64 id = 0;
    qint64 appId = 0;
    QString windowTitle;
    qint64 startTimeMs = 0;
    qint64 continuityGroupStartTimeMs = 0;
    std::optional<qint64> endTimeMs;

    bool isOpen() const { return !endTimeMs.has_value(); }

    // 未闭合段的时长没有意义，返回空。
    std::optional<qint64> durationMs() const
    {
        if (!endTimeMs.has_value()) {
            return std::nullopt;
        }
        return *endTimeMs - startTimeMs;
    }
};

// settings 表的一行。
struct SettingRow
{
    QString key;
    QString value;
};

// 一段标题样本：某个会话里标题保持不变的那段时间。
// endTimeMs 为空表示样本还在延续。
struct SessionTitleSample
{
    qint64 id = 0;
    qint64 sessionId = 0;
    QString title;
    qint64 startTimeMs = 0;
    std::optional<qint64> endTimeMs;

    std::optional<qint64> durationMs() const
    {
        if (!endTimeMs.has_value()) {
            return std::nullopt;
        }
        return *endTimeMs - startTimeMs;
    }
};

} // namespace patina::data

#endif // PATINA_DATA_TYPES_H
