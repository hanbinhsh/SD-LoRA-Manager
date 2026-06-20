#pragma once

#include <QColor>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QString>

namespace AppStyle {

// === 颜色调色板（单一事实源）===
// QSS 用 @token / @rgba(token, A) 占位，加载时由 applyTokens 替换为下面的默认(Steam Dark)值。
// Round 2 做主题时，只需提供另一套同名 token 的调色板即可整体换肤。
inline const QHash<QString, QString> &defaultPalette()
{
    static const QHash<QString, QString> p = {
        { QStringLiteral("windowBg"), QStringLiteral("#1b2838") },
        { QStringLiteral("sidebarBg"), QStringLiteral("#1a1f29") },
        { QStringLiteral("panelBg"), QStringLiteral("#1e252f") },
        { QStringLiteral("headerBg"), QStringLiteral("#212831") },
        { QStringLiteral("inputBg"), QStringLiteral("#16191e") },
        { QStringLiteral("consoleBg"), QStringLiteral("#131519") },
        { QStringLiteral("scrollTrack"), QStringLiteral("#12161c") },
        { QStringLiteral("downloadFieldBg"), QStringLiteral("#0f141a") },
        { QStringLiteral("downloadCardBg"), QStringLiteral("#131922") },
        { QStringLiteral("progressBg"), QStringLiteral("#111a26") },
        { QStringLiteral("downloadCardHoverBg"), QStringLiteral("#1b222b") },
        { QStringLiteral("downloadCardSelBg"), QStringLiteral("#20344a") },
        { QStringLiteral("sliderGroove"), QStringLiteral("#27313d") },
        { QStringLiteral("disabledBg"), QStringLiteral("#2a3038") },
        { QStringLiteral("templateHoverBg"), QStringLiteral("#2a3442") },
        { QStringLiteral("buttonBg"), QStringLiteral("#2a3f5a") },
        { QStringLiteral("previewBorder"), QStringLiteral("#2b3440") },
        { QStringLiteral("inputBorder"), QStringLiteral("#31363d") },
        { QStringLiteral("sidebarTabBg"), QStringLiteral("#313a46") },
        { QStringLiteral("downloadBorder"), QStringLiteral("#313d49") },
        { QStringLiteral("consoleBorder"), QStringLiteral("#363c46") },
        { QStringLiteral("scrollHandle"), QStringLiteral("#3a4654") },
        { QStringLiteral("dangerBg"), QStringLiteral("#3c1919") },
        { QStringLiteral("hoverBg"), QStringLiteral("#3d4450") },
        { QStringLiteral("groupBorder"), QStringLiteral("#3d4d5d") },
        { QStringLiteral("selectionBg"), QStringLiteral("#3d5677") },
        { QStringLiteral("scrollHandleHover"), QStringLiteral("#4b5a6b") },
        { QStringLiteral("downloadHoverBorder"), QStringLiteral("#4b637a") },
        { QStringLiteral("copyBtnPressed"), QStringLiteral("#4c6b0c") },
        { QStringLiteral("placeholderText"), QStringLiteral("#5a6f8a") },
        { QStringLiteral("copyBtnBg"), QStringLiteral("#5c7e10") },
        { QStringLiteral("checkboxBorder"), QStringLiteral("#66717f") },
        { QStringLiteral("accentBlue"), QStringLiteral("#66c0f4") },
        { QStringLiteral("copyBtnHover"), QStringLiteral("#79a615") },
        { QStringLiteral("successGreenBright"), QStringLiteral("#7ee081") },
        { QStringLiteral("disabledText"), QStringLiteral("#7f8791") },
        { QStringLiteral("mutedText2"), QStringLiteral("#8c929a") },
        { QStringLiteral("mutedText"), QStringLiteral("#8c96a0") },
        { QStringLiteral("accentBright"), QStringLiteral("#90d0ff") },
        { QStringLiteral("htmlSubtle"), QStringLiteral("#aaaaaa") },
        { QStringLiteral("subtleText"), QStringLiteral("#acb2b8") },
        { QStringLiteral("downloadMeta"), QStringLiteral("#aeb7c2") },
        { QStringLiteral("bodyText"), QStringLiteral("#dcdedf") },
        { QStringLiteral("userTagChipText"), QStringLiteral("#e4ffe9") },
        { QStringLiteral("inputBorderBright"), QStringLiteral("#eeeeee") },
        { QStringLiteral("errorRed"), QStringLiteral("#ff4c4c") },
        { QStringLiteral("chipMixedText"), QStringLiteral("#fff1c7") },
        { QStringLiteral("whiteText"), QStringLiteral("#ffffff") },
        { QStringLiteral("black"), QStringLiteral("#000000") },
        { QStringLiteral("tagGreenBg"), QStringLiteral("#1f3520") },
        { QStringLiteral("chipYellow"), QStringLiteral("#ffcc66") },
        { QStringLiteral("successGreen"), QStringLiteral("#5fd38d") },
        // 语义前景/边框 token（取代 QSS 里的 3 位简写色，Steam 值与原字面量一致）
        { QStringLiteral("primaryText"), QStringLiteral("#ffffff") },   // 标题/侧栏/分区/输入等主前景
        { QStringLiteral("onButtonText"), QStringLiteral("#ffffff") },  // @buttonBg 上的按钮文字
        { QStringLiteral("onAccentText"), QStringLiteral("#000000") },  // 强调/选中底上的文字
        { QStringLiteral("tabText"), QStringLiteral("#cccccc") },       // 标签未选中文字
        { QStringLiteral("thumbBorder"), QStringLiteral("#333333") },   // 缩略图/收藏按钮边框
        { QStringLiteral("thumbBorderHover"), QStringLiteral("#888888") },
        { QStringLiteral("hintText"), QStringLiteral("#666666") },      // 提示/状态小字
        { QStringLiteral("paneBorder"), QStringLiteral("#444444") },    // Tab pane 边框
        { QStringLiteral("overlayFg"), QStringLiteral("#ffffff") },     // 半透明叠加按钮/输入的前景（浅色主题翻转为深色）
    };
    return p;
}

// === 主题调色板（只需提供与 defaultPalette 同名的 token；缺省项回退到 Steam Dark）===
// Light：浅色方案
inline const QHash<QString, QString> &lightPalette()
{
    static const QHash<QString, QString> p = {
        { QStringLiteral("windowBg"), QStringLiteral("#eef2f7") },
        { QStringLiteral("sidebarBg"), QStringLiteral("#e3e9f1") },
        { QStringLiteral("panelBg"), QStringLiteral("#ffffff") },
        { QStringLiteral("headerBg"), QStringLiteral("#dde6f1") },
        { QStringLiteral("inputBg"), QStringLiteral("#ffffff") },
        { QStringLiteral("consoleBg"), QStringLiteral("#f3f6fa") },
        { QStringLiteral("scrollTrack"), QStringLiteral("#e3e9f1") },
        { QStringLiteral("downloadFieldBg"), QStringLiteral("#f7fafc") },
        { QStringLiteral("downloadCardBg"), QStringLiteral("#eef3f9") },
        { QStringLiteral("progressBg"), QStringLiteral("#e3e9f1") },
        { QStringLiteral("downloadCardHoverBg"), QStringLiteral("#eaf0f7") },
        { QStringLiteral("downloadCardSelBg"), QStringLiteral("#d8e6fb") },
        { QStringLiteral("sliderGroove"), QStringLiteral("#cdd7e3") },
        { QStringLiteral("disabledBg"), QStringLiteral("#e6ebf1") },
        { QStringLiteral("templateHoverBg"), QStringLiteral("#eaf0f7") },
        { QStringLiteral("buttonBg"), QStringLiteral("#e8eef6") },
        { QStringLiteral("previewBorder"), QStringLiteral("#cdd7e3") },
        { QStringLiteral("inputBorder"), QStringLiteral("#c2cedd") },
        { QStringLiteral("sidebarTabBg"), QStringLiteral("#d6deea") },
        { QStringLiteral("downloadBorder"), QStringLiteral("#c8d3e1") },
        { QStringLiteral("consoleBorder"), QStringLiteral("#c2cedd") },
        { QStringLiteral("scrollHandle"), QStringLiteral("#b8c4d4") },
        { QStringLiteral("dangerBg"), QStringLiteral("#fde2e2") },
        { QStringLiteral("hoverBg"), QStringLiteral("#d8e2f0") },
        { QStringLiteral("groupBorder"), QStringLiteral("#c2cedd") },
        { QStringLiteral("selectionBg"), QStringLiteral("#2f80ed") },
        { QStringLiteral("scrollHandleHover"), QStringLiteral("#9aa9bd") },
        { QStringLiteral("downloadHoverBorder"), QStringLiteral("#2f80ed") },
        { QStringLiteral("copyBtnPressed"), QStringLiteral("#4c6b0c") },
        { QStringLiteral("placeholderText"), QStringLiteral("#8090a4") },
        { QStringLiteral("copyBtnBg"), QStringLiteral("#5c7e10") },
        { QStringLiteral("checkboxBorder"), QStringLiteral("#9aa9bd") },
        { QStringLiteral("accentBlue"), QStringLiteral("#2f80ed") },
        { QStringLiteral("copyBtnHover"), QStringLiteral("#6f8f10") },
        { QStringLiteral("successGreenBright"), QStringLiteral("#1f9d57") },
        { QStringLiteral("disabledText"), QStringLiteral("#9aa6b4") },
        { QStringLiteral("mutedText2"), QStringLiteral("#5e6b7e") },
        { QStringLiteral("mutedText"), QStringLiteral("#5e6b7e") },
        { QStringLiteral("accentBright"), QStringLiteral("#1c6fe0") },
        { QStringLiteral("htmlSubtle"), QStringLiteral("#6b7686") },
        { QStringLiteral("subtleText"), QStringLiteral("#44505f") },
        { QStringLiteral("downloadMeta"), QStringLiteral("#44505f") },
        { QStringLiteral("bodyText"), QStringLiteral("#172033") },
        { QStringLiteral("userTagChipText"), QStringLiteral("#14532d") },
        { QStringLiteral("inputBorderBright"), QStringLiteral("#2f80ed") },
        { QStringLiteral("errorRed"), QStringLiteral("#d92d2d") },
        { QStringLiteral("chipMixedText"), QStringLiteral("#7a5800") },
        { QStringLiteral("whiteText"), QStringLiteral("#ffffff") },
        { QStringLiteral("black"), QStringLiteral("#000000") },
        { QStringLiteral("tagGreenBg"), QStringLiteral("#d6f5e0") },
        { QStringLiteral("chipYellow"), QStringLiteral("#b8860b") },
        { QStringLiteral("successGreen"), QStringLiteral("#1f9d57") },
        { QStringLiteral("primaryText"), QStringLiteral("#18233a") },
        { QStringLiteral("onButtonText"), QStringLiteral("#1a2a44") },
        { QStringLiteral("onAccentText"), QStringLiteral("#ffffff") },
        { QStringLiteral("tabText"), QStringLiteral("#51607a") },
        { QStringLiteral("thumbBorder"), QStringLiteral("#c2cedd") },
        { QStringLiteral("thumbBorderHover"), QStringLiteral("#8aa0b8") },
        { QStringLiteral("hintText"), QStringLiteral("#7a8696") },
        { QStringLiteral("paneBorder"), QStringLiteral("#c2cedd") },
        { QStringLiteral("overlayFg"), QStringLiteral("#1a2540") },
    };
    return p;
}

// Midnight Blue：偏蓝深色方案
inline const QHash<QString, QString> &midnightPalette()
{
    static const QHash<QString, QString> p = {
        { QStringLiteral("windowBg"), QStringLiteral("#101927") },
        { QStringLiteral("sidebarBg"), QStringLiteral("#0c1320") },
        { QStringLiteral("panelBg"), QStringLiteral("#111f31") },
        { QStringLiteral("headerBg"), QStringLiteral("#142338") },
        { QStringLiteral("inputBg"), QStringLiteral("#0f1a2b") },
        { QStringLiteral("consoleBg"), QStringLiteral("#0c1626") },
        { QStringLiteral("scrollTrack"), QStringLiteral("#0c1626") },
        { QStringLiteral("downloadFieldBg"), QStringLiteral("#0a1422") },
        { QStringLiteral("downloadCardBg"), QStringLiteral("#0b1525") },
        { QStringLiteral("progressBg"), QStringLiteral("#0c1626") },
        { QStringLiteral("downloadCardHoverBg"), QStringLiteral("#16273d") },
        { QStringLiteral("downloadCardSelBg"), QStringLiteral("#1c3553") },
        { QStringLiteral("sliderGroove"), QStringLiteral("#1b2c44") },
        { QStringLiteral("disabledBg"), QStringLiteral("#172638") },
        { QStringLiteral("templateHoverBg"), QStringLiteral("#1a2c44") },
        { QStringLiteral("buttonBg"), QStringLiteral("#1e4069") },
        { QStringLiteral("previewBorder"), QStringLiteral("#24405f") },
        { QStringLiteral("inputBorder"), QStringLiteral("#26405f") },
        { QStringLiteral("sidebarTabBg"), QStringLiteral("#1a2c44") },
        { QStringLiteral("downloadBorder"), QStringLiteral("#24405f") },
        { QStringLiteral("consoleBorder"), QStringLiteral("#26405f") },
        { QStringLiteral("scrollHandle"), QStringLiteral("#2c466a") },
        { QStringLiteral("dangerBg"), QStringLiteral("#3a1820") },
        { QStringLiteral("hoverBg"), QStringLiteral("#1e3654") },
        { QStringLiteral("groupBorder"), QStringLiteral("#2a466a") },
        { QStringLiteral("selectionBg"), QStringLiteral("#2563eb") },
        { QStringLiteral("scrollHandleHover"), QStringLiteral("#3a5a82") },
        { QStringLiteral("downloadHoverBorder"), QStringLiteral("#3b82f6") },
        { QStringLiteral("copyBtnPressed"), QStringLiteral("#4c6b0c") },
        { QStringLiteral("placeholderText"), QStringLiteral("#5a78a0") },
        { QStringLiteral("copyBtnBg"), QStringLiteral("#5c7e10") },
        { QStringLiteral("checkboxBorder"), QStringLiteral("#3a5a82") },
        { QStringLiteral("accentBlue"), QStringLiteral("#38bdf8") },
        { QStringLiteral("copyBtnHover"), QStringLiteral("#79a615") },
        { QStringLiteral("successGreenBright"), QStringLiteral("#34d399") },
        { QStringLiteral("disabledText"), QStringLiteral("#5a6b80") },
        { QStringLiteral("mutedText2"), QStringLiteral("#8aa2c0") },
        { QStringLiteral("mutedText"), QStringLiteral("#8aa2c0") },
        { QStringLiteral("accentBright"), QStringLiteral("#7dd3fc") },
        { QStringLiteral("htmlSubtle"), QStringLiteral("#8aa2c0") },
        { QStringLiteral("subtleText"), QStringLiteral("#c3d4e8") },
        { QStringLiteral("downloadMeta"), QStringLiteral("#c3d4e8") },
        { QStringLiteral("bodyText"), QStringLiteral("#dbeafe") },
        { QStringLiteral("userTagChipText"), QStringLiteral("#bbf7d0") },
        { QStringLiteral("inputBorderBright"), QStringLiteral("#7dd3fc") },
        { QStringLiteral("errorRed"), QStringLiteral("#f87171") },
        { QStringLiteral("chipMixedText"), QStringLiteral("#fde68a") },
        { QStringLiteral("whiteText"), QStringLiteral("#ffffff") },
        { QStringLiteral("black"), QStringLiteral("#000000") },
        { QStringLiteral("tagGreenBg"), QStringLiteral("#14351f") },
        { QStringLiteral("chipYellow"), QStringLiteral("#fbbf24") },
        { QStringLiteral("successGreen"), QStringLiteral("#34d399") },
        { QStringLiteral("primaryText"), QStringLiteral("#eef4ff") },
        { QStringLiteral("onButtonText"), QStringLiteral("#eaf2ff") },
        { QStringLiteral("onAccentText"), QStringLiteral("#ffffff") },
        { QStringLiteral("tabText"), QStringLiteral("#9fb3cf") },
        { QStringLiteral("thumbBorder"), QStringLiteral("#24405f") },
        { QStringLiteral("thumbBorderHover"), QStringLiteral("#5a7aa0") },
        { QStringLiteral("hintText"), QStringLiteral("#6f88a8") },
        { QStringLiteral("paneBorder"), QStringLiteral("#26405f") },
        { QStringLiteral("overlayFg"), QStringLiteral("#ffffff") },
    };
    return p;
}

// High Contrast：黑底白字 + 黄色强调
inline const QHash<QString, QString> &highContrastPalette()
{
    static const QHash<QString, QString> p = {
        { QStringLiteral("windowBg"), QStringLiteral("#000000") },
        { QStringLiteral("sidebarBg"), QStringLiteral("#000000") },
        { QStringLiteral("panelBg"), QStringLiteral("#050505") },
        { QStringLiteral("headerBg"), QStringLiteral("#0a0a0a") },
        { QStringLiteral("inputBg"), QStringLiteral("#000000") },
        { QStringLiteral("consoleBg"), QStringLiteral("#000000") },
        { QStringLiteral("scrollTrack"), QStringLiteral("#0a0a0a") },
        { QStringLiteral("downloadFieldBg"), QStringLiteral("#000000") },
        { QStringLiteral("downloadCardBg"), QStringLiteral("#050505") },
        { QStringLiteral("progressBg"), QStringLiteral("#0a0a0a") },
        { QStringLiteral("downloadCardHoverBg"), QStringLiteral("#1a1a00") },
        { QStringLiteral("downloadCardSelBg"), QStringLiteral("#333300") },
        { QStringLiteral("sliderGroove"), QStringLiteral("#333333") },
        { QStringLiteral("disabledBg"), QStringLiteral("#1a1a1a") },
        { QStringLiteral("templateHoverBg"), QStringLiteral("#1a1a00") },
        { QStringLiteral("buttonBg"), QStringLiteral("#000000") },
        { QStringLiteral("previewBorder"), QStringLiteral("#ffff66") },
        { QStringLiteral("inputBorder"), QStringLiteral("#ffff66") },
        { QStringLiteral("sidebarTabBg"), QStringLiteral("#1a1a1a") },
        { QStringLiteral("downloadBorder"), QStringLiteral("#ffffff") },
        { QStringLiteral("consoleBorder"), QStringLiteral("#ffff66") },
        { QStringLiteral("scrollHandle"), QStringLiteral("#888844") },
        { QStringLiteral("dangerBg"), QStringLiteral("#330000") },
        { QStringLiteral("hoverBg"), QStringLiteral("#333300") },
        { QStringLiteral("groupBorder"), QStringLiteral("#ffffff") },
        { QStringLiteral("selectionBg"), QStringLiteral("#ffff66") },
        { QStringLiteral("scrollHandleHover"), QStringLiteral("#ffff66") },
        { QStringLiteral("downloadHoverBorder"), QStringLiteral("#ffff66") },
        { QStringLiteral("copyBtnPressed"), QStringLiteral("#888800") },
        { QStringLiteral("placeholderText"), QStringLiteral("#cccc66") },
        { QStringLiteral("copyBtnBg"), QStringLiteral("#aaaa00") },
        { QStringLiteral("checkboxBorder"), QStringLiteral("#ffff66") },
        { QStringLiteral("accentBlue"), QStringLiteral("#ffff66") },
        { QStringLiteral("copyBtnHover"), QStringLiteral("#cccc00") },
        { QStringLiteral("successGreenBright"), QStringLiteral("#66ff66") },
        { QStringLiteral("disabledText"), QStringLiteral("#888888") },
        { QStringLiteral("mutedText2"), QStringLiteral("#cccccc") },
        { QStringLiteral("mutedText"), QStringLiteral("#cccccc") },
        { QStringLiteral("accentBright"), QStringLiteral("#ffffaa") },
        { QStringLiteral("htmlSubtle"), QStringLiteral("#cccccc") },
        { QStringLiteral("subtleText"), QStringLiteral("#ffffff") },
        { QStringLiteral("downloadMeta"), QStringLiteral("#ffffff") },
        { QStringLiteral("bodyText"), QStringLiteral("#ffffff") },
        { QStringLiteral("userTagChipText"), QStringLiteral("#66ff66") },
        { QStringLiteral("inputBorderBright"), QStringLiteral("#ffffaa") },
        { QStringLiteral("errorRed"), QStringLiteral("#ff6666") },
        { QStringLiteral("chipMixedText"), QStringLiteral("#ffff66") },
        { QStringLiteral("whiteText"), QStringLiteral("#000000") },
        { QStringLiteral("black"), QStringLiteral("#000000") },
        { QStringLiteral("tagGreenBg"), QStringLiteral("#003300") },
        { QStringLiteral("chipYellow"), QStringLiteral("#ffff66") },
        { QStringLiteral("successGreen"), QStringLiteral("#66ff66") },
        { QStringLiteral("primaryText"), QStringLiteral("#ffffff") },
        { QStringLiteral("onButtonText"), QStringLiteral("#ffffff") },
        { QStringLiteral("onAccentText"), QStringLiteral("#000000") },
        { QStringLiteral("tabText"), QStringLiteral("#cccccc") },
        { QStringLiteral("thumbBorder"), QStringLiteral("#ffff66") },
        { QStringLiteral("thumbBorderHover"), QStringLiteral("#ffffaa") },
        { QStringLiteral("hintText"), QStringLiteral("#cccc66") },
        { QStringLiteral("paneBorder"), QStringLiteral("#ffffff") },
        { QStringLiteral("overlayFg"), QStringLiteral("#ffffff") },
    };
    return p;
}

inline const QHash<QString, QString> &paletteForTheme(const QString &themeId)
{
    if (themeId == QLatin1String("light")) return lightPalette();
    if (themeId == QLatin1String("midnight_blue")) return midnightPalette();
    if (themeId == QLatin1String("high_contrast")) return highContrastPalette();
    return defaultPalette(); // steam_dark / custom_qss / 未知
}

// 当前活动调色板（仅主线程读写）。
inline const QHash<QString, QString> *&activePalettePtr()
{
    static const QHash<QString, QString> *p = &defaultPalette();
    return p;
}
inline const QHash<QString, QString> &activePalette() { return *activePalettePtr(); }
inline void setActiveTheme(const QString &themeId) { activePalettePtr() = &paletteForTheme(themeId); }

// 取 token 的字符串值 / QColor：先查活动调色板，缺省回退 Steam Dark。
inline QString str(const QString &token)
{
    const QString v = activePalette().value(token);
    return v.isEmpty() ? defaultPalette().value(token) : v;
}
inline QColor color(const QString &token) { return QColor(str(token)); }

// 把 QSS 里的 @token 与 @rgba(token, A) 占位符替换成调色板里的实际颜色。
inline QString applyTokens(QString qss)
{
    // 1) @rgba(token, A) -> rgba(r,g,b,A)
    static const QRegularExpression rgbaRe(QStringLiteral("@rgba\\(\\s*(\\w+)\\s*,\\s*(\\d+)\\s*\\)"));
    {
        QString out;
        out.reserve(qss.size());
        qsizetype last = 0;
        auto it = rgbaRe.globalMatch(qss);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            out += qss.mid(last, m.capturedStart() - last);
            const QColor c = color(m.captured(1));
            out += QStringLiteral("rgba(%1,%2,%3,%4)")
                       .arg(c.red()).arg(c.green()).arg(c.blue()).arg(m.captured(2));
            last = m.capturedEnd();
        }
        out += qss.mid(last);
        qss = out;
    }
    // 2) @token -> value（未知 token 原样保留）
    static const QRegularExpression tokenRe(QStringLiteral("@(\\w+)"));
    {
        QString out;
        out.reserve(qss.size());
        qsizetype last = 0;
        auto it = tokenRe.globalMatch(qss);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            out += qss.mid(last, m.capturedStart() - last);
            const QString val = str(m.captured(1));
            out += val.isEmpty() ? m.captured(0) : val;
            last = m.capturedEnd();
        }
        out += qss.mid(last);
        qss = out;
    }
    return qss;
}

// 读取一个 QSS 资源文件内容并完成 token 替换（失败时返回空串）。
inline QString loadQss(const QString &resourcePath)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return applyTokens(QString::fromUtf8(file.readAll()));
}

// 读取工具页所需的完整 QSS（公共 base + 工具页专属），均已 token 替换。
inline QString loadToolPageQss()
{
    return loadQss(QStringLiteral(":/styles/base.qss")) + QChar('\n')
         + loadQss(QStringLiteral(":/styles/toolpage.qss"));
}

inline constexpr const char *AccentBlue = "#66c0f4";
inline constexpr const char *CustomTriggerGreen = "#5fd38d";
inline constexpr const char *MutedText = "#8c96a0";
inline constexpr const char *BodyText = "#dcdedf";
inline constexpr const char *WhiteText = "#ffffff";
inline constexpr const char *WarningYellow = "#ffcc00";
inline constexpr const char *ErrorRed = "#ff4c4c";
inline constexpr const char *SoftErrorRed = "#ff6b6b";
inline constexpr const char *SoftErrorText = "#ff7b7b";
inline constexpr const char *PanelDark = "#25282f";
inline constexpr const char *PanelBorder = "#3d4450";
inline constexpr const char *PanelBorderSoft = "#3a4654";
inline constexpr const char *PlaceholderText = "#5a6f8a";
inline constexpr const char *HeaderBackground = "#212831";
inline constexpr const char *HtmlNegative = "#ff6666";
inline constexpr const char *HtmlSubtle = "#aaaaaa";
inline constexpr const char *HtmlDim = "#888888";
inline constexpr const char *MainBackground = "#1b2838";
inline constexpr const char *SidebarDark = "#1a1f29";
inline constexpr const char *DownloadCardBackground = "#131922";
inline constexpr const char *TableSelected = "#3d5677";
inline constexpr const char *WorkflowBackground = "#101824";
inline constexpr const char *WorkflowEdge = "#50667d";
inline constexpr const char *WorkflowEdgeHighlight = "#8fc7ff";
inline constexpr const char *WorkflowNodeBorder = "#6b7b8f";
inline constexpr const char *WorkflowTitleText = "#f1f6ff";
inline constexpr const char *WorkflowTypeText = "#a8bdd3";
inline constexpr const char *WorkflowSubtitleText = "#d8e1ea";
inline constexpr const char *SyncStopRed = "#aa3333";
inline constexpr const char *ChatUserBubbleBg = "#27425f";
inline constexpr const char *ChatAssistantBubbleBg = "#1f2834";
inline constexpr const char *ChatUserBubbleBorder = "#3f6b95";
inline constexpr const char *ChatAssistantBubbleBorder = "#3a4654";
inline constexpr const char *ChatUserText = "#eaf4ff";
inline constexpr const char *ChatActionText = "#b9c4d0";

inline constexpr const char *MutedLabelStyle =
    "color:#8c96a0;background:transparent;";

inline constexpr const char *MutedBoldLabelStyle =
    "color:#8c96a0;background:transparent;font-weight:bold;";

inline constexpr const char *UserTagChipInlineStyle =
    "padding: 2px 8px; border-radius: 8px; "
    "background: rgba(95,211,141,48); color: #dfffea;";

inline constexpr const char *FilterTagSourceModel = "model";
inline constexpr const char *FilterTagSourceUser = "user";
inline constexpr const char *FilterTagSourceMixed = "mixed";

inline QColor translucentWhite(int alpha)
{
    return QColor(255, 255, 255, alpha);
}

inline QColor imageCompareOnlyA()
{
    return QColor(122, 74, 42, 120);
}

inline QColor imageCompareOnlyB()
{
    return QColor(47, 106, 79, 120);
}

inline QColor workflowNodeColor(const QString &type, bool highlighted)
{
    const QString lower = type.toLower();
    if (highlighted) return QColor("#315f89");
    if (lower.contains("ksampler")) return QColor("#324b63");
    if (lower.contains("cliptextencode") || lower.contains("text")) return QColor("#3d4f3a");
    if (lower.contains("loraloader")) return QColor("#5a4634");
    if (lower.contains("checkpointloader")) return QColor("#4f3b5f");
    return QColor("#253446");
}

inline QString modelTitleNormalStyle()
{
    return QStringLiteral(
        "color: #fff;"
        "background-color: rgba(0,0,0,120);"
        "padding: 15px;"
        "border-left: 5px solid %1;"
        "font-size: 24px;"
        "font-weight: bold;").arg(AccentBlue);
}

inline QString modelTitleErrorStyle()
{
    return QStringLiteral(
        "color: %1;"
        "background-color: rgba(45, 20, 20, 0.8);"
        "border-left: 5px solid #ff0000;"
        "padding: 15px;"
        "font-size: 15px;").arg(ErrorRed);
}

inline QString globalGalleryTitleStyle()
{
    return QStringLiteral(
        "color: #fff;"
        "background-color: rgba(0,0,0,120);"
        "padding: 15px;"
        "border-left: 5px solid %1;"
        "font-size: 24px;"
        "font-weight: bold;").arg(WarningYellow);
}

inline QColor steamBackground(int alpha = 255)
{
    QColor c = color("windowBg"); // 随主题变化（Hero 渐变叠加层）
    c.setAlpha(alpha);
    return c;
}

inline QString chatStatusStyle(bool isError)
{
    return QStringLiteral("color:%1;").arg(isError ? SoftErrorText : MutedText);
}

inline QString dangerButtonStyle()
{
    return QStringLiteral(
        "QPushButton { background-color: #3c1919; color: %1; border: none; padding: 4px 8px; border-radius: 2px; }"
        "QPushButton:hover { background-color: %1; color: #fff; }").arg(ErrorRed);
}

inline QString allowButtonStyle()
{
    return QStringLiteral(
        "QPushButton { background-color: #1b4b2a; color: #8fbc8f; border: none; padding: 4px 8px; border-radius: 2px; }"
        "QPushButton:hover { background-color: #2e8b57; color: #fff; }");
}

inline QString chatThumbStyle()
{
    return QStringLiteral("QPushButton{background:#11161c;border:1px solid %1;border-radius:6px;color:%2;padding:2px;}")
        .arg(PanelBorderSoft, MutedText);
}

inline QString chatActionButtonStyle(const QString &textColor)
{
    return QStringLiteral(
        "QPushButton{background:#223041;border:1px solid #3a4b60;border-radius:4px;padding:2px 8px;color:%1;}"
        "QPushButton:hover{background:#2d3f56;color:#ffffff;}").arg(textColor);
}

inline QString chatFooterButtonStyle()
{
    return QStringLiteral(
        "QPushButton{background:#18212b;border:1px solid #344254;border-radius:4px;color:%1;padding:2px 7px;font-size:11px;}"
        "QPushButton:hover{background:#263447;color:#ffffff;}"
        "QPushButton:disabled{background:#171b20;border-color:#2a3038;color:#66707c;}").arg(ChatActionText);
}

inline QString chatThinkingBrowserStyle()
{
    return QStringLiteral(
        "QTextBrowser{border:1px solid #2e3742;border-radius:6px;background:#11161c;color:%1;padding:6px;}"
        "QScrollBar:vertical{width:4px;background:transparent;}"
        "QScrollBar::handle:vertical{background:#5f6f80;min-height:20px;border-radius:2px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0px;}").arg(BodyText);
}

inline QString chatBodyBrowserStyle(const QString &textColor)
{
    return QStringLiteral(
        "QTextBrowser{border:none;background:transparent;color:%1;font-size:13px;padding:0px;}"
        "QScrollBar:vertical{background:transparent;width:4px;margin:0px;}"
        "QScrollBar::handle:vertical{background:#5f6f80;min-height:20px;border-radius:2px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0px;}"
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}").arg(textColor);
}

} // namespace AppStyle
