#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Replace the hardcoded palette region in styleconstants.h with declarations.

The 4 palette bodies + paletteForTheme + active-pointer helpers move to
styleconstants.cpp. str()/color()/applyTokens()/loadQss()/named accessors stay
inline (they only need the declarations).
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "utils" / "styleconstants.h"

START = "// === 颜色调色板（单一事实源）==="
END = "// 取 token 的字符串值 / QColor：先查活动调色板，缺省回退 Steam Dark。"

NEW_BLOCK = """// === 颜色调色板（单一事实源，预设存于 :/themes/*.json）===
// QSS 用 @token / @rgba(token, A) 占位，加载时由 applyTokens 替换为活动调色板的值。
// 调色板的构造 / 注册 / 读盘实现见 styleconstants.cpp（避免 QJson 进入大量 TU）。

// 主题描述（供设置页下拉等使用）。
struct ThemeInfo {
    QString id;
    QString displayName;
    bool builtin = false;
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
// 注册表是否含该主题 id（含内置与用户主题，不含 custom_qss）。
bool themeExists(const QString &themeId);
// 用户主题目录（= 程序目录/config/themes，便携，与 settings.json 同级）。
QString userThemesDir();
// 写出用户主题到 config/themes/<id>.json（只存与默认不同的 token）。
bool saveUserTheme(const QString &id, const QString &displayName,
                   const QHash<QString, QString> &tokens, QString *errorOut = nullptr);
// 删除用户主题文件（内置主题不可删，返回 false）。
bool deleteUserTheme(const QString &id);
// 重新扫描用户主题目录，刷新注册表中的用户项。
void reloadUserThemes();

"""

text = HEADER.read_text(encoding="utf-8")

# add QList include for QList<ThemeInfo>
if "#include <QList>" not in text:
    text = text.replace("#include <QHash>\n", "#include <QHash>\n#include <QList>\n", 1)

s = text.index(START)
e = text.index(END)
assert s < e, "anchors out of order"
new_text = text[:s] + NEW_BLOCK + text[e:]

HEADER.write_text(new_text, encoding="utf-8")
print(f"header: {len(text.splitlines())} -> {len(new_text.splitlines())} lines")
print("replaced palette region with declarations; added <QList> include")
