// 平台采样的人工验收程序：每秒打印一次当前前台应用。
//
// 用法：patina_foreground_probe [次数] [--dump-dir <目录>]
//   省略次数表示一直采样到 Ctrl+C；
//   --dump-dir 指定目录时，每次前台切换都把应用图标存成 PNG，便于核对取图正确性。
//
// 输出：前台对象发生变化时先打一行 "=== 切换 ==="，便于肉眼确认切窗口是否采对。
// 该文件只用于验证 src/platform/ 的采样能力，不参与主程序。

#include "platform/foreground_window.h"
#include "platform/icon_extractor.h"
#include "platform/win_handle.h"

#include <QDir>
#include <QImage>
#include <QString>
#include <QTime>

#include <cstdio>
#include <cstring>

namespace
{

constexpr DWORD kSampleIntervalMs = 1000;
constexpr int kIconSize = 32;

QString formatTime()
{
    return QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
}

// 只报尺寸不足以证明图标真取到了：有些进程会返回一张全透明的占位图。
// 这里统计不透明像素占比，取到实心图标时应在 10% 以上。
QString describeIcon(const QImage& icon)
{
    if (icon.isNull()) {
        return QStringLiteral("无");
    }

    int opaque = 0;
    for (int y = 0; y < icon.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(icon.constScanLine(y));
        for (int x = 0; x < icon.width(); ++x) {
            if (qAlpha(line[x]) != 0) {
                ++opaque;
            }
        }
    }

    const int total = icon.width() * icon.height();
    const double coverage = total > 0 ? (opaque * 100.0 / total) : 0.0;
    return QStringLiteral("%1x%2, 不透明 %3%")
        .arg(icon.width())
        .arg(icon.height())
        .arg(QString::number(coverage, 'f', 1));
}

void writeLine(const QString& text)
{
    std::fputs(text.toUtf8().constData(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void printSample(const patina::platform::ForegroundWindow& sample)
{
    // 用拼接而不是 QString::arg 链：窗口标题里可能出现 "%1" 之类的片段，
    // arg 会把它们当占位符二次替换。
    QString line = QLatin1Char('[') + formatTime() + QLatin1String("] ");
    line += sample.isEmpty() ? QStringLiteral("<无前台应用>") : sample.exeName;
    line += QStringLiteral(" | 标题: ");
    line += sample.title.isEmpty() ? QStringLiteral("-") : sample.title;
    line += QStringLiteral(" | pid: ") + QString::number(sample.processId);
    line += QStringLiteral(" | class: ");
    line += sample.windowClass.isEmpty() ? QStringLiteral("-") : sample.windowClass;
    line += QStringLiteral(" | 图标: ");
    line += describeIcon(patina::platform::appIconImage(sample.exePath, kIconSize));

    writeLine(line);
}

// 切换时把图标落盘，人工确认提取结果没有偏移、没有黑底。
void dumpIcon(const patina::platform::ForegroundWindow& sample, const QString& dumpDir)
{
    if (dumpDir.isEmpty() || sample.isEmpty()) {
        return;
    }

    const QImage icon = patina::platform::appIconImage(sample.exePath, kIconSize);
    if (icon.isNull()) {
        return;
    }

    QDir dir(dumpDir);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return;
    }

    const QString base = sample.exeName.isEmpty() ? QStringLiteral("unknown") : sample.exeName;
    const QString path = dir.filePath(base + QLatin1String(".png"));
    if (icon.save(path, "PNG")) {
        writeLine(QStringLiteral("  图标已保存: ") + QDir::toNativeSeparators(path));
    }
}

} // namespace

int main(int argc, char* argv[])
{
    // 中文按 UTF-8 输出，否则默认代码页下会乱码。
    ::SetConsoleOutputCP(CP_UTF8);

    // 直接读 argv 而不是 QCoreApplication::arguments()：本程序不建 QApplication，
    // 没有实例时该静态函数取不到命令行。
    int maxSamples = 0;
    QString dumpDir;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-dir") == 0 && i + 1 < argc) {
            dumpDir = QString::fromLocal8Bit(argv[++i]);
            continue;
        }
        if (maxSamples == 0) {
            maxSamples = QString::fromLocal8Bit(argv[i]).toInt();
        }
    }

    writeLine(QStringLiteral("前台应用采样中（Ctrl+C 结束）..."));

    patina::platform::ForegroundWindow previous;
    int taken = 0;

    for (;;) {
        const patina::platform::ForegroundWindow current = patina::platform::sampleForegroundWindow();

        if (!current.hasSameIdentity(previous)) {
            writeLine(QStringLiteral("=== 切换 ==="));
            dumpIcon(current, dumpDir);
            previous = current;
        }
        printSample(current);

        ++taken;
        if (maxSamples > 0 && taken >= maxSamples) {
            break;
        }
        ::Sleep(kSampleIntervalMs);
    }

    return 0;
}
