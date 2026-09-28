#pragma once
#include "core/modeltypes.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QVector>

namespace ModelMetadata {
QStringList jsonModelTags(const QJsonObject &root);
QString jsonModelCreator(const QJsonObject &root);
QString readModelCreatorAvatarFromJson(const QJsonObject &root);
QStringList trainedWords(const QJsonObject &root);
QString metadataShaFromRoot(const QJsonObject &root);
QString metadataBrowserUrlFromRoot(const QJsonObject &root);
QVector<ImageInfo> imageInfosFromVersionJson(const QJsonObject &root);
QJsonObject selectVersionFileForLocalModel(const QJsonArray &files, const QString &localFilePath,
                                         const QString &preferredSha256 = {});
QJsonObject mergeCivitaiModelIntoVersion(const QJsonObject &versionRoot, const QJsonObject &modelRoot);
// Display data only. The original JSON is never rewritten through ModelMeta.
ModelMeta parseVersion(const QJsonObject &root, const QString &filePath, const QString &baseName,
                       const QString &preferredSha256 = {});
bool readLocalJson(const QString &dirPath, const QString &baseName, ModelMeta &meta);
QString previewPath(const QString &dirPath, const QString &baseName, int index);
}
