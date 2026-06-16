#ifndef TAGUTILS_H
#define TAGUTILS_H

#include <QString>
#include <QStringList>

namespace TagUtils {

QString cleanPromptTag(QString text, bool preserveEmoticons = true);

// 把一段 prompt 文本切分成原始 tag 片段（未做 cleanPromptTag 清洗）。
// 处理逻辑：trim -> 疑似 JSON（ComfyUI 工作流等）直接返回空 -> 可选换行转逗号 -> 逗号切分。
// 调用方再自行决定 cleanPromptTag 的参数、过滤与去重。
QStringList splitPromptParts(const QString &rawPrompt, bool splitOnNewline);

}

#endif // TAGUTILS_H
