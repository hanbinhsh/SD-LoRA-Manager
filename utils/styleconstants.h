#pragma once

#include <QColor>
#include <QFile>
#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QString>

namespace AppStyle {

// === 颜色调色板（单一事实源，预设存于 :/themes/*.json）===
// QSS 用 @token / @rgba(token, A) 占位，加载时由 applyTokens 替换为活动调色板的值。
// 调色板的构造 / 注册 / 读盘实现见 styleconstants.cpp（避免 QJson 进入大量 TU）。

// 主题描述（供设置页下拉等使用）。
struct ThemeInfo {
    QString id;
    QString displayName;
    bool builtin = false;
    QString scheme = QStringLiteral("dark"); // 窗口基调："light" 或 "dark"（决定 colorScheme）
};

// 默认(Steam Dark)全量调色板，作为缺省回退基底。
const QHash<QString, QString> &defaultPalette();
// 指定主题的全量调色板（未知 id / custom_qss 回退 Steam Dark）。
const QHash<QString, QString> &paletteForTheme(const QString &themeId);
// 当前活动调色板（未设置时为 Steam Dark）。
const QHash<QString, QString> &activePalette();

// 按 id 切换活动主题（指向注册表中的调色板）。
void setActiveTheme(const QString &themeId);
// 直接指定一份调色板为活动（编辑器实时预览用，内部拷贝持有）。
void setActivePalette(const QHash<QString, QString> &palette);

// 已注册主题（内置 + 用户）有序列表，供设置页下拉。
QList<ThemeInfo> availableThemes();
// 主题 id -> 显示名（未知返回原 id）。
QString themeDisplayName(const QString &themeId);
// 主题 id -> 窗口基调 "light"/"dark"（未知/custom_qss 返回 "dark"）。
QString themeColorScheme(const QString &themeId);
// 注册表是否含该主题 id（含内置与用户主题，不含 custom_qss）。
bool themeExists(const QString &themeId);
// 用户主题目录（= 程序目录/config/themes，便携，与 settings.json 同级）。
QString userThemesDir();
// 写出用户主题到 config/themes/<id>.json（含 _name/_scheme，只存与默认不同的 token）。
bool saveUserTheme(const QString &id, const QString &displayName, const QString &scheme,
                   const QHash<QString, QString> &tokens, QString *errorOut = nullptr);
// 删除用户主题文件（内置主题不可删，返回 false）。
bool deleteUserTheme(const QString &id);
// 重新扫描用户主题目录，刷新注册表中的用户项。
void reloadUserThemes();

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

// === 命名颜色访问器（原 constexpr 常量，现读运行时调色板，随主题变化）===
inline QString AccentBlue()                { return str("accentBlue"); }
inline QString CustomTriggerGreen()        { return str("successGreen"); }
inline QString MutedText()                 { return str("mutedText"); }
inline QString BodyText()                  { return str("bodyText"); }
inline QString WhiteText()                 { return str("whiteText"); }
inline QString WarningYellow()             { return str("warningYellow"); }
inline QString ErrorRed()                  { return str("errorRed"); }
inline QString SoftErrorRed()              { return str("softErrorRed"); }
inline QString SoftErrorText()             { return str("softErrorText"); }
inline QString PanelDark()                 { return str("panelDark"); }
inline QString PanelBorder()               { return str("hoverBg"); }
inline QString PanelBorderSoft()           { return str("scrollHandle"); }
inline QString PlaceholderText()           { return str("placeholderText"); }
inline QString HeaderBackground()          { return str("headerBg"); }
inline QString HtmlNegative()              { return str("htmlNegative"); }
inline QString HtmlSubtle()                { return str("htmlSubtle"); }
inline QString HtmlDim()                   { return str("htmlDim"); }
inline QString MainBackground()            { return str("windowBg"); }
inline QString SidebarDark()               { return str("sidebarBg"); }
inline QString DownloadCardBackground()    { return str("downloadCardBg"); }
inline QString TableSelected()             { return str("selectionBg"); }
inline QString WorkflowBackground()        { return str("workflowBg"); }
inline QString WorkflowEdge()              { return str("workflowEdge"); }
inline QString WorkflowEdgeHighlight()     { return str("workflowEdgeHi"); }
inline QString WorkflowNodeBorder()        { return str("workflowNodeBorder"); }
inline QString WorkflowTitleText()         { return str("workflowTitleText"); }
inline QString WorkflowTypeText()          { return str("workflowTypeText"); }
inline QString WorkflowSubtitleText()      { return str("workflowSubtitleText"); }
inline QString SyncStopRed()               { return str("syncStopRed"); }
inline QString ChatUserBubbleBg()          { return str("chatUserBubbleBg"); }
inline QString ChatAssistantBubbleBg()     { return str("chatAssistantBubbleBg"); }
inline QString ChatUserBubbleBorder()      { return str("chatUserBubbleBorder"); }
inline QString ChatAssistantBubbleBorder() { return str("scrollHandle"); }
inline QString ChatUserText()              { return str("chatUserText"); }
inline QString ChatActionText()            { return str("chatActionText"); }

inline QString MutedLabelStyle()
{ return QStringLiteral("color:%1;background:transparent;").arg(str("mutedText")); }

inline QString MutedBoldLabelStyle()
{ return QStringLiteral("color:%1;background:transparent;font-weight:bold;").arg(str("mutedText")); }

inline QString UserTagChipInlineStyle()
{
    const QColor g = color("successGreen");
    return QStringLiteral("padding: 2px 8px; border-radius: 8px; background: rgba(%1,%2,%3,48); color: %4;")
        .arg(g.red()).arg(g.green()).arg(g.blue()).arg(str("userTagChipText"));
}

inline constexpr const char *FilterTagSourceModel = "model";
inline constexpr const char *FilterTagSourceUser = "user";
inline constexpr const char *FilterTagSourceMixed = "mixed";

inline QColor translucentWhite(int alpha)
{
    QColor c = color("overlayFg"); // 浅色主题翻转为深色叠加
    c.setAlpha(alpha);
    return c;
}

inline QColor imageCompareOnlyA()
{
    QColor c = color("imgCompareA");
    c.setAlpha(120);
    return c;
}

inline QColor imageCompareOnlyB()
{
    QColor c = color("imgCompareB");
    c.setAlpha(120);
    return c;
}

inline QColor workflowNodeColor(const QString &type, bool highlighted)
{
    const QString lower = type.toLower();
    if (highlighted) return color("wfNodeHi");
    if (lower.contains("ksampler")) return color("wfNodeKsampler");
    if (lower.contains("cliptextencode") || lower.contains("text")) return color("wfNodeText");
    if (lower.contains("loraloader")) return color("wfNodeLora");
    if (lower.contains("checkpointloader")) return color("wfNodeCkpt");
    return color("wfNodeDefault");
}

inline QString modelTitleNormalStyle()
{
    return QStringLiteral(
        "color: #fff;"
        "background-color: rgba(0,0,0,120);"
        "padding: 15px;"
        "border-left: 5px solid %1;"
        "font-size: 24px;"
        "font-weight: bold;").arg(AccentBlue());
}

inline QString modelTitleErrorStyle()
{
    return QStringLiteral(
        "color: %1;"
        "background-color: rgba(45, 20, 20, 0.8);"
        "border-left: 5px solid #ff0000;"
        "padding: 15px;"
        "font-size: 15px;").arg(ErrorRed());
}

inline QString globalGalleryTitleStyle()
{
    return QStringLiteral(
        "color: #fff;"
        "background-color: rgba(0,0,0,120);"
        "padding: 15px;"
        "border-left: 5px solid %1;"
        "font-size: 24px;"
        "font-weight: bold;").arg(WarningYellow());
}

inline QColor steamBackground(int alpha = 255)
{
    QColor c = color("windowBg"); // 随主题变化（Hero 渐变叠加层）
    c.setAlpha(alpha);
    return c;
}

inline QString chatStatusStyle(bool isError)
{
    return QStringLiteral("color:%1;").arg(isError ? SoftErrorText() : MutedText());
}

inline QString dangerButtonStyle()
{
    return QStringLiteral(
        "QPushButton { background-color: %1; color: %2; border: none; padding: 4px 8px; border-radius: 2px; }"
        "QPushButton:hover { background-color: %2; color: %3; }")
        .arg(str("dangerBg"), ErrorRed(), str("whiteText"));
}

inline QString allowButtonStyle()
{
    return QStringLiteral(
        "QPushButton { background-color: #1b4b2a; color: #8fbc8f; border: none; padding: 4px 8px; border-radius: 2px; }"
        "QPushButton:hover { background-color: #2e8b57; color: #fff; }");
}

inline QString chatThumbStyle()
{
    return QStringLiteral("QPushButton{background:%1;border:1px solid %2;border-radius:6px;color:%3;padding:2px;}")
        .arg(str("chatInnerBg"), PanelBorderSoft(), MutedText());
}

inline QString chatActionButtonStyle(const QString &textColor)
{
    return QStringLiteral(
        "QPushButton{background:%2;border:1px solid %3;border-radius:4px;padding:2px 8px;color:%1;}"
        "QPushButton:hover{background:%4;color:%5;}")
        .arg(textColor, str("chatBtnBg"), str("chatBtnBorder"), str("chatBtnHover"), str("primaryText"));
}

inline QString chatFooterButtonStyle()
{
    return QStringLiteral(
        "QPushButton{background:%1;border:1px solid %2;border-radius:4px;color:%3;padding:2px 7px;font-size:11px;}"
        "QPushButton:hover{background:%4;color:%5;}"
        "QPushButton:disabled{background:%6;border-color:%7;color:%8;}")
        .arg(str("chatFooterBg")).arg(str("chatFooterBorder")).arg(ChatActionText())
        .arg(str("chatFooterHover")).arg(str("primaryText"))
        .arg(str("chatFooterDisBg")).arg(str("disabledBg")).arg(str("chatFooterDisText"));
}

inline QString chatThinkingBrowserStyle()
{
    return QStringLiteral(
        "QTextBrowser{border:1px solid %1;border-radius:6px;background:%2;color:%3;padding:6px;}"
        "QScrollBar:vertical{width:4px;background:transparent;}"
        "QScrollBar::handle:vertical{background:%4;min-height:20px;border-radius:2px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0px;}")
        .arg(str("chatInnerBorder"), str("chatInnerBg"), BodyText(), str("chatScrollHandle"));
}

inline QString chatBodyBrowserStyle(const QString &textColor)
{
    return QStringLiteral(
        "QTextBrowser{border:none;background:transparent;color:%1;font-size:13px;padding:0px;}"
        "QScrollBar:vertical{background:transparent;width:4px;margin:0px;}"
        "QScrollBar::handle:vertical{background:%2;min-height:20px;border-radius:2px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0px;}"
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}")
        .arg(textColor, str("chatScrollHandle"));
}

} // namespace AppStyle
