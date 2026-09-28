#pragma once

#include "core/modeltypes.h"

namespace GalleryMetadata {

inline constexpr int ParserVersion = 7;
void parseImage(const QString &path, UserImageInfo &info,
                bool splitOnNewline, const QStringList &filterTags);

}
