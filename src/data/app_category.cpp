#include "data/app_category.h"

#include <QHash>
#include <QStringList>

namespace patina::data
{
namespace
{

// 分类顺序即界面图例顺序：先按「工作时间占比最高」的几类排，other 垫底。
//
// 标签用两字短名（长名留给多语言场景，本项目无 i18n）。
// other 的 #8F98A8 是 OTHER_CATEGORY_FIXED_COLOR：固定色，配置色覆盖时被 data/ 拒绝。
// system（#475569）与 anonymous（同 other 固定色）不进本表：
// system 无 exe 规则映射（C++ 侧未引入），anonymous 是显示态不是归属目标。
const QVector<CategoryToken>& tokenTable()
{
    static const QVector<CategoryToken> table = {
        {QStringLiteral("ai"),            QStringLiteral("AI"),   QStringLiteral("#3293C8")},
        {QStringLiteral("development"),   QStringLiteral("开发"), QStringLiteral("#4790CF")},
        {QStringLiteral("office"),        QStringLiteral("办公"), QStringLiteral("#6F7AE6")},
        {QStringLiteral("browser"),       QStringLiteral("浏览"), QStringLiteral("#36AC7E")},
        {QStringLiteral("communication"), QStringLiteral("通讯"), QStringLiteral("#C56A73")},
        {QStringLiteral("video"),         QStringLiteral("视频"), QStringLiteral("#66955C")},
        {QStringLiteral("music"),         QStringLiteral("音乐"), QStringLiteral("#3D9C6B")},
        {QStringLiteral("game"),          QStringLiteral("游戏"), QStringLiteral("#B07E55")},
        {QStringLiteral("design"),        QStringLiteral("设计"), QStringLiteral("#8C6FA1")},
        {QStringLiteral("utility"),       QStringLiteral("工具"), QStringLiteral("#35A69E")},
        {QStringLiteral("other"),         QStringLiteral("其他"), QStringLiteral("#8F98A8")},
    };
    return table;
}

// exe 名 → 分类。键一律小写，查表前把输入也小写，等价于 apps.exe_name 的
// COLLATE NOCASE 语义。
//
// 只收常见桌面应用，不做模糊匹配：猜错的分类比归到「其他」更让人困惑。
// 名单随使用补充，加一行就行。
const QHash<QString, QString>& ruleTable()
{
    static const QHash<QString, QString> table = [] {
        QHash<QString, QString> map;

        const auto add = [&map](const QString& category, const QStringList& names) {
            for (const QString& name : names) {
                map.insert(name, category);
            }
        };

        add(QStringLiteral("development"), {
            QStringLiteral("code.exe"),
            QStringLiteral("code - insiders.exe"),
            QStringLiteral("devenv.exe"),
            QStringLiteral("clion64.exe"),
            QStringLiteral("idea64.exe"),
            QStringLiteral("pycharm64.exe"),
            QStringLiteral("webstorm64.exe"),
            QStringLiteral("goland64.exe"),
            QStringLiteral("rider64.exe"),
            QStringLiteral("phpstorm64.exe"),
            QStringLiteral("rustrover64.exe"),
            QStringLiteral("qtcreator.exe"),
            QStringLiteral("studio64.exe"),
            QStringLiteral("android studio.exe"),
            QStringLiteral("windowsterminal.exe"),
            QStringLiteral("wt.exe"),
            QStringLiteral("cmd.exe"),
            QStringLiteral("powershell.exe"),
            QStringLiteral("pwsh.exe"),
            QStringLiteral("git-bash.exe"),
            QStringLiteral("mintty.exe"),
            QStringLiteral("wezterm-gui.exe"),
            QStringLiteral("alacritty.exe"),
            QStringLiteral("conhost.exe"),
            QStringLiteral("sublime_text.exe"),
            QStringLiteral("notepad++.exe"),
            QStringLiteral("vim.exe"),
            QStringLiteral("nvim-qt.exe"),
            QStringLiteral("emacs.exe"),
            QStringLiteral("docker desktop.exe"),
            QStringLiteral("postman.exe"),
            QStringLiteral("apifox.exe"),
            QStringLiteral("navicat.exe"),
            QStringLiteral("dbeaver.exe"),
            QStringLiteral("heidisql.exe"),
            QStringLiteral("gitkraken.exe"),
            QStringLiteral("sourcetree.exe"),
            QStringLiteral("fork.exe"),
        });

        add(QStringLiteral("browser"), {
            QStringLiteral("chrome.exe"),
            QStringLiteral("msedge.exe"),
            QStringLiteral("firefox.exe"),
            QStringLiteral("brave.exe"),
            QStringLiteral("opera.exe"),
            QStringLiteral("vivaldi.exe"),
            QStringLiteral("360se.exe"),
            QStringLiteral("360chrome.exe"),
            QStringLiteral("qqbrowser.exe"),
            QStringLiteral("sogouexplorer.exe"),
            QStringLiteral("quark.exe"),
            QStringLiteral("arc.exe"),
        });

        add(QStringLiteral("communication"), {
            QStringLiteral("wechat.exe"),
            QStringLiteral("weixin.exe"),
            QStringLiteral("qq.exe"),
            QStringLiteral("tim.exe"),
            QStringLiteral("dingtalk.exe"),
            QStringLiteral("wxwork.exe"),
            QStringLiteral("feishu.exe"),
            QStringLiteral("lark.exe"),
            QStringLiteral("telegram.exe"),
            QStringLiteral("discord.exe"),
            QStringLiteral("slack.exe"),
            QStringLiteral("teams.exe"),
            QStringLiteral("ms-teams.exe"),
            QStringLiteral("zoom.exe"),
            QStringLiteral("tencentmeeting.exe"),
            QStringLiteral("wemeetapp.exe"),
            QStringLiteral("skype.exe"),
            QStringLiteral("whatsapp.exe"),
            QStringLiteral("thunderbird.exe"),
            QStringLiteral("outlook.exe"),
            QStringLiteral("foxmail.exe"),
        });

        add(QStringLiteral("office"), {
            QStringLiteral("winword.exe"),
            QStringLiteral("excel.exe"),
            QStringLiteral("powerpnt.exe"),
            QStringLiteral("onenote.exe"),
            QStringLiteral("msaccess.exe"),
            QStringLiteral("visio.exe"),
            QStringLiteral("wps.exe"),
            QStringLiteral("et.exe"),
            QStringLiteral("wpp.exe"),
            QStringLiteral("acrobat.exe"),
            QStringLiteral("acrord32.exe"),
            QStringLiteral("sumatrapdf.exe"),
            QStringLiteral("obsidian.exe"),
            QStringLiteral("typora.exe"),
            QStringLiteral("notion.exe"),
            QStringLiteral("evernote.exe"),
            QStringLiteral("youdaonote.exe"),
            QStringLiteral("xmind.exe"),
            QStringLiteral("microsoft.mydesktop.exe"),
        });

        add(QStringLiteral("design"), {
            QStringLiteral("photoshop.exe"),
            QStringLiteral("illustrator.exe"),
            QStringLiteral("indesign.exe"),
            QStringLiteral("aftereffects.exe"),
            QStringLiteral("premiere pro.exe"),
            QStringLiteral("lightroom.exe"),
            QStringLiteral("figma.exe"),
            QStringLiteral("blender.exe"),
            QStringLiteral("sketchup.exe"),
            QStringLiteral("3dsmax.exe"),
            QStringLiteral("autocad.exe"),
            QStringLiteral("coreldrw.exe"),
            QStringLiteral("krita.exe"),
            QStringLiteral("gimp-2.10.exe"),
            QStringLiteral("inkscape.exe"),
            QStringLiteral("paint.net.exe"),
        });

        add(QStringLiteral("video"), {
            QStringLiteral("potplayermini64.exe"),
            QStringLiteral("potplayermini.exe"),
            QStringLiteral("vlc.exe"),
            QStringLiteral("mpc-hc64.exe"),
            QStringLiteral("mpc-be64.exe"),
            QStringLiteral("mpv.exe"),
            QStringLiteral("bilibili.exe"),
            QStringLiteral("iqiyi.exe"),
            QStringLiteral("qyclient.exe"),
            QStringLiteral("youku.exe"),
            QStringLiteral("qqvideo.exe"),
            QStringLiteral("tencentvideo.exe"),
            QStringLiteral("cloudmusicvideo.exe"),
            QStringLiteral("obs64.exe"),
            QStringLiteral("obs32.exe"),
            QStringLiteral("vlc media player.exe"),
            QStringLiteral("microsoft.photos.exe"),
            QStringLiteral("photosapp.exe"),
        });

        add(QStringLiteral("music"), {
            QStringLiteral("cloudmusic.exe"),
            QStringLiteral("qqmusic.exe"),
            QStringLiteral("kugou.exe"),
            QStringLiteral("kuwo.exe"),
            QStringLiteral("spotify.exe"),
            QStringLiteral("foobar2000.exe"),
            QStringLiteral("aimp.exe"),
            QStringLiteral("musicbee.exe"),
            QStringLiteral("itunes.exe"),
        });

        add(QStringLiteral("game"), {
            QStringLiteral("steam.exe"),
            QStringLiteral("steamwebhelper.exe"),
            QStringLiteral("epicgameslauncher.exe"),
            QStringLiteral("battle.net.exe"),
            QStringLiteral("wegame.exe"),
            QStringLiteral("goggalaxy.exe"),
            QStringLiteral("origin.exe"),
            QStringLiteral("eaapp.exe"),
            QStringLiteral("ubisoftconnect.exe"),
            QStringLiteral("genshinimpact.exe"),
            QStringLiteral("yuanshen.exe"),
            QStringLiteral("starrail.exe"),
        });

        add(QStringLiteral("ai"), {
            QStringLiteral("chatgpt.exe"),
            QStringLiteral("cursor.exe"),
            QStringLiteral("claude.exe"),
            QStringLiteral("ollama app.exe"),
            QStringLiteral("lm studio.exe"),
            QStringLiteral("lmstudio.exe"),
            QStringLiteral("comfyui.exe"),
            QStringLiteral("stability matrix.exe"),
            QStringLiteral("windsurf.exe"),
        });

        add(QStringLiteral("utility"), {
            QStringLiteral("explorer.exe"),
            QStringLiteral("7zfm.exe"),
            QStringLiteral("winrar.exe"),
            QStringLiteral("bandizip.exe"),
            QStringLiteral("everything.exe"),
            QStringLiteral("listary.exe"),
            QStringLiteral("powertoys.exe"),
            QStringLiteral("snipaste.exe"),
            QStringLiteral("sharex.exe"),
            QStringLiteral("pixpin.exe"),
            QStringLiteral("translucenttb.exe"),
            QStringLiteral("clash-verge.exe"),
            QStringLiteral("clash verge rev.exe"),
            QStringLiteral("v2rayn.exe"),
            QStringLiteral("rufus.exe"),
            QStringLiteral("winver.exe"),
            QStringLiteral("mspaint.exe"),
            QStringLiteral("calc.exe"),
            QStringLiteral("notepad.exe"),
            QStringLiteral("wordpad.exe"),
            QStringLiteral("snippingtool.exe"),
            QStringLiteral("quicklook.exe"),
            QStringLiteral("ditto.exe"),
        });

        return map;
    }();
    return table;
}

} // namespace

QVector<CategoryToken> categoryTokens()
{
    return tokenTable();
}

CategoryToken categoryToken(const QString& categoryId)
{
    const QVector<CategoryToken>& table = tokenTable();
    for (const CategoryToken& token : table) {
        if (token.id == categoryId) {
            return token;
        }
    }
    // 未知 id（老配置里的自定义分类等）统一落到 other，不留空标签给界面。
    return table.last();
}

QString classifyExecutable(const QString& exeName)
{
    const QString key = exeName.trimmed().toLower();
    if (key.isEmpty()) {
        return QStringLiteral("other");
    }

    const auto it = ruleTable().constFind(key);
    if (it != ruleTable().constEnd()) {
        return it.value();
    }
    return QStringLiteral("other");
}

QString resolveCategoryId(const QString& exeName, const QString& categoryIdOverride)
{
    const QString trimmed = categoryIdOverride.trimmed();
    if (!trimmed.isEmpty()) {
        return trimmed;
    }
    return classifyExecutable(exeName);
}

bool isValidColorHex(const QString& colorHex)
{
    const QString trimmed = colorHex.trimmed();
    if (trimmed.size() != 7 || trimmed.at(0) != QLatin1Char('#')) {
        return false;
    }
    for (int index = 1; index < trimmed.size(); ++index) {
        const QChar character = trimmed.at(index);
        const bool isHex = (character >= QLatin1Char('0') && character <= QLatin1Char('9'))
            || (character >= QLatin1Char('a') && character <= QLatin1Char('f'))
            || (character >= QLatin1Char('A') && character <= QLatin1Char('F'));
        if (!isHex) {
            return false;
        }
    }
    return true;
}

} // namespace patina::data
