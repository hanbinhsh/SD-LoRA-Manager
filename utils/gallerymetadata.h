#pragma once

#include "core/modeltypes.h"

namespace GalleryMetadata {

inline constexpr int ParserVersion = 10;
void parseImage(const QString &path, UserImageInfo &info,
                bool splitOnNewline, const QStringList &filterTags);

}
