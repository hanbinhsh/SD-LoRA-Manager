#include "downloadcache.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

namespace DownloadCache {

ModelUpdateInfo infoFromJson(const QJsonObject &obj)
{
    ModelUpdateInfo info;
    info.filePath = obj.value("filePath").toString();
    info.modelDir = obj.value("modelDir").toString(QFileInfo(info.filePath).absolutePath());
    info.baseName = obj.value("baseName").toString(QFileInfo(info.filePath).completeBaseName());
    info.displayName = obj.value("displayName").toString(info.baseName);
    info.currentVersion = obj.value("currentVersion").toString();
    info.latestVersion = obj.value("latestVersion").toString();
    info.downloadUrl = obj.value("downloadUrl").toString();
    info.downloadFileName = obj.value("downloadFileName").toString();
    info.sha256 = obj.value("sha256").toString();
    info.metadataSource = obj.value("metadataSource").toString();
    info.sourceUrl = obj.value("sourceUrl").toString();
    info.latestVersionJson = obj.value("latestVersionJson").toObject();
    info.modelId = obj.value("modelId").toInt();
    info.currentVersionId = obj.value("currentVersionId").toInt();
    info.latestVersionId = obj.value("latestVersionId").toInt();
    info.sizeMB = obj.value("sizeMB").toDouble();
    info.hasUpdate = obj.value("hasUpdate").toBool(false);
    info.latestFileExistsLocally = obj.value("latestFileExistsLocally").toBool(false);
    info.previewState = static_cast<ModelPreviewState>(
        obj.value("previewState").toInt(static_cast<int>(ModelPreviewState::MissingOrUnknown)));

    return info;
}

QJsonObject entryToJson(const ModelUpdateInfo &info, const QString &status)
{
    QJsonObject obj;
    obj["filePath"] = info.filePath;
    obj["modelDir"] = info.modelDir;
    obj["baseName"] = info.baseName;
    obj["displayName"] = info.displayName;
    obj["currentVersion"] = info.currentVersion;
    obj["latestVersion"] = info.latestVersion;
    obj["downloadUrl"] = info.downloadUrl;
    obj["downloadFileName"] = info.downloadFileName;
    obj["sha256"] = info.sha256;
    obj["metadataSource"] = info.metadataSource;
    obj["sourceUrl"] = info.sourceUrl;
    obj["latestVersionJson"] = info.latestVersionJson;
    obj["modelId"] = info.modelId;
    obj["currentVersionId"] = info.currentVersionId;
    obj["latestVersionId"] = info.latestVersionId;
    obj["sizeMB"] = info.sizeMB;
    obj["hasUpdate"] = info.hasUpdate;
    obj["latestFileExistsLocally"] = info.latestFileExistsLocally;
    obj["previewState"] = static_cast<int>(info.previewState);
    obj["status"] = status;
    return obj;
}

LoadResult load(const QString &path)
{
    LoadResult result;
    QFile file(path);
    if (!file.exists()) return result;
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = file.errorString();
        return result;
    }
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()
        || !doc.object().value("items").isArray()) {
        result.error = error.error != QJsonParseError::NoError
            ? error.errorString() : QStringLiteral("Invalid downloads.json: expected an object with an items array");
        return result;
    }
    QHash<QString, int> indices;
    const QJsonArray array = doc.object().value("items").toArray();
    result.entries.reserve(array.size());
    for (const QJsonValue &value : array) {
        const QJsonObject obj = value.toObject();
        Entry entry;
        entry.info = infoFromJson(obj);
        if (entry.info.filePath.isEmpty()) continue;
        entry.status = obj.value("status").toString(entry.info.hasUpdate ? "发现新版本" : "已是最新");
        entry.sourceAvailable = QFileInfo::exists(entry.info.filePath);
        const auto it = indices.constFind(entry.info.filePath);
        if (it == indices.cend()) {
            indices.insert(entry.info.filePath, result.entries.size());
            result.entries.append(entry);
        } else {
            result.entries[it.value()] = entry;
        }
    }
    return result;
}

bool save(const QString &path, const QVector<Entry> &entries, QString *error)
{
    QJsonArray items;
    for (const Entry &entry : entries) items.append(entryToJson(entry.info, entry.status));
    const QJsonObject root{{"version", 1},
                           {"savedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                           {"items", items}};
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    const QByteArray payload = QJsonDocument(root).toJson();
    const bool ok = file.open(QIODevice::WriteOnly)
                    && file.write(payload) == payload.size() && file.commit();
    if (!ok && error) *error = file.errorString();
    return ok;
}

}
