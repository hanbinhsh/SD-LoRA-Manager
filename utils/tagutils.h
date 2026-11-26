#ifndef TAGUTILS_H
#define TAGUTILS_H

#include <QMap>
#include <QString>
#include <QStringList>

namespace TagUtils {

// Gallery keys preserve hyphens and already-cleaned prompt syntax.
QString normalizedGalleryTagKey(QString text);
QStringList normalizedGalleryTagKeys(const QStringList &tags);
QStringList parsePromptTags(const QString &rawPrompt, bool splitOnNewline,
                            const QStringList &filterTags);

QString cleanPromptTag(QString text, bool preserveEmoticons = true);

// Stable key used for counting, deduplication and matching across tool pages.
// Whitespace, underscores and hyphens are equivalent and matching is case-insensitive.
QString normalizedPromptTagKey(QString text);

// 把一段 prompt 文本切分成原始 tag 片段（未做 cleanPromptTag 清洗）。
// 处理逻辑：trim -> 疑似 JSON（ComfyUI 工作流等）直接返回空 -> 可选换行转逗号 -> 逗号切分。
// 调用方再自行决定 cleanPromptTag 的参数、过滤与去重。
QStringList splitPromptParts(const QString &rawPrompt, bool splitOnNewline);

// 统计清洗后 Tag 的出现次数，并可选按首次出现顺序去重输出。
void collectPromptTagStats(const QString &rawPrompt,
                           bool splitOnNewline,
                           QMap<QString, int> &countsOut,
                           QStringList *uniqueOrderOut = nullptr);

}

#endif // TAGUTILS_H
