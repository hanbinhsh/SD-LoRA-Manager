#include "civarchiveparser.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <QUrlQuery>

namespace CivArchiveParser {
namespace {
QString htmlDecodeMinimal(QString text)
{
    text.replace("&quot;", "\"");
    text.replace("&#34;", "\"");
    text.replace("&#x22;", "\"");
    text.replace("&amp;", "&");
    text.replace("&#38;", "&");
    text.replace("&lt;", "<");
    text.replace("&gt;", ">");
    text.replace("&#x2F;", "/");
    return text;
}

QJsonObject findObjectWithModelVersions(const QJsonValue &value)
{
    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        if (obj.value("modelVersions").isArray()) return obj;
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            const QJsonObject found = findObjectWithModelVersions(it.value());
            if (!found.isEmpty()) return found;
        }
    } else if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        for (const QJsonValue &child : arr) {
            const QJsonObject found = findObjectWithModelVersions(child);
            if (!found.isEmpty()) return found;
        }
    }
    return {};
}

QJsonObject findObjectWithVersionFiles(const QJsonValue &value)
{
    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        if (obj.value("files").isArray() && (obj.contains("modelId") || obj.contains("model"))) return obj;
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            const QJsonObject found = findObjectWithVersionFiles(it.value());
            if (!found.isEmpty()) return found;
        }
    } else if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        for (const QJsonValue &child : arr) {
            const QJsonObject found = findObjectWithVersionFiles(child);
            if (!found.isEmpty()) return found;
        }
    }
    return {};
}

QString normalizedSha256Text(QString hash)
{
    hash.remove(QRegularExpression("[^A-Fa-f0-9]"));
    return hash.toLower();
}

QString shaFromCivArchiveSourceUrl(const QString &sourceUrl)
{
    const QUrl url(sourceUrl);
    const QStringList parts = url.path().split('/', Qt::SkipEmptyParts);
    for (int i = 0; i + 1 < parts.size(); ++i) {
        if (parts.at(i).compare("sha256", Qt::CaseInsensitive) == 0) {
            return normalizedSha256Text(parts.at(i + 1));
        }
    }
    return {};
}

QString civArchiveFileSha(const QJsonObject &file)
{
    QString sha = file.value("sha256").toString();
    if (sha.isEmpty()) sha = file.value("hashes").toObject().value("SHA256").toString();
    return normalizedSha256Text(sha);
}

bool civArchiveVersionMatchesHash(const QJsonObject &version, const QString &hash)
{
    if (hash.isEmpty()) return true;
    for (const QJsonValue &fileVal : version.value("files").toArray()) {
        if (civArchiveFileSha(fileVal.toObject()) == hash) return true;
    }
    return false;
}

bool findCivArchiveModelAndVersion(const QJsonValue &value,
                                   const QString &hash,
                                   QJsonObject &archiveModel,
                                   QJsonObject &archiveVersion)
{
    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        const QJsonObject version = obj.value("version").toObject();
        if (!version.isEmpty()
            && version.value("files").isArray()
            && (obj.contains("id") || obj.contains("name"))
            && civArchiveVersionMatchesHash(version, hash)) {
            archiveModel = obj;
            archiveVersion = version;
            return true;
        }

        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            if (findCivArchiveModelAndVersion(it.value(), hash, archiveModel, archiveVersion)) return true;
        }
    } else if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        for (const QJsonValue &child : arr) {
            if (findCivArchiveModelAndVersion(child, hash, archiveModel, archiveVersion)) return true;
        }
    }
    return false;
}

QJsonObject civArchiveFileToCivitaiFile(QJsonObject file)
{
    if (!file.contains("sizeKB") && file.contains("size_kb")) file["sizeKB"] = file.value("size_kb");
    if (!file.contains("downloadUrl") && file.contains("download_url")) file["downloadUrl"] = file.value("download_url");
    if (!file.contains("primary") && file.contains("is_primary")) file["primary"] = file.value("is_primary");
    if (!file.contains("modelId") && file.contains("model_id")) file["modelId"] = file.value("model_id");
    if (!file.contains("modelVersionId") && file.contains("model_version_id")) file["modelVersionId"] = file.value("model_version_id");

    QJsonObject hashes = file.value("hashes").toObject();
    const QString sha = file.value("sha256").toString().trimmed();
    if (!sha.isEmpty() && hashes.value("SHA256").toString().isEmpty()) hashes["SHA256"] = sha;
    if (!hashes.isEmpty()) file["hashes"] = hashes;
    return file;
}

QJsonObject civArchiveImageToCivitaiImage(QJsonObject image)
{
    if (!image.contains("url") && image.contains("image_url")) image["url"] = image.value("image_url");
    if (!image.contains("nsfwLevel") && image.contains("nsfw_level")) image["nsfwLevel"] = image.value("nsfw_level");
    return image;
}

QJsonObject civArchiveVersionToCivitaiVersion(QJsonObject version, const QJsonObject &archiveModel)
{
    if (!version.contains("modelId")) {
        const int modelId = version.value("model_id").toInt(archiveModel.value("id").toInt());
        if (modelId > 0) version["modelId"] = modelId;
    }
    if (!version.contains("baseModel") && version.contains("base_model")) version["baseModel"] = version.value("base_model");
    if (!version.contains("baseModelType") && version.contains("base_model_type")) version["baseModelType"] = version.value("base_model_type");
    if (!version.contains("publishedAt") && version.contains("created_at")) version["publishedAt"] = version.value("created_at");
    if (!version.contains("createdAt") && version.contains("created_at")) version["createdAt"] = version.value("created_at");
    if (!version.contains("updatedAt") && version.contains("updated_at")) version["updatedAt"] = version.value("updated_at");
    if (!version.contains("downloadUrl") && version.contains("download_url")) version["downloadUrl"] = version.value("download_url");
    if (!version.contains("trainedWords") && version.value("trigger").isArray()) version["trainedWords"] = version.value("trigger");

    QJsonArray files;
    for (const QJsonValue &fileVal : version.value("files").toArray()) {
        files.append(civArchiveFileToCivitaiFile(fileVal.toObject()));
    }
    if (!files.isEmpty()) version["files"] = files;

    QJsonArray images;
    for (const QJsonValue &imageVal : version.value("images").toArray()) {
        images.append(civArchiveImageToCivitaiImage(imageVal.toObject()));
    }
    if (!images.isEmpty()) version["images"] = images;
    return version;
}

QJsonObject civArchiveModelToCivitaiRoot(QJsonObject archiveModel, const QJsonObject &archiveVersion)
{
    QJsonObject version = civArchiveVersionToCivitaiVersion(archiveVersion, archiveModel);

    QJsonObject root = archiveModel;
    root.remove("version");
    root.remove("versions");
    root.remove("meta");
    if (!root.contains("nsfw") && root.contains("is_nsfw")) root["nsfw"] = root.value("is_nsfw");
    if (!root.contains("nsfwLevel") && root.contains("nsfw_level")) root["nsfwLevel"] = root.value("nsfw_level");

    QJsonObject creator = root.value("creator").toObject();
    const QString creatorUser = root.value("creator_username").toString(root.value("username").toString()).trimmed();
    const QString creatorName = root.value("creator_name").toString(creatorUser).trimmed();
    if (!creatorUser.isEmpty() && creator.value("username").toString().isEmpty()) creator["username"] = creatorUser;
    if (!creatorName.isEmpty() && creator.value("name").toString().isEmpty()) creator["name"] = creatorName;
    if (!creator.isEmpty()) root["creator"] = creator;

    QJsonArray versions;
    versions.append(version);
    root["modelVersions"] = versions;
    return root;
}

QJsonObject modelRootFromVersionObject(const QJsonObject &version)
{
    if (version.isEmpty()) return {};
    QJsonObject model = version.value("model").toObject();
    const int modelId = version.value("modelId").toInt(model.value("id").toInt());
    if (modelId > 0 && !model.contains("id")) model["id"] = modelId;

    QJsonObject root = model;
    if (root.isEmpty()) root["id"] = modelId;
    if (!root.contains("id") && modelId > 0) root["id"] = modelId;
    QJsonArray versions;
    versions.append(version);
    root["modelVersions"] = versions;
    return root;
}

}

bool parseCivArchivePayload(const QByteArray &data,
                            const QString &sourceUrl,
                            QJsonObject &modelRoot,
                            QJsonObject &versionHint)
{
    modelRoot = {};
    versionHint = {};
    if (data.trimmed().isEmpty()) return false;
    const QString sourceHash = shaFromCivArchiveSourceUrl(sourceUrl);

    auto acceptJson = [&](const QJsonDocument &doc) -> bool {
        if (doc.isNull()) return false;
        const QJsonValue rootValue = doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(doc.array());
        modelRoot = findObjectWithModelVersions(rootValue);
        versionHint = findObjectWithVersionFiles(rootValue);
        if (modelRoot.isEmpty()) {
            QJsonObject archiveModel;
            QJsonObject archiveVersion;
            if (findCivArchiveModelAndVersion(rootValue, sourceHash, archiveModel, archiveVersion)) {
                modelRoot = civArchiveModelToCivitaiRoot(archiveModel, archiveVersion);
                versionHint = civArchiveVersionToCivitaiVersion(archiveVersion, archiveModel);
            }
        }
        if (modelRoot.isEmpty() && !versionHint.isEmpty()) {
            modelRoot = modelRootFromVersionObject(versionHint);
        }
        if (modelRoot.isEmpty()) return false;
        modelRoot["metadataSource"] = QStringLiteral("civarchive");
        modelRoot["sourceUrl"] = sourceUrl;
        return true;
    };

    QJsonParseError directErr;
    if (acceptJson(QJsonDocument::fromJson(data, &directErr))) return true;

    const QString html = QString::fromUtf8(data);
    static const QRegularExpression nextDataRegex(
        "<script[^>]*id=[\"']__NEXT_DATA__[\"'][^>]*>(.*?)</script>",
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch nextMatch = nextDataRegex.match(html);
    if (nextMatch.hasMatch()) {
        const QByteArray jsonBytes = htmlDecodeMinimal(nextMatch.captured(1).trimmed()).toUtf8();
        if (acceptJson(QJsonDocument::fromJson(jsonBytes))) return true;
    }

    static const QRegularExpression jsonScriptRegex(
        "<script[^>]*type=[\"']application/(?:ld\\+)?json[\"'][^>]*>(.*?)</script>",
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = jsonScriptRegex.globalMatch(html);
    while (it.hasNext()) {
        const QByteArray jsonBytes = htmlDecodeMinimal(it.next().captured(1).trimmed()).toUtf8();
        if (acceptJson(QJsonDocument::fromJson(jsonBytes))) return true;
    }
    return false;
}

QUrl civArchiveLookupUrl(const MetadataSyncJob &job)
{
    QString hash = job.snapshot.currentSha256.trimmed();
    hash.remove(QRegularExpression("[^A-Fa-f0-9]"));
    if (!hash.isEmpty()) {
        return QUrl(QString("https://civarchive.com/sha256/%1").arg(hash.toLower()));
    }
    if (job.snapshot.modelId > 0) {
        QUrl url(QString("https://civarchive.com/models/%1").arg(job.snapshot.modelId));
        if (job.snapshot.currentVersionId > 0) {
            QUrlQuery query(url);
            query.addQueryItem("modelVersionId", QString::number(job.snapshot.currentVersionId));
            url.setQuery(query);
        }
        return url;
    }
    return {};
}

}
