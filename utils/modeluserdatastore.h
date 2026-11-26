#ifndef MODELUSERDATASTORE_H
#define MODELUSERDATASTORE_H

#include "core/modeltypes.h"

#include <QColor>
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

namespace ModelUserDataStore {

QStringList normalizeDistinctTrimmedList(const QStringList &items);
QStringList normalizeUserTagsText(const QString &text);
QString formatRating(double rating, const QString &unratedLabel);
QString formatUserNoteTooltip(const ModelUserNote &note,
                              const QString &baseTooltip,
                              const QString &unratedLabel);

QMap<QString, QStringList> loadCollections(const QString &filePath);
bool saveCollections(const QString &filePath, const QMap<QString, QStringList> &collections);

QHash<QString, QColor> loadHighlightColors(const QString &filePath);
bool saveHighlightColors(const QString &filePath, const QHash<QString, QColor> &colors);

QHash<QString, ModelUserNote> loadUserNotes(const QString &filePath);
bool saveUserNotes(const QString &filePath, const QHash<QString, ModelUserNote> &notes);

QHash<QString, QJsonObject> loadSyncFailures(const QString &filePath);
bool saveSyncFailures(const QString &filePath, const QHash<QString, QJsonObject> &failures);

} // namespace ModelUserDataStore

#endif // MODELUSERDATASTORE_H
