#include "modelmetadatacodec.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QDebug>
#include <utility>

namespace ModelMetadata {
QStringList jsonModelTags(const QJsonObject &root)
{
    QJsonArray arr = root.value("model").toObject().value("tags").toArray();
    if (arr.isEmpty()) arr = root.value("tags").toArray();

    QStringList tags;
    QSet<QString> seen;
    for (const QJsonValue &value : arr) {
        const QString tag = value.toString().trimmed();
        if (tag.isEmpty()) continue;
        const QString key = tag.toCaseFolded();
        if (seen.contains(key)) continue;
        seen.insert(key);
        tags.append(tag);
    }
    return tags;
}

QString jsonModelCreator(const QJsonObject &root)
{
    QJsonObject creator = root.value("model").toObject().value("creator").toObject();
    if (creator.isEmpty()) creator = root.value("creator").toObject();
    QString name = creator.value("username").toString().trimmed();
    if (name.isEmpty()) name = creator.value("name").toString().trimmed();
    return name;
}

QString readModelCreatorAvatarFromJson(const QJsonObject &root)
{
    QJsonObject creator = root.value("model").toObject().value("creator").toObject();
    if (creator.isEmpty()) creator = root.value("creator").toObject();
    return creator.value("image").toString().trimmed();
}

QString metadataShaFromRoot(const QJsonObject &root)
{
    for (const QJsonValue &fileVal : root.value("files").toArray()) {
        const QString sha = fileVal.toObject().value("hashes").toObject().value("SHA256").toString().trimmed();
        if (!sha.isEmpty()) return sha;
    }
    return QString();
}

QString metadataBrowserUrlFromRoot(const QJsonObject &root)
{
    const QString customUrl = root.value("modelUrl").toString().trimmed();
    if (!customUrl.isEmpty()) return customUrl;

    const QString source = root.value("metadataSource").toString().trimmed();
    QString sourceUrl = root.value("sourceUrl").toString().trimmed();
    if (source.compare("civarchive", Qt::CaseInsensitive) == 0
        || sourceUrl.contains("civarchive.com", Qt::CaseInsensitive)) {
        if (sourceUrl.isEmpty()) {
            QString sha = metadataShaFromRoot(root);
            sha.remove(QRegularExpression("[^A-Fa-f0-9]"));
            if (!sha.isEmpty()) sourceUrl = QString("https://civarchive.com/sha256/%1").arg(sha.toLower());
        }
        return sourceUrl;
    }

    const int modelId = root.value("modelId").toInt(root.value("model").toObject().value("id").toInt());
    if (modelId > 0) return QString("https://civitai.com/models/%1").arg(modelId);
    return {};
}

QVector<ImageInfo> imageInfosFromVersionJson(const QJsonObject &root)
{
    QVector<ImageInfo> result;
    const QJsonArray images = root.value("images").toArray();
    for (const QJsonValue &val : images) {
        const QJsonObject imgObj = val.toObject();
        const QString type = imgObj.value("type").toString();
        const QString url = imgObj.value("url").toString();
        if (type == "video" || url.endsWith(".mp4", Qt::CaseInsensitive) || url.endsWith(".webm", Qt::CaseInsensitive)) {
            continue;
        }

        ImageInfo img;
        img.url = url;
        img.hash = imgObj.value("hash").toString();
        img.width = imgObj.value("width").toInt();
        img.height = imgObj.value("height").toInt();
        img.nsfwLevel = imgObj.value("nsfwLevel").toInt();
        img.nsfw = img.nsfwLevel > 1;
        const QJsonObject meta = imgObj.value("meta").toObject();
        img.prompt = meta.value("prompt").toString();
        img.negativePrompt = meta.value("negativePrompt").toString();
        img.sampler = meta.value("sampler").toString();
        if (meta.contains("steps")) img.steps = meta.value("steps").isString() ? meta.value("steps").toString() : QString::number(meta.value("steps").toInt());
        if (meta.contains("cfgScale")) img.cfgScale = meta.value("cfgScale").isString() ? meta.value("cfgScale").toString() : QString::number(meta.value("cfgScale").toDouble());
        if (meta.contains("seed")) img.seed = meta.value("seed").isString() ? meta.value("seed").toString() : QString::number(meta.value("seed").toVariant().toLongLong());
        result.append(img);
    }
    return result;
}

QJsonObject selectVersionFileForLocalModel(const QJsonArray &files,
                                           const QString &localFilePath,
                                           const QString &preferredSha256)
{
    const QString normalizedHash = preferredSha256.trimmed();
    if (!normalizedHash.isEmpty()) {
        for (const QJsonValue &value : files) {
            const QJsonObject file = value.toObject();
            const QString sha256 = file.value("hashes").toObject().value("SHA256").toString();
            if (!sha256.isEmpty() && sha256.compare(normalizedHash, Qt::CaseInsensitive) == 0) {
                return file;
            }
        }
    }

    const QString localFileName = QFileInfo(localFilePath).fileName();
    if (!localFileName.isEmpty()) {
        for (const QJsonValue &value : files) {
            const QJsonObject file = value.toObject();
            if (file.value("name").toString().compare(localFileName, Qt::CaseInsensitive) == 0) {
                return file;
            }
        }
    }

    QJsonObject fallback;
    for (const QJsonValue &value : files) {
        const QJsonObject file = value.toObject();
        if (fallback.isEmpty()) fallback = file;
        if (file.value("primary").toBool() || file.value("is_primary").toBool()) return file;
    }
    return fallback;
}

QJsonObject mergeCivitaiModelIntoVersion(const QJsonObject &versionRoot, const QJsonObject &modelRoot)
{
    QJsonObject merged = versionRoot;
    if (modelRoot.isEmpty()) return merged;

    QJsonObject modelObj = merged.value("model").toObject();
    for (auto it = modelRoot.constBegin(); it != modelRoot.constEnd(); ++it) {
        if (it.key() == "metadataSource" || it.key() == "sourceUrl") continue;
        modelObj.insert(it.key(), it.value());
    }
    merged["model"] = modelObj;

    if (!merged.contains("modelId")) merged["modelId"] = modelRoot.value("id").toInt();
    if (!merged.contains("description") && modelRoot.contains("description")) {
        merged["description"] = modelRoot.value("description");
    }
    return merged;
}

QStringList trainedWords(const QJsonObject &root)
{
    QStringList result;
    for (const QJsonValue &value : root.value("trainedWords").toArray()) {
        QString word = value.toString().trimmed();
        if (word.endsWith(',')) word.chop(1);
        if (!word.isEmpty()) result.append(word);
    }
    return result;
}

QString previewPath(const QString &dirPath, const QString &baseName, int index)
{
    if (dirPath.isEmpty()) return {};
    const QString suffix = index == 0 ? ".preview.png" : QString(".preview.%1.png").arg(index);
    return QFileInfo(QDir(dirPath).filePath(baseName + suffix)).absoluteFilePath();
}

ModelMeta parseVersion(const QJsonObject &root, const QString &filePath,
                       const QString &baseName, const QString &preferredSha256)
{
    ModelMeta meta;
    meta.filePath = filePath;
    // 1. 基础名称
    QString modelName = root["model"].toObject()["name"].toString();
    QString versionName = root["name"].toString();
    meta.modelName = modelName;
    meta.versionName = versionName;
    if (meta.modelName.isEmpty()) meta.modelName = baseName;
    if (!meta.modelName.isEmpty()) {
        meta.name = meta.versionName.isEmpty() ? meta.modelName : meta.modelName + " [" + meta.versionName + "]";
    }

    // ID (用于打开网页)
    int modelId = root["modelId"].toInt(root.value("model").toObject().value("id").toInt());
    meta.modelId = modelId;
    meta.versionId = root["id"].toInt();
    meta.modelUrl = metadataBrowserUrlFromRoot(root);
    meta.isLocalEdited = root["localEdited"].toBool(false);
    meta.isLocalOnly = root["localOnly"].toBool(false);
    if (!meta.isLocalOnly && modelId <= 0 && meta.modelUrl.isEmpty()) {
        meta.isLocalOnly = true;
    }
    if (meta.fileName.isEmpty() && !meta.filePath.isEmpty()) {
        meta.fileName = QFileInfo(meta.filePath).fileName();
    }
    meta.creatorName = jsonModelCreator(root);
    meta.creatorAvatarUrl = readModelCreatorAvatarFromJson(root);
    meta.modelTags = jsonModelTags(root);

    meta.trainedWordsGroups = trainedWords(root);

    meta.images = imageInfosFromVersionJson(root);

    // 4. 其他信息 (之前漏掉了 createdAt)
    meta.description = root["description"].toString();
    meta.baseModel = root["baseModel"].toString();
    meta.type = root["model"].toObject()["type"].toString();
    meta.nsfw = root["model"].toObject()["nsfw"].toBool();

    // === 关键修复：补上日期读取 ===
    meta.createdAt = root["createdAt"].toString();
    // ===========================

    QJsonObject stats = root["stats"].toObject();
    meta.downloadCount = stats["downloadCount"].toInt();
    meta.thumbsUpCount = stats["thumbsUpCount"].toInt();

    QJsonArray files = root["files"].toArray();
    if(!files.isEmpty()) {
        const QJsonObject f = selectVersionFileForLocalModel(files, meta.filePath, preferredSha256);
        meta.fileSizeMB = f["sizeKB"].toDouble() / 1024.0;
        meta.fileNameServer = f["name"].toString();
        meta.sha256 = f["hashes"].toObject()["SHA256"].toString();
    }

    return meta;
}

bool readLocalJson(const QString &dirPath, const QString &baseName, ModelMeta &meta)
{
    if (dirPath.isEmpty()) return false;
    QString jsonPath = QDir(dirPath).filePath(baseName + ".json");

    QFile file(jsonPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) return false;

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "Invalid model metadata JSON:" << jsonPath << parseError.errorString();
        return false;
    }
    QJsonObject root = doc.object();

    ModelMeta result = parseVersion(root, meta.filePath, baseName);
    QString bestPreviewPath = previewPath(dirPath, baseName, 0);

    if (QFile::exists(bestPreviewPath)) {
        QImageReader reader(bestPreviewPath);
        if (reader.canRead()) {
            result.previewPath = bestPreviewPath;
        } else {
            result.previewPath = ""; // 文件坏了或不是图片
        }
    } else {
        result.previewPath = ""; // 没找到文件
    }

    meta = std::move(result);
    return true;
}
}
