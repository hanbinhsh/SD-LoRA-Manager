#pragma once

#include <QColor>
#include <QFile>
#include <QString>

namespace AppStyle {

// 读取一个 QSS 资源文件内容（失败时返回空串）。统一各工具页加载 toolpage.qss 的逻辑。
inline QString loadQss(const QString &resourcePath)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(file.readAll());
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
    return QColor(27, 40, 56, alpha);
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
