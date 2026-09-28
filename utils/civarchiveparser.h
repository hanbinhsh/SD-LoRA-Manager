#pragma once
#include "core/modeltypes.h"
#include <QByteArray>
#include <QJsonObject>
#include <QUrl>

namespace CivArchiveParser {
bool parseCivArchivePayload(const QByteArray &data, const QString &sourceUrl,
                           QJsonObject &modelRoot, QJsonObject &versionHint);
QUrl civArchiveLookupUrl(const MetadataSyncJob &job);
}
