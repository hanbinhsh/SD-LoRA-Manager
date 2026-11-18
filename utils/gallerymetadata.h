#pragma once

#include "core/modeltypes.h"

namespace GalleryMetadata {

inline constexpr int ParserVersion = 10;
void parseImage(const QString &path, UserImageInfo &info,
                bool splitOnNewline, const QStringList &filterTags);
void populateParsedModelFields(UserImageInfo &info);
QString cleanModelOrLoraBaseName(QString raw);
QStringList parseNsfwRules(const QString &rawRules);
bool matchesPositivePromptNsfwRules(const QString &rawPrompt,
                                    const QStringList &cleanTags,
                                    const QStringList &rules);

}
