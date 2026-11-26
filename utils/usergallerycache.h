#ifndef USERGALLERYCACHE_H
#define USERGALLERYCACHE_H

#include "core/modeltypes.h"

#include <QMap>
#include <QString>
#include <QStringList>

namespace UserGalleryCache {

QMap<QString, UserImageInfo> load(const QString &cachePath,
                                  bool splitOnNewline,
                                  const QStringList &filterTags);

bool save(const QString &cachePath, const QMap<QString, UserImageInfo> &imageCache);

} // namespace UserGalleryCache

#endif // USERGALLERYCACHE_H
