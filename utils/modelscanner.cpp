#include "modelscanner.h"
#include "modelmetadatacodec.h"
#include "modellistcache.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QDebug>
#include <QSet>

namespace ModelScanner {
namespace {
QJsonObject modelListSummary(const ModelListMetadata &m)
{
    return {{"sortDate", QString::number(m.sortDate)}, {"sortAdded", QString::number(m.sortAdded)},
            {"downloads", m.downloads}, {"likes", m.likes}, {"base", m.filterBase},
            {"nsfw", m.nsfwLevel}, {"edited", m.localEdited}, {"modelId", m.modelId},
            {"versionId", m.versionId}, {"sha256", m.civitaiSha256}, {"creator", m.creator},
            {"tags", QJsonArray::fromStringList(m.modelTags)}, {"type", m.modelType},
            {"triggers", QJsonArray::fromStringList(m.trainedWords)}, {"name", m.civitaiName},
            {"previewState", static_cast<int>(m.previewState)}};
}

bool restoreModelListSummary(const QJsonObject &s, ModelListMetadata *m)
{
    // Reject truncated/older summaries rather than silently losing roles.
    static const QJsonObject schema = modelListSummary(ModelListMetadata{});
    for (auto it = schema.begin(); it != schema.end(); ++it) {
        if (s.value(it.key()).type() != it.value().type()) return false;
    }
    m->sortDate = s.value("sortDate").toString().toLongLong();
    m->sortAdded = s.value("sortAdded").toString().toLongLong();
    m->downloads = s.value("downloads").toInt();
    m->likes = s.value("likes").toInt();
    m->filterBase = s.value("base").toString();
    m->nsfwLevel = s.value("nsfw").toInt();
    m->localEdited = s.value("edited").toBool();
    m->modelId = s.value("modelId").toInt();
    m->versionId = s.value("versionId").toInt();
    m->civitaiSha256 = s.value("sha256").toString();
    m->creator = s.value("creator").toString();
    for (const QJsonValue &v : s.value("tags").toArray()) m->modelTags.append(v.toString());
    m->modelType = s.value("type").toString();
    for (const QJsonValue &v : s.value("triggers").toArray()) m->trainedWords.append(v.toString());
    m->civitaiName = s.value("name").toString();
    m->previewState = static_cast<ModelPreviewState>(s.value("previewState").toInt());
    return true;
}

}

ModelListMetadata parseModelListMetadata(const QString &filePath, const QString &jsonPath,
                                         bool *cacheable)
{
    if (cacheable) *cacheable = false;
    ModelListMetadata m;

    QFileInfo fi(filePath);
    QDateTime birthTime = fi.birthTime();
    if (!birthTime.isValid()) birthTime = fi.lastModified();
    m.sortAdded = birthTime.toMSecsSinceEpoch();

    QFile file(jsonPath);
    if (!file.exists()) {
        if (cacheable) *cacheable = true;
        m.sortDate = fi.lastModified().toMSecsSinceEpoch();
        return m;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        m.sortDate = fi.lastModified().toMSecsSinceEpoch();
        return m;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        m.sortDate = fi.lastModified().toMSecsSinceEpoch();
        return m;
    }
    const QJsonObject root = document.object();
    if (cacheable) *cacheable = true;
    m.creator = ModelMetadata::jsonModelCreator(root);
    m.modelTags = ModelMetadata::jsonModelTags(root);
    m.modelType = root["model"].toObject()["type"].toString();

    m.trainedWords = ModelMetadata::trainedWords(root);

    const QString modelName = root["model"].toObject()["name"].toString();
    const QString versionName = root["name"].toString();
    if (!modelName.isEmpty()) {
        QString fullName = modelName;
        if (!versionName.isEmpty()) fullName += " [" + versionName + "]";
        m.civitaiName = fullName;
    }
    m.localEdited = root["localEdited"].toBool(false) || root["localOnly"].toBool(false);
    m.modelId = root["modelId"].toInt(root.value("model").toObject().value("id").toInt());
    m.versionId = root["id"].toInt();

    bool hasUsableImage = false;
    for (const QJsonValue &value : root.value("images").toArray()) {
        const QJsonObject image = value.toObject();
        const QString type = image.value("type").toString();
        const QString url = image.value("url").toString();
        if (type.compare("video", Qt::CaseInsensitive) == 0
            || url.endsWith(".mp4", Qt::CaseInsensitive)
            || url.endsWith(".webm", Qt::CaseInsensitive)) {
            continue;
        }
        if (!url.trimmed().isEmpty()) {
            hasUsableImage = true;
            break;
        }
    }
    const bool localOnly = root.value("localOnly").toBool(false);
    const bool knownSynced = localOnly
                             || m.modelId > 0
                             || m.versionId > 0
                             || !root.value("metadataSource").toString().trimmed().isEmpty()
                             || !root.value("syncedAt").toString().trimmed().isEmpty();
    if (knownSynced && !hasUsableImage) m.previewState = ModelPreviewState::KnownNoPreview;

    int coverLevel = 1; // 默认 Safe
    const QJsonArray images = root["images"].toArray();
    if (!images.isEmpty()) {
        const QJsonObject coverObj = images[0].toObject();
        if (coverObj.contains("nsfwLevel")) {
            coverLevel = coverObj["nsfwLevel"].toInt();
        } else if (coverObj.contains("nsfw")) {
            const QString val = coverObj["nsfw"].toString().toLower();
            if (val == "x" || val == "mature") coverLevel = 16;
            else if (val == "soft") coverLevel = 2;
            else coverLevel = 1;
        }
    } else {
        if (root.contains("nsfwLevel")) coverLevel = root["nsfwLevel"].toInt();
        else if (root["nsfw"].toBool()) coverLevel = 16;
    }
    m.nsfwLevel = coverLevel;

    const QString baseModel = root["baseModel"].toString();
    if (!baseModel.isEmpty()) m.filterBase = baseModel;

    const QString dateStr = root["createdAt"].toString();
    if (!dateStr.isEmpty()) {
        const QDateTime dt = QDateTime::fromString(dateStr, Qt::ISODate);
        if (dt.isValid()) m.sortDate = dt.toMSecsSinceEpoch();
        // dateStr 非空但无法解析时，保持 0（与旧逻辑一致）
    } else {
        m.sortDate = fi.lastModified().toMSecsSinceEpoch();
    }

    const QJsonObject stats = root["stats"].toObject();
    m.downloads = stats["downloadCount"].toInt();
    m.likes = stats["thumbsUpCount"].toInt();
    const QJsonArray files = root["files"].toArray();
    if (!files.isEmpty()) {
        const QJsonObject selectedFile = ModelMetadata::selectVersionFileForLocalModel(files, filePath);
        m.civitaiSha256 = selectedFile["hashes"].toObject()["SHA256"].toString();
    }

    return m;
}

QList<ScannedModelEntry> scanModelsWorker(const QStringList &paths, bool recursive,
                                         const QString &cachePath)
{
    QList<ScannedModelEntry> entries;
    QElapsedTimer timer;
    timer.start();
    ModelListCache cache(cachePath);
    QSet<QString> visited;
    int cacheHits = 0;
    static const QStringList nameFilters = {"*.safetensors", "*.ckpt", "*.pt"};
    static const QStringList imgExts = {".preview.png", ".png", ".jpg", ".jpeg"};
    const QDir::Filters dirFilters = QDir::Files | QDir::NoDotAndDotDot;
    const QDirIterator::IteratorFlags iterFlags =
        recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;

    for (const QString &path : paths) {
        if (path.isEmpty() || !QDir(path).exists()) continue;
        const QString rootPath = QFileInfo(path).absoluteFilePath();
        QString rootName = QFileInfo(rootPath).fileName();
        if (rootName.isEmpty()) rootName = rootPath;

        QDirIterator it(path, nameFilters, dirFilters, iterFlags);
        while (it.hasNext()) {
            it.next();
            const QFileInfo fileInfo = it.fileInfo();
            QString pathKey = fileInfo.absoluteFilePath();
#ifdef Q_OS_WIN
            pathKey = pathKey.toCaseFolded();
#endif
            if (visited.contains(pathKey)) continue;
            visited.insert(pathKey);

            ScannedModelEntry e;
            e.baseName = fileInfo.completeBaseName();
            e.fullPath = fileInfo.absoluteFilePath();
            e.rootPath = rootPath;
            e.rootName = rootName;

            const QDir currentFileDir = fileInfo.dir();
            for (const QString &ext : imgExts) {
                const QString tryPath = currentFileDir.absoluteFilePath(e.baseName + ext);
                if (QFile::exists(tryPath)) { e.previewPath = tryPath; break; }
            }

            const QString jsonPath = currentFileDir.filePath(e.baseName + ".json");
            const QJsonObject stamp = ModelListCache::fingerprint(fileInfo, QFileInfo(jsonPath));
            QJsonObject summary;
            if (cache.lookup(e.fullPath, stamp, &summary) && restoreModelListSummary(summary, &e.meta)) {
                ++cacheHits;
            } else {
                bool cacheable = false;
                e.meta = parseModelListMetadata(e.fullPath, jsonPath, &cacheable);
                // A sync/edit may replace the JSON while this scan is reading it.
                if (cacheable && stamp == ModelListCache::fingerprint(QFileInfo(e.fullPath), QFileInfo(jsonPath)))
                    cache.insert(e.fullPath, stamp, modelListSummary(e.meta));
            }
            // Decode once in the thumbnail worker, not while discovering models.
            // Until it succeeds, an existing but possibly corrupt cover stays an X.
            if (!e.previewPath.isEmpty()) e.meta.previewState = ModelPreviewState::MissingOrUnknown;
            entries.append(e);
        }
    }
    if (!cache.save()) qWarning() << "Unable to save model list cache:" << cachePath;
    qDebug() << "Model scan:" << entries.size() << "models," << cacheHits
             << "cached summaries," << timer.elapsed() << "ms";
    return entries;
}
}
