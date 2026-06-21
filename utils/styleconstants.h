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
        { QStringLiteral("warningYellow"), QStringLiteral("#ffcc00") },
        { QStringLiteral("softErrorRed"), QStringLiteral("#ff6b6b") },
        { QStringLiteral("softErrorText"), QStringLiteral("#ff7b7b") },
        { QStringLiteral("panelDark"), QStringLiteral("#25282f") },
        { QStringLiteral("htmlNegative"), QStringLiteral("#ff6666") },
        { QStringLiteral("htmlDim"), QStringLiteral("#888888") },
        { QStringLiteral("workflowBg"), QStringLiteral("#101824") },
        { QStringLiteral("workflowEdge"), QStringLiteral("#50667d") },
        { QStringLiteral("workflowEdgeHi"), QStringLiteral("#8fc7ff") },
        { QStringLiteral("workflowNodeBorder"), QStringLiteral("#6b7b8f") },
        { QStringLiteral("workflowTitleText"), QStringLiteral("#f1f6ff") },
        { QStringLiteral("workflowTypeText"), QStringLiteral("#a8bdd3") },
        { QStringLiteral("workflowSubtitleText"), QStringLiteral("#d8e1ea") },
        { QStringLiteral("syncStopRed"), QStringLiteral("#aa3333") },
        { QStringLiteral("chatUserBubbleBg"), QStringLiteral("#27425f") },
        { QStringLiteral("chatAssistantBubbleBg"), QStringLiteral("#1f2834") },
        { QStringLiteral("chatUserBubbleBorder"), QStringLiteral("#3f6b95") },
        { QStringLiteral("chatUserText"), QStringLiteral("#eaf4ff") },
        { QStringLiteral("chatActionText"), QStringLiteral("#b9c4d0") },
        { QStringLiteral("wfNodeHi"), QStringLiteral("#315f89") },
        { QStringLiteral("wfNodeKsampler"), QStringLiteral("#324b63") },
        { QStringLiteral("wfNodeText"), QStringLiteral("#3d4f3a") },
        { QStringLiteral("wfNodeLora"), QStringLiteral("#5a4634") },
        { QStringLiteral("wfNodeCkpt"), QStringLiteral("#4f3b5f") },
        { QStringLiteral("wfNodeDefault"), QStringLiteral("#253446") },
        { QStringLiteral("imgCompareA"), QStringLiteral("#7a4a2a") },
        { QStringLiteral("imgCompareB"), QStringLiteral("#2f6a4f") },
        { QStringLiteral("tagDiffCommon"), QStringLiteral("#314f7a") },
        { QStringLiteral("chartBar"), QStringLiteral("#23303f") },
        { QStringLiteral("chartAxis"), QStringLiteral("#aeb6bf") },
        { QStringLiteral("templateCardBg"), QStringLiteral("#1f2833") },
        { QStringLiteral("templateCardChild"), QStringLiteral("#202936") },
        { QStringLiteral("chatInnerBg"), QStringLiteral("#11161c") },
        { QStringLiteral("chatBtnBg"), QStringLiteral("#223041") },
        { QStringLiteral("chatBtnBorder"), QStringLiteral("#3a4b60") },
        { QStringLiteral("chatBtnHover"), QStringLiteral("#2d3f56") },
        { QStringLiteral("chatFooterBg"), QStringLiteral("#18212b") },
        { QStringLiteral("chatFooterBorder"), QStringLiteral("#344254") },
        { QStringLiteral("chatFooterHover"), QStringLiteral("#263447") },
        { QStringLiteral("chatFooterDisBg"), QStringLiteral("#171b20") },
        { QStringLiteral("chatFooterDisText"), QStringLiteral("#66707c") },
        { QStringLiteral("chatInnerBorder"), QStringLiteral("#2e3742") },
        { QStringLiteral("chatScrollHandle"), QStringLiteral("#5f6f80") },
        { QStringLiteral("buttonBorder"), QStringLiteral("#2a3f5a") },
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
        { QStringLiteral("warningYellow"), QStringLiteral("#b8860b") },
        { QStringLiteral("softErrorRed"), QStringLiteral("#d92d2d") },
        { QStringLiteral("softErrorText"), QStringLiteral("#d92d2d") },
        { QStringLiteral("panelDark"), QStringLiteral("#e6ebf1") },
        { QStringLiteral("htmlNegative"), QStringLiteral("#d92d2d") },
        { QStringLiteral("htmlDim"), QStringLiteral("#8a96a6") },
        { QStringLiteral("workflowBg"), QStringLiteral("#e9eef5") },
        { QStringLiteral("workflowEdge"), QStringLiteral("#9aabbf") },
        { QStringLiteral("workflowEdgeHi"), QStringLiteral("#2f80ed") },
        { QStringLiteral("workflowNodeBorder"), QStringLiteral("#b0bccb") },
        { QStringLiteral("workflowTitleText"), QStringLiteral("#18233a") },
        { QStringLiteral("workflowTypeText"), QStringLiteral("#51607a") },
        { QStringLiteral("workflowSubtitleText"), QStringLiteral("#2a3850") },
        { QStringLiteral("syncStopRed"), QStringLiteral("#c0392b") },
        { QStringLiteral("chatUserBubbleBg"), QStringLiteral("#d8e6fb") },
        { QStringLiteral("chatAssistantBubbleBg"), QStringLiteral("#ffffff") },
        { QStringLiteral("chatUserBubbleBorder"), QStringLiteral("#a9c7ef") },
        { QStringLiteral("chatUserText"), QStringLiteral("#14283f") },
        { QStringLiteral("chatActionText"), QStringLiteral("#51607a") },
        { QStringLiteral("wfNodeHi"), QStringLiteral("#cfe0f5") },
        { QStringLiteral("wfNodeKsampler"), QStringLiteral("#dde8f3") },
        { QStringLiteral("wfNodeText"), QStringLiteral("#e0ecdc") },
        { QStringLiteral("wfNodeLora"), QStringLiteral("#f0e6da") },
        { QStringLiteral("wfNodeCkpt"), QStringLiteral("#ebe0f0") },
        { QStringLiteral("wfNodeDefault"), QStringLiteral("#e6edf5") },
        { QStringLiteral("imgCompareA"), QStringLiteral("#caa37e") },
        { QStringLiteral("imgCompareB"), QStringLiteral("#8fcba8") },
        { QStringLiteral("tagDiffCommon"), QStringLiteral("#c9d9f0") },
        { QStringLiteral("chartBar"), QStringLiteral("#cdd9e8") },
        { QStringLiteral("chartAxis"), QStringLiteral("#5e6b7e") },
        { QStringLiteral("templateCardBg"), QStringLiteral("#ffffff") },
        { QStringLiteral("templateCardChild"), QStringLiteral("#f3f6fa") },
        { QStringLiteral("chatInnerBg"), QStringLiteral("#f3f6fa") },
        { QStringLiteral("chatBtnBg"), QStringLiteral("#e2e8f0") },
        { QStringLiteral("chatBtnBorder"), QStringLiteral("#c2cedd") },
        { QStringLiteral("chatBtnHover"), QStringLiteral("#d8e2f0") },
        { QStringLiteral("chatFooterBg"), QStringLiteral("#eef2f7") },
        { QStringLiteral("chatFooterBorder"), QStringLiteral("#c8d3e1") },
        { QStringLiteral("chatFooterHover"), QStringLiteral("#dbe5f1") },
        { QStringLiteral("chatFooterDisBg"), QStringLiteral("#f0f2f5") },
        { QStringLiteral("chatFooterDisText"), QStringLiteral("#9aa6b4") },
        { QStringLiteral("chatInnerBorder"), QStringLiteral("#c2cedd") },
        { QStringLiteral("chatScrollHandle"), QStringLiteral("#b8c4d4") },
        { QStringLiteral("buttonBorder"), QStringLiteral("#c2cedd") },
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
        { QStringLiteral("warningYellow"), QStringLiteral("#fbbf24") },
        { QStringLiteral("softErrorRed"), QStringLiteral("#f87171") },
        { QStringLiteral("softErrorText"), QStringLiteral("#fca5a5") },
        { QStringLiteral("panelDark"), QStringLiteral("#142338") },
        { QStringLiteral("htmlNegative"), QStringLiteral("#f87171") },
        { QStringLiteral("htmlDim"), QStringLiteral("#6f88a8") },
        { QStringLiteral("workflowBg"), QStringLiteral("#0a1422") },
        { QStringLiteral("workflowEdge"), QStringLiteral("#3a5a82") },
        { QStringLiteral("workflowEdgeHi"), QStringLiteral("#7dd3fc") },
        { QStringLiteral("workflowNodeBorder"), QStringLiteral("#3a5a82") },
        { QStringLiteral("workflowTitleText"), QStringLiteral("#eaf2ff") },
        { QStringLiteral("workflowTypeText"), QStringLiteral("#9fb3cf") },
        { QStringLiteral("workflowSubtitleText"), QStringLiteral("#c3d4e8") },
        { QStringLiteral("syncStopRed"), QStringLiteral("#b14a4a") },
        { QStringLiteral("chatUserBubbleBg"), QStringLiteral("#1c3553") },
        { QStringLiteral("chatAssistantBubbleBg"), QStringLiteral("#111f31") },
        { QStringLiteral("chatUserBubbleBorder"), QStringLiteral("#3a5a82") },
        { QStringLiteral("chatUserText"), QStringLiteral("#eaf4ff") },
        { QStringLiteral("chatActionText"), QStringLiteral("#9fb3cf") },
        { QStringLiteral("wfNodeHi"), QStringLiteral("#1d4b78") },
        { QStringLiteral("wfNodeKsampler"), QStringLiteral("#1e3a55") },
        { QStringLiteral("wfNodeText"), QStringLiteral("#26402a") },
        { QStringLiteral("wfNodeLora"), QStringLiteral("#4a3a28") },
        { QStringLiteral("wfNodeCkpt"), QStringLiteral("#3e2f4d") },
        { QStringLiteral("wfNodeDefault"), QStringLiteral("#152536") },
        { QStringLiteral("imgCompareA"), QStringLiteral("#7a4a2a") },
        { QStringLiteral("imgCompareB"), QStringLiteral("#2f6a4f") },
        { QStringLiteral("tagDiffCommon"), QStringLiteral("#243d5f") },
        { QStringLiteral("chartBar"), QStringLiteral("#18293c") },
        { QStringLiteral("chartAxis"), QStringLiteral("#9fb3cf") },
        { QStringLiteral("templateCardBg"), QStringLiteral("#111f31") },
        { QStringLiteral("templateCardChild"), QStringLiteral("#142338") },
        { QStringLiteral("chatInnerBg"), QStringLiteral("#0c1626") },
        { QStringLiteral("chatBtnBg"), QStringLiteral("#1a2c44") },
        { QStringLiteral("chatBtnBorder"), QStringLiteral("#2a466a") },
        { QStringLiteral("chatBtnHover"), QStringLiteral("#243d5f") },
        { QStringLiteral("chatFooterBg"), QStringLiteral("#0f1a2b") },
        { QStringLiteral("chatFooterBorder"), QStringLiteral("#26405f") },
        { QStringLiteral("chatFooterHover"), QStringLiteral("#1e3654") },
        { QStringLiteral("chatFooterDisBg"), QStringLiteral("#0c1626") },
        { QStringLiteral("chatFooterDisText"), QStringLiteral("#5a6b80") },
        { QStringLiteral("chatInnerBorder"), QStringLiteral("#1f3550") },
        { QStringLiteral("chatScrollHandle"), QStringLiteral("#3a5a82") },
        { QStringLiteral("buttonBorder"), QStringLiteral("#1e4069") },
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
        { QStringLiteral("warningYellow"), QStringLiteral("#ffff66") },
        { QStringLiteral("softErrorRed"), QStringLiteral("#ff6666") },
        { QStringLiteral("softErrorText"), QStringLiteral("#ff8888") },
        { QStringLiteral("panelDark"), QStringLiteral("#0a0a0a") },
        { QStringLiteral("htmlNegative"), QStringLiteral("#ff8888") },
        { QStringLiteral("htmlDim"), QStringLiteral("#aaaaaa") },
        { QStringLiteral("workflowBg"), QStringLiteral("#000000") },
        { QStringLiteral("workflowEdge"), QStringLiteral("#cccc66") },
        { QStringLiteral("workflowEdgeHi"), QStringLiteral("#ffff66") },
        { QStringLiteral("workflowNodeBorder"), QStringLiteral("#ffff66") },
        { QStringLiteral("workflowTitleText"), QStringLiteral("#ffffff") },
        { QStringLiteral("workflowTypeText"), QStringLiteral("#cccccc") },
        { QStringLiteral("workflowSubtitleText"), QStringLiteral("#ffffff") },
        { QStringLiteral("syncStopRed"), QStringLiteral("#ff6666") },
        { QStringLiteral("chatUserBubbleBg"), QStringLiteral("#1a1a00") },
        { QStringLiteral("chatAssistantBubbleBg"), QStringLiteral("#050505") },
        { QStringLiteral("chatUserBubbleBorder"), QStringLiteral("#ffff66") },
        { QStringLiteral("chatUserText"), QStringLiteral("#ffffff") },
        { QStringLiteral("chatActionText"), QStringLiteral("#cccccc") },
        { QStringLiteral("wfNodeHi"), QStringLiteral("#333300") },
        { QStringLiteral("wfNodeKsampler"), QStringLiteral("#1a1a00") },
        { QStringLiteral("wfNodeText"), QStringLiteral("#003300") },
        { QStringLiteral("wfNodeLora"), QStringLiteral("#332200") },
        { QStringLiteral("wfNodeCkpt"), QStringLiteral("#2a0033") },
        { QStringLiteral("wfNodeDefault"), QStringLiteral("#0a0a0a") },
        { QStringLiteral("imgCompareA"), QStringLiteral("#aa6622") },
        { QStringLiteral("imgCompareB"), QStringLiteral("#22aa66") },
        { QStringLiteral("tagDiffCommon"), QStringLiteral("#333300") },
        { QStringLiteral("chartBar"), QStringLiteral("#1a1a1a") },
        { QStringLiteral("chartAxis"), QStringLiteral("#cccccc") },
        { QStringLiteral("templateCardBg"), QStringLiteral("#050505") },
        { QStringLiteral("templateCardChild"), QStringLiteral("#0a0a0a") },
        { QStringLiteral("chatInnerBg"), QStringLiteral("#000000") },
        { QStringLiteral("chatBtnBg"), QStringLiteral("#1a1a1a") },
        { QStringLiteral("chatBtnBorder"), QStringLiteral("#ffff66") },
        { QStringLiteral("chatBtnHover"), QStringLiteral("#333300") },
        { QStringLiteral("chatFooterBg"), QStringLiteral("#000000") },
        { QStringLiteral("chatFooterBorder"), QStringLiteral("#ffffff") },
        { QStringLiteral("chatFooterHover"), QStringLiteral("#333300") },
        { QStringLiteral("chatFooterDisBg"), QStringLiteral("#0a0a0a") },
        { QStringLiteral("chatFooterDisText"), QStringLiteral("#888888") },
        { QStringLiteral("chatInnerBorder"), QStringLiteral("#ffff66") },
        { QStringLiteral("chatScrollHandle"), QStringLiteral("#888844") },
        { QStringLiteral("buttonBorder"), QStringLiteral("#ffff66") },
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
