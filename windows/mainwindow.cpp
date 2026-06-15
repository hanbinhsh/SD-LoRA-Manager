#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QDateTime>
#include <QDebug>
#include <QClipboard>
#include <QScreen>
#include <QMouseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QPainter>
#include <QPushButton>
#include <QLabel>
#include <QScrollBar>
#include <QScrollArea>
#include <QMenu>
#include <QInputDialog>
#include <QComboBox>
#include <QJsonArray>
#include <QPainterPath>
#include <QImageReader>
#include <QImageWriter>
#include <QPair>
#include <QTimer>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QGraphicsBlurEffect>
#include <QTextDocument>
#include <QTextEdit>
#include <QRegularExpression>
#include <QImage>
#include <QDirIterator>
#include <QtEndian>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSignalBlocker>
#include <QColorDialog>
#include <QBrush>
#include <QStyledItemDelegate>
#include <QApplication>
#include <QCoreApplication>
#include <QStyle>
#include <QSizePolicy>
#include <QProgressBar>
#include <QSaveFile>
#include <QUrlQuery>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <utility>
#include <QElapsedTimer>
#include <QSharedPointer>

#include "utils/imageloader.h"
#include "utils/imagemetadataparser.h"
#include "tools/comfyworkflowviewer.h"
#include "tools/llmpromptwidget.h"
#include "dialogs/pathlistdialog.h"
#include "tools/tagbrowserwidget.h"
#include "tools/syncwidget.h"
#include "tools/promptparserwidget.h"
#include "tools/usageanalysiswidget.h"
#include "tools/prompttemplatelibrarywidget.h"
#include "pages/downloadmanager.h"
#include "pages/downloadspage.h"
#include "pages/settingspage.h"
#include "pages/aboutpage.h"
#include "dialogs/modelnotedialog.h"
#include "utils/styleconstants.h"
#include "utils/fileutils.h"
#include "utils/tagutils.h"

namespace {
QString normalizedHomeTagKey(const QString &tag)
{
    return tag.trimmed().toCaseFolded();
}

PreviewMetadataPayload previewPayloadFromImageInfo(const ImageInfo &img)
{
    PreviewMetadataPayload payload;
    payload.prompt = img.prompt;
    payload.negativePrompt = img.negativePrompt;
    payload.sampler = img.sampler;
    payload.cfgScale = img.cfgScale;
    payload.steps = img.steps;
    payload.seed = img.seed;
    payload.width = img.width;
    payload.height = img.height;
    payload.nsfwLevel = img.nsfwLevel;
    return payload;
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

bool writePreviewMetadataToPath(const QString &path,
                                const QString &parameters,
                                const QString &prompt,
                                const QString &negativePrompt)
{
    if (path.isEmpty() || parameters.isEmpty() || !QFile::exists(path)) return false;

    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) return false;
    const QByteArray data = input.readAll();
    input.close();
    if (data.size() < 12) return false;

    const bool isPng = data.startsWith("\x89PNG\r\n\x1a\n");
    const bool isJpeg = data.startsWith("\xff\xd8");
    const bool isWebp = data.size() >= 12 && data.left(4) == "RIFF" && data.mid(8, 4) == "WEBP";
    if (!isPng && !isJpeg && !isWebp) return false;

    // Avoid libpng spam on partial/corrupt downloads. A complete PNG must contain IEND.
    if (isPng && !data.contains("IEND")) {
        return false;
    }

    QImage image;
    if (!image.loadFromData(data)) return false;
    if (image.isNull()) return false;

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) return false;
    QImageWriter writer(&output, "png");
    writer.setText("parameters", parameters);
    writer.setText("civitai_prompt", prompt);
    writer.setText("civitai_negative_prompt", negativePrompt);
    if (!writer.write(image)) return false;
    return output.commit();
}

bool previewFileAlreadyHasPromptMetadata(const QString &path)
{
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) return false;
    const QByteArray data = file.readAll();
    return data.contains("parameters") || data.contains("civitai_prompt");
}

QString loadQssResource(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

struct ThemeBundle {
    QString mainQss;
    QString toolQss;
    QString dialogQss;
    QString status;
    bool ok = true;
};

QString themeDisplayName(const QString &themeId)
{
    if (themeId == "midnight_blue") return "Midnight Blue";
    if (themeId == "light") return "Light";
    if (themeId == "high_contrast") return "High Contrast";
    if (themeId == "custom_qss") return "Custom QSS";
    return "Steam Dark";
}

ThemeBundle loadThemeBundle(const QString &themeId, const QString &customPath)
{
    ThemeBundle bundle;
    const QString baseMain = loadQssResource(":/styles/mainwindow.qss");
    const QString baseTool = loadQssResource(":/styles/toolpage.qss");
    const QString baseDialog = loadQssResource(":/styles/dialog.qss");
    bundle.dialogQss = baseDialog;

    auto withOverrides = [&](const QString &mainOverride, const QString &toolOverride, const QString &name) {
        bundle.mainQss = baseMain + "\n" + loadQssResource(mainOverride);
        bundle.toolQss = baseTool + "\n" + loadQssResource(toolOverride);
        bundle.dialogQss = baseDialog;
        bundle.status = QString("当前主题：%1").arg(name);
        bundle.ok = true;
    };

    if (themeId == "custom_qss") {
        QFile file(customPath);
        if (!customPath.trimmed().isEmpty() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString customQss = QString::fromUtf8(file.readAll());
            bundle.mainQss = baseMain + "\n" + customQss;
            bundle.toolQss = baseTool + "\n" + customQss;
            bundle.dialogQss = baseDialog + "\n" + customQss;
            bundle.status = QString("当前主题：Custom QSS (%1)").arg(QFileInfo(customPath).fileName());
            bundle.ok = true;
            return bundle;
        }
        bundle.mainQss = baseMain;
        bundle.toolQss = baseTool;
        bundle.dialogQss = baseDialog;
        bundle.status = "自定义 QSS 读取失败，已保留 Steam Dark。";
        bundle.ok = false;
        return bundle;
    }

    if (themeId == "midnight_blue") {
        withOverrides(":/styles/themes/midnight_blue_main.qss", ":/styles/themes/midnight_blue_tool.qss", "Midnight Blue");
    } else if (themeId == "light") {
        withOverrides(":/styles/themes/light_main.qss", ":/styles/themes/light_tool.qss", "Light");
    } else if (themeId == "high_contrast") {
        withOverrides(":/styles/themes/high_contrast_main.qss", ":/styles/themes/high_contrast_tool.qss", "High Contrast");
    } else {
        bundle.mainQss = baseMain;
        bundle.toolQss = baseTool;
        bundle.dialogQss = baseDialog;
        bundle.status = "当前主题：Steam Dark";
        bundle.ok = true;
    }
    return bundle;
}

QString metadataIsoTimeForDisplay(const QString &iso)
{
    QDateTime dt = QDateTime::fromString(iso, Qt::ISODate);
    if (!dt.isValid()) return iso;
    return dt.toLocalTime().toString("yyyy-MM-dd HH:mm");
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

QVector<MetadataScanItem> scanMetadataItemsWorker(QVector<MetadataScanItem> items)
{
    for (MetadataScanItem &item : items) {
        if (!QFileInfo::exists(item.filePath)) {
            item.category = "invalid";
            item.status = "模型文件不存在";
            continue;
        }

        QFileInfo jsonInfo(item.jsonPath);
        if (!jsonInfo.exists()) {
            item.category = "missing";
            item.status = "缺少 metadata JSON";
            continue;
        }

        QFile file(item.jsonPath);
        if (!file.open(QIODevice::ReadOnly)) {
            item.category = "invalid";
            item.status = "无法读取 metadata JSON";
            continue;
        }

        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            item.category = "invalid";
            item.status = "metadata JSON 解析失败: " + err.errorString();
            continue;
        }

        const QJsonObject root = doc.object();
        const int modelId = root.value("modelId").toInt(root.value("model").toObject().value("id").toInt());
        const int versionId = root.value("id").toInt();
        const QString sha = metadataShaFromRoot(root);
        if (item.modelIdText.isEmpty() && modelId > 0) item.modelIdText = QString::number(modelId);
        if (item.versionIdText.isEmpty() && versionId > 0) item.versionIdText = QString::number(versionId);
        if (item.sha256.isEmpty()) item.sha256 = sha;
        item.localEdited = root.value("localEdited").toBool(false) || root.value("localOnly").toBool(false);

        const QString syncedAt = root.value("syncedAt").toString().trimmed();
        if (!syncedAt.isEmpty()) {
            item.lastSyncedAt = metadataIsoTimeForDisplay(syncedAt);
            item.lastSyncedSource = "同步时间";
        } else {
            item.lastSyncedAt = jsonInfo.lastModified().toString("yyyy-MM-dd HH:mm");
            item.lastSyncedSource = "文件时间";
        }

        if (item.localEdited) {
            item.category = "local";
            item.status = "本地/已编辑 metadata";
        } else if (!item.syncFailure.isEmpty()) {
            item.category = "failed";
            item.status = "存在同步失败缓存: " + item.syncFailure;
        } else if (modelId <= 0 && versionId <= 0 && item.sha256.isEmpty()) {
            item.category = "no_ids";
            item.status = "缺少 modelId/versionId/SHA256";
        } else {
            item.category = "existing";
            item.status = "metadata 可用";
        }
    }
    return items;
}

QVector<MetadataHealthIssue> metadataHealthCheckWorker(QVector<MetadataScanItem> items)
{
    QVector<MetadataHealthIssue> issues;
    const QVector<MetadataScanItem> scanned = scanMetadataItemsWorker(std::move(items));
    for (const MetadataScanItem &item : scanned) {
        if (!QFileInfo::exists(item.filePath)) {
            issues.append({"错误", item.displayName, "模型文件不存在", "检查模型路径或从列表移除失效项", item.filePath});
            continue;
        }
        if (item.category == "missing") {
            issues.append({"警告", item.displayName, "缺少 metadata JSON", "可在下载页元信息扫描中同步", item.filePath});
        } else if (item.category == "invalid") {
            issues.append({"错误", item.displayName, item.status, "检查 JSON 文件或重新同步 metadata", item.filePath});
        }
        if (!item.syncFailure.isEmpty()) {
            issues.append({"警告", item.displayName, "存在同步失败缓存", item.syncFailure, item.filePath});
        }
        if (item.category == "no_ids") {
            issues.append({"警告", item.displayName, "缺少 Civitai 识别字段", "缺少 modelId/versionId/sha256，更新检测可能无法判断", item.filePath});
        }
        if (item.previewPath.isEmpty() || !QFileInfo::exists(item.previewPath)) {
            issues.append({"信息", item.displayName, "缺少本地封面预览图", "可在详情页或元信息同步后重新同步预览图", item.filePath});
        }
        if (item.localEdited) {
            issues.append({"信息", item.displayName, "本地/已编辑模型", "同步或更新前会按本地保护逻辑确认", item.filePath});
        }
    }
    return issues;
}

class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget *parent = nullptr, int margin = 0, int hSpacing = 6, int vSpacing = 6)
        : QLayout(parent), m_hSpace(hSpacing), m_vSpace(vSpacing)
    {
        setContentsMargins(margin, margin, margin, margin);
    }

    ~FlowLayout() override
    {
        QLayoutItem *item = nullptr;
        while ((item = takeAt(0)) != nullptr) delete item;
    }

    void addItem(QLayoutItem *item) override { itemList.append(item); }
    int count() const override { return itemList.size(); }
    QLayoutItem *itemAt(int index) const override { return itemList.value(index); }
    QLayoutItem *takeAt(int index) override
    {
        if (index < 0 || index >= itemList.size()) return nullptr;
        return itemList.takeAt(index);
    }

    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return doLayout(QRect(0, 0, width, 0), true); }
    QSize minimumSize() const override
    {
        QSize size;
        for (QLayoutItem *item : itemList) size = size.expandedTo(item->minimumSize());
        const QMargins margins = contentsMargins();
        size += QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
        return size;
    }
    QSize sizeHint() const override { return minimumSize(); }
    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);
        doLayout(rect, false);
    }

private:
    int doLayout(const QRect &rect, bool testOnly) const
    {
        const QMargins margins = contentsMargins();
        QRect effectiveRect = rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
        int x = effectiveRect.x();
        int y = effectiveRect.y();
        int lineHeight = 0;

        for (QLayoutItem *item : itemList) {
            const int spaceX = m_hSpace;
            const int spaceY = m_vSpace;
            const QSize itemSize = item->sizeHint();
            int nextX = x + itemSize.width() + spaceX;
            if (nextX - spaceX > effectiveRect.right() && lineHeight > 0) {
                x = effectiveRect.x();
                y += lineHeight + spaceY;
                nextX = x + itemSize.width() + spaceX;
                lineHeight = 0;
            }
            if (!testOnly) item->setGeometry(QRect(QPoint(x, y), itemSize));
            x = nextX;
            lineHeight = qMax(lineHeight, itemSize.height());
        }
        return y + lineHeight - rect.y() + margins.bottom();
    }

    QList<QLayoutItem*> itemList;
    int m_hSpace = 6;
    int m_vSpace = 6;
};
}

class HighlightItemDelegate : public QStyledItemDelegate
{
public:
    explicit HighlightItemDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();

        // Draw the native item background first, then place the user highlight
        // under the icon/text so translucent colors do not wash out content.
        QStyleOptionViewItem backgroundOpt(opt);
        backgroundOpt.text.clear();
        backgroundOpt.icon = QIcon();
        backgroundOpt.features &= ~QStyleOptionViewItem::HasDisplay;
        backgroundOpt.features &= ~QStyleOptionViewItem::HasDecoration;
        style->drawControl(QStyle::CE_ItemViewItem, &backgroundOpt, painter, widget);

        const QColor highlight = index.data(ROLE_MODEL_HIGHLIGHT_COLOR).value<QColor>();
        if (highlight.isValid() && highlight.alpha() > 0) {
            painter->save();
            painter->fillRect(opt.rect, highlight);
            painter->restore();
        }

        if (!opt.icon.isNull()) {
            const QRect iconRect = style->subElementRect(QStyle::SE_ItemViewItemDecoration, &opt, widget);
            QIcon::Mode mode = QIcon::Normal;
            if (!(opt.state & QStyle::State_Enabled)) {
                mode = QIcon::Disabled;
            } else if (opt.state & QStyle::State_Selected) {
                mode = QIcon::Selected;
            }
            const QIcon::State state = (opt.state & QStyle::State_Open) ? QIcon::On : QIcon::Off;
            opt.icon.paint(painter, iconRect, opt.decorationAlignment, mode, state);
        }

        if (!opt.text.isEmpty()) {
            const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
            const QString text = opt.fontMetrics.elidedText(opt.text, opt.textElideMode, textRect.width());
            const QPalette::ColorRole role = (opt.state & QStyle::State_Selected)
                                                 ? QPalette::HighlightedText
                                                 : QPalette::Text;
            style->drawItemText(painter,
                                textRect,
                                opt.displayAlignment,
                                opt.palette,
                                opt.state & QStyle::State_Enabled,
                                text,
                                role);
        }
    }
};

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    applyApplicationTheme(optThemeId, optCustomThemePath, false);
    downloadsPage = new DownloadsPage(ui->pageDownloads);
    if (ui->pageDownloads && ui->pageDownloads->layout()) {
        ui->pageDownloads->layout()->addWidget(downloadsPage);
    }
    settingsPage = new SettingsPage(ui->pageSettings);
    if (ui->pageSettings && ui->pageSettings->layout()) {
        ui->pageSettings->layout()->addWidget(settingsPage);
    }
    aboutPage = new AboutPage(ui->pageAbout);
    if (ui->pageAbout && ui->pageAbout->layout()) {
        ui->pageAbout->layout()->addWidget(aboutPage);
    }
    aboutPage->setVersionText(CURRENT_VERSION);
    connect(aboutPage, &AboutPage::checkUpdateRequested, this, &MainWindow::onCheckUpdateClicked);

    currentUserAgent = getRandomUserAgent();

    // 初始化运行时状态
    isFirstTreeRefresh = true;
    startupTreeScrollPos = 0;
    // 线程池初始化 (此时还没有读取配置，先不设最大数)
    threadPool = new QThreadPool(this);
    backgroundThreadPool = new QThreadPool(this);
    // Hash 计算器
    hashWatcher = new QFutureWatcher<QString>(this);
    connect(hashWatcher, &QFutureWatcherBase::finished, this, &MainWindow::onHashCalculated);
    // 图片加载器
    imageLoadWatcher = new QFutureWatcher<ImageLoadResult>(this);
    connect(imageLoadWatcher, &QFutureWatcher<ImageLoadResult>::finished, this, [this](){
        // A. 获取后台加载的原图
        ImageLoadResult result = imageLoadWatcher->result();
        if (result.path != currentHeroPath) {
            qDebug() << "Discarding obsolete image load:" << result.path;
            return;
        }
        if (!result.valid) {
            // 图片无效，淡出
            nextHeroPixmap = QPixmap();
            nextBlurredBgPix = QPixmap();
        } else {
            QPixmap rawPix = QPixmap::fromImage(result.originalImg);

            // --- NSFW 大图处理 ---
            bool shouldBlur = false;
            // 判断这张图是否为 NSFW。可以通过 path 在 currentMeta.images 里查找
            // 简单处理：如果是当前详情页的封面，直接看 currentMeta.nsfw
            if (optFilterNSFW && optNSFWMode == 1) {
                // 检查该图在详情页列表中是否标记为 NSFW
                for(const auto& img : currentMeta.images) {
                    if(result.path.contains(img.hash) || result.path == currentMeta.previewPath) {
                        if(img.nsfwLevel > optNSFWLevel) shouldBlur = true;
                        break;
                    }
                }
            }
            if (shouldBlur) {
                nextHeroPixmap = applyNSFWBlur(rawPix);
            } else {
                nextHeroPixmap = rawPix;
            }
            // C. 准备背景图
            QSize targetSize = ui->scrollAreaWidgetContents ? ui->scrollAreaWidgetContents->size() : ui->backgroundLabel->size();
            if (QWidget *viewport = ui->scrollAreaWidgetContents ? ui->scrollAreaWidgetContents->parentWidget() : nullptr) {
                targetSize.setWidth(qMax(targetSize.width(), viewport->width()));
                targetSize.setHeight(qMax(targetSize.height(), viewport->height()));
            }
            if (targetSize.isEmpty()) targetSize = QSize(1920, 1080);
            if (ui->backgroundLabel) {
                const QRect bgRect(QPoint(0, 0), targetSize);
                if (ui->backgroundLabel->geometry() != bgRect) {
                    ui->backgroundLabel->setGeometry(bgRect);
                }
            }
            QSize heroSize = ui->heroFrame->size();
            if (heroSize.isEmpty()) heroSize = QSize(targetSize.width(), 400);
            if (currentBlurredBgPix.isNull() && !currentHeroPixmap.isNull()) {
                currentBlurredBgPix = applyBlurToImage(currentHeroPixmap.toImage(), targetSize, heroSize);
            }
            nextBlurredBgPix = applyBlurToImage(result.originalImg, targetSize, heroSize);
        }
        transitionOpacity = 0.0;
        if (transitionAnim->state() == QAbstractAnimation::Running) {
            transitionAnim->stop();
        }
        transitionAnim->start();
    });

    // 动画初始化
    transitionAnim = new QVariantAnimation(this);
    transitionAnim->setStartValue(0.0f);
    transitionAnim->setEndValue(1.0f);
    transitionAnim->setDuration(250);
    transitionAnim->setEasingCurve(QEasingCurve::InOutQuad);
    connect(transitionAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &val){
        transitionOpacity = val.toFloat();
        ui->heroFrame->update();
        updateBackgroundDuringTransition();
    });
    connect(transitionAnim, &QVariantAnimation::finished, this, [this](){
        currentHeroPixmap = nextHeroPixmap;
        currentBlurredBgPix = nextBlurredBgPix;
        nextHeroPixmap = QPixmap();
        nextBlurredBgPix = QPixmap();
        transitionOpacity = 0.0;
        ui->heroFrame->update();
        updateBackgroundDuringTransition();
    });
    placeholderIcon = generatePlaceholderIcon();

    netManager = new QNetworkAccessManager(this);

    // === 1. 初始化菜单栏 ===
    initMenuBar();
    // === 2. 加载配置 ===
    loadGlobalConfig();   // 从 JSON 读选项
    loadModelSyncFailures();
    // === 应用线程数 ===
    threadPool->setMaxThreadCount(optRenderThreadCount);
    backgroundThreadPool->setMaxThreadCount(optRenderThreadCount);
    // 样式设置
    QGraphicsDropShadowEffect *shadow = new QGraphicsDropShadowEffect;
    shadow->setBlurRadius(20);
    shadow->setColor(Qt::black);
    shadow->setOffset(0, 0);
    ui->lblModelName->setGraphicsEffect(shadow);

    ui->heroFrame->installEventFilter(this);
    ui->heroFrame->setCursor(Qt::PointingHandCursor);

    ui->btnFavorite->setContextMenuPolicy(Qt::CustomContextMenu);

    // 1. 开启像素滚动
    ui->homeGalleryList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    ui->listUserImages->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // 2. 设置滚轮滚一下移动的像素距离
    ui->homeGalleryList->verticalScrollBar()->setSingleStep(40);
    ui->listUserImages->verticalScrollBar()->setSingleStep(40);
    userImageThumbLoadTimer = new QTimer(this);
    userImageThumbLoadTimer->setSingleShot(true);
    connect(userImageThumbLoadTimer, &QTimer::timeout, this, &MainWindow::dispatchVisibleUserImageThumbLoad);
    connect(ui->listUserImages->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
        scheduleVisibleUserImageThumbLoad();
    });
    ui->listUserImages->viewport()->installEventFilter(this);

    ui->collectionTree->setHeaderHidden(true); // 隐藏 "Collection / Model" 表头

    ui->btnModelsTab->setCheckable(true);
    ui->btnCollectionsTab->setCheckable(true);
    ui->btnModelsTab->setAutoExclusive(true);
    ui->btnCollectionsTab->setAutoExclusive(true);
    ui->btnModelsTab->setChecked(true);

    // === 主界面信号连接 ===
    connect(ui->modelList, &QListWidget::itemClicked, this, &MainWindow::onModelListClicked);
    connect(ui->comboSort, QOverload<int>::of(&QComboBox::currentIndexChanged),this, &MainWindow::onSortIndexChanged);
    connect(ui->comboBaseModel, &QComboBox::currentTextChanged,this, &MainWindow::onFilterBaseModelChanged);
    connect(ui->btnModelsTab, &QPushButton::clicked, this, &MainWindow::onModelsTabButtonClicked);
    connect(ui->btnCollectionsTab, &QPushButton::clicked, this, &MainWindow::onCollectionsTabButtonClicked);
    connect(ui->collectionTree, &QTreeWidget::itemClicked, this, &MainWindow::onCollectionTreeItemClicked);
    // 侧边栏右键菜单
    ui->modelList->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->modelList->setSelectionMode(QAbstractItemView::ExtendedSelection); // 开启 Shift/Ctrl 多选
    ui->modelList->setItemDelegate(new HighlightItemDelegate(ui->modelList));
    connect(ui->modelList, &QListWidget::customContextMenuRequested, this, &MainWindow::onSidebarContextMenu);
    ui->collectionTree->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->collectionTree->setItemDelegate(new HighlightItemDelegate(ui->collectionTree));
    connect(ui->collectionTree, &QTreeWidget::customContextMenuRequested, this, &MainWindow::onCollectionTreeContextMenu);
    ui->collectionTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // 工具栏按钮
    connect(ui->btnOpenUrl, &QPushButton::clicked, this, &MainWindow::onOpenUrlClicked);
    connect(ui->btnCopyLoraTag, &QPushButton::clicked, this, &MainWindow::onCopyLoraTagClicked);
    connect(ui->btnScanLocal, &QPushButton::clicked, this, &MainWindow::onScanLocalClicked);
    connect(ui->btnForceUpdate, &QPushButton::clicked, this, &MainWindow::onForceUpdateClicked);
    connect(ui->btnCheckModelUpdate, &QPushButton::clicked, this, [this]() {
        QList<QListWidgetItem*> items;
        if (QListWidgetItem *item = ui->modelList->currentItem(); isModelListItem(item)) items << item;
        checkUpdatesForItems(items, false, true);
    });
    connect(ui->btnLocalMetaSave, &QPushButton::clicked, this, &MainWindow::onLocalMetaSaveClicked);
    connect(ui->btnLocalMetaReset, &QPushButton::clicked, this, &MainWindow::onLocalMetaResetClicked);
    connect(ui->btnEditMeta, &QPushButton::clicked, this, &MainWindow::onEditMetaTabClicked);
    connect(ui->btnShowDescriptionDetail, &QPushButton::clicked, this, &MainWindow::showModelDescriptionDialog);
    connect(ui->btnEditUserNote, &QPushButton::clicked, this, [this]() {
        if (QListWidgetItem *item = ui->modelList->currentItem(); isModelListItem(item)) {
            openModelNoteDialog(item);
        }
    });
    connect(ui->listEditImages, &QListWidget::currentRowChanged, this, &MainWindow::onEditImageSelectionChanged);
    connect(ui->btnEditAddImage, &QPushButton::clicked, this, &MainWindow::onEditAddImageClicked);
    connect(ui->btnEditReplaceImage, &QPushButton::clicked, this, &MainWindow::onEditReplaceImageClicked);
    connect(ui->btnEditRemoveImage, &QPushButton::clicked, this, &MainWindow::onEditRemoveImageClicked);
    connect(ui->btnEditSetCover, &QPushButton::clicked, this, &MainWindow::onEditSetCoverClicked);
    initDownloadsPage();
    connect(ui->searchEdit, &QLineEdit::textChanged, this, &MainWindow::onSearchTextChanged);
    // 主页与画廊按钮
    connect(ui->btnHome, &QPushButton::clicked, this, &MainWindow::onHomeButtonClicked);
    connect(ui->homeGalleryList, &QListWidget::itemClicked, this, &MainWindow::onHomeGalleryClicked);
    connect(ui->btnAddCollection, &QPushButton::clicked, this, &MainWindow::onCreateCollection);
    connect(ui->btnGallery, &QPushButton::clicked, this, &MainWindow::onGalleryButtonClicked);
    connect(ui->editHomeAuthorFilter, &QLineEdit::textChanged, this, [this](const QString &text) {
        currentHomeAuthorFilter = text.trimmed();
        refreshHomeFilterChips();
        refreshHomeGallery();
    });
    connect(ui->editHomeTagFilter, &QLineEdit::returnPressed, this, [this]() {
        addHomeTagFilter(ui->editHomeTagFilter->text());
    });
    connect(ui->btnHomeAddTagFilter, &QPushButton::clicked, this, [this]() {
        addHomeTagFilter(ui->editHomeTagFilter->text());
    });
    connect(ui->btnHomeTagSortMode, &QPushButton::clicked, this, [this]() {
        homeFilterTagsSortByCount = !homeFilterTagsSortByCount;
        refreshHomeFilterChips();
    });
    connect(ui->btnHomeClearFilters, &QPushButton::clicked, this, &MainWindow::clearHomeFilters);
    connect(ui->btnHomeFilterToggle, &QPushButton::clicked, this, [this](bool checked) {
        homeFilterExpanded = checked;
        refreshHomeFilterChips();
    });

    ui->listEditImages->setUniformItemSizes(true);
    ui->listEditImages->setGridSize(QSize(118, 186));
    ui->listEditImages->setWordWrap(false);

    // === 用户图库页面初始化 ===
    // 1. 初始化 Tag 流式控件，放入 XML 定义好的 scrollAreaTags 中
    tagFlowWidget = new TagFlowWidget();
    tagFlowWidget->setTranslationMap(&translationMap);
    tagFlowWidget->setObjectName("tagFlowContainer");
    tagFlowWidget->setAttribute(Qt::WA_TranslucentBackground);
    ui->scrollAreaTags->viewport()->setAutoFillBackground(false);
    ui->scrollAreaTags->setWidget(tagFlowWidget);
    ui->scrollAreaTags->viewport()->setAutoFillBackground(false);
    connect(ui->scrollAreaTags->verticalScrollBar(), &QScrollBar::valueChanged,
            ui->scrollAreaTags->viewport(), [this](){
                ui->scrollAreaTags->viewport()->update();});
    // 设置右键菜单策略
    ui->listUserImages->setContextMenuPolicy(Qt::CustomContextMenu);
    // 连接右键信号
    connect(ui->listUserImages, &QListWidget::customContextMenuRequested,
            this, &MainWindow::onUserGalleryContextMenu);
    // 2. 双击列表项查看大图 ===
    connect(ui->listUserImages, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item){
        if (!item) return;
        QString path = item->data(ROLE_USER_IMAGE_PATH).toString(); // 取出全路径
        if (!path.isEmpty()) {
            showFullImageDialog(path); // 调用已有的显示大图函数
        }
    });
    // 3. 信号连接
    // 切换 Tab 按钮
    connect(ui->btnShowUserGallery, &QPushButton::clicked, this, &MainWindow::onToggleDetailTab);
    // SD 目录与扫描
    connect(ui->btnSetSdFolder, &QPushButton::clicked, this, &MainWindow::onSetSdFolderClicked);
    connect(ui->btnRescanUser, &QPushButton::clicked, this, &MainWindow::onRescanUserClicked);
    connect(ui->btnTranslate, &QPushButton::toggled, this, [this](bool checked){
        if (checked) {
            // 用户想开启翻译，检查是否有数据
            if (translationMap.isEmpty()) {
                // 1. 临时阻断信号，把勾选状态取消掉（因为开启失败）
                ui->btnTranslate->blockSignals(true);
                ui->btnTranslate->setChecked(false);
                ui->btnTranslate->blockSignals(false);

                // 2. 弹窗提示
                QMessageBox::StandardButton reply;
                reply = QMessageBox::question(this, "未加载翻译",
                                              "尚未加载翻译词表 (CSV)。\n是否现在前往设置页面进行设置？\n\n(格式: 英文,中文)",
                                              QMessageBox::Yes|QMessageBox::No);

                if (reply == QMessageBox::Yes) {
                    ui->rootStack->setCurrentWidget(ui->pageSettings); // 跳转到设置页
                    if (settingsPage) settingsPage->focusTranslationPath();
                }
                return;
            }
        }
        // 如果有数据（或者用户关闭翻译），通知控件切换模式
        tagFlowWidget->setShowTranslation(checked);
    });
    connect(ui->btnClearUserTagSelection, &QPushButton::clicked, this, [this]() {
        tagFlowWidget->clearSelectedTags();
    });
    connect(ui->comboUserTagSortMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index){
        tagFlowWidget->setSortMode(index == 1 ? TagFlowWidget::SortAlphabetically : TagFlowWidget::SortByCount);
    });
    connect(ui->editUserTagSearch, &QLineEdit::textChanged, this, [this](const QString &text){
        tagFlowWidget->setSearchText(text);
    });
    connect(ui->chkUserTagLoraOnly, &QCheckBox::toggled, this, [this](bool checked){
        tagFlowWidget->setLoraOnly(checked);
    });
    connect(ui->chkUserTagCurrentImageOnly, &QCheckBox::toggled, this, [this](bool) {
        refreshUserTagFlowStats();
    });
    connect(ui->chkUserTagIncludeNegative, &QCheckBox::toggled, this, [this](bool) {
        refreshUserTagFlowStats();
    });
    // 图片点击
    connect(ui->listUserImages, &QListWidget::itemClicked, this, &MainWindow::onUserImageClicked);
    // Tag 筛选
    connect(tagFlowWidget, &TagFlowWidget::filterChanged, this, &MainWindow::onTagFilterChanged);
    // 2. 右键点击 -> 弹出菜单
    connect(ui->btnFavorite, &QPushButton::customContextMenuRequested, this, [this](const QPoint &pos){
        // 获取当前选中的所有模型
        QList<QListWidgetItem*> selectedItems = ui->modelList->selectedItems();

        // 如果没有多选，但有当前焦点的单选项，也把它加进去
        if (selectedItems.isEmpty() && ui->modelList->currentItem()) {
            selectedItems.append(ui->modelList->currentItem());
        }

        // 只有非空时才弹出
        if (!selectedItems.isEmpty()) {
            // 在按钮下方弹出菜单
            showCollectionMenu(selectedItems, ui->btnFavorite->mapToGlobal(pos));
        }
    });
    connect(ui->btnFavorite, &QPushButton::clicked, this, &MainWindow::onBtnFavoriteClicked);

    // 设置 Splitter
    ui->splitter->setSizes(QList<int>() << 260 << 1000);
    ui->splitterUser->setSizes(QList<int>() << 480 << 435);

    // 默认显示主页 (Page 0)
    ui->rootStack->setCurrentIndex(0);          // 库页面
    ui->mainStack->setCurrentIndex(0);          // 库页面中的主页 (大图网格)
    ui->sidebarStack->setCurrentIndex(1);       // 侧边栏默认显示收藏夹树
    ui->btnCollectionsTab->setChecked(true);    // 确保收藏夹按钮选中

    bgResizeTimer = new QTimer(this);
    bgResizeTimer->setSingleShot(true);
    // 当定时器时间到，执行更新背景函数
    connect(bgResizeTimer, &QTimer::timeout, this, &MainWindow::updateBackgroundImage);

    detailGalleryBuildTimer = new QTimer(this);
    detailGalleryBuildTimer->setSingleShot(true);
    connect(detailGalleryBuildTimer, &QTimer::timeout, this, &MainWindow::buildGalleryBatch);

    if (ui->backgroundLabel && ui->scrollAreaWidgetContents) {
        ui->scrollAreaWidgetContents->installEventFilter(this);
        ui->backgroundLabel->setScaledContents(false);
        ui->backgroundLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        updateBackgroundImage();
    }

    clearDetailView();
    refreshHomeFilterChips();

    QTimer::singleShot(0, this, [this]() {
        reloadTranslationMaps();
    });

    QTimer::singleShot(300, this, [this](){
        ui->statusbar->showMessage("正在扫描本地模型库...");
        loadCollections();
        loadModelHighlightColors();
        loadModelUserNotes();
        loadUserGalleryCache();
        const QStringList activeLoraPaths = collectEnabledPaths(loraPaths, disabledLoraPaths);
        if (!activeLoraPaths.isEmpty()) scanModels(activeLoraPaths);
        ui->comboSort->setCurrentIndex(0);
        executeSort();
        refreshCollectionTreeView();
        int modelCount = 0;
        for (int i = 0; i < ui->modelList->count(); ++i) {
            if (isModelListItem(ui->modelList->item(i))) modelCount++;
        }
        ui->statusbar->showMessage(QString("加载完成，共 %1 个模型").arg(modelCount), 3000);
        if (optAutoCheckUpdatesOnStartup) {
            QTimer::singleShot(800, this, [this]() {
                startAppUpdateCheck(true);
            });
        }
    });

}

QString forceWrap(const QString &text) { // 强制加入零宽空格换行
    QString result;
    for (int i = 0; i < text.length(); ++i) {
        result += text[i];
        result += QChar(0x200B); // 插入零宽空格
    }
    return result;
}

static constexpr int USER_GALLERY_PARSER_VERSION = 4;

static QStringList parsePromptsToTagsWorker(const QString &rawPrompt, bool splitOnNewline, const QStringList &filterTags)
{
    QStringList result;
    const QString trimmedPrompt = rawPrompt.trimmed();
    if (trimmedPrompt.isEmpty()) return result;
    if (trimmedPrompt.startsWith('{') || trimmedPrompt.startsWith('[')) {
        const QJsonDocument doc = QJsonDocument::fromJson(trimmedPrompt.toUtf8());
        if (!doc.isNull()) return result;
    }

    QString processText = trimmedPrompt;
    if (splitOnNewline) {
        processText.replace("\r\n", ",");
        processText.replace("\n", ",");
        processText.replace("\r", ",");
    }

    const QStringList parts = processText.split(",", Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QString clean = TagUtils::cleanPromptTag(part);
        if (clean.isEmpty()) continue;

        bool isBlocked = false;
        for (const QString &filterWord : filterTags) {
            if (clean.compare(filterWord, Qt::CaseInsensitive) == 0) {
                isBlocked = true;
                break;
            }
        }
        if (!isBlocked) result.append(clean);
    }
    return result;
}

static QString getSafetensorsInternalNameWorker(const QString &path)
{
    if (!path.endsWith(".safetensors", Qt::CaseInsensitive)) {
        return QString();
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QString();

    qint64 headerLen = 0;
    if (file.read(reinterpret_cast<char*>(&headerLen), 8) != 8) return QString();
    if (headerLen <= 0 || headerLen > 100 * 1024 * 1024) return QString();

    const QByteArray headerData = file.read(headerLen);
    const QJsonDocument doc = QJsonDocument::fromJson(headerData);
    if (!doc.isObject()) return QString();

    const QJsonObject root = doc.object();
    const QJsonObject meta = root.value("__metadata__").toObject();
    return meta.value("ss_output_name").toString().trimmed();
}

static QString normalizeLoraNameForMatch(QString name)
{
    name = name.trimmed();
    if (name.isEmpty()) return QString();
    if (name.endsWith(".safetensors", Qt::CaseInsensitive) ||
        name.endsWith(".ckpt", Qt::CaseInsensitive) ||
        name.endsWith(".pt", Qt::CaseInsensitive)) {
        name = QFileInfo(name).completeBaseName();
    }

    static QRegularExpression bracketSuffix("\\s*\\[[^\\]]+\\]\\s*$");
    name.remove(bracketSuffix);
    name.replace(QRegularExpression("\\s+"), "_");
    name.replace(QRegularExpression("_+"), "_");
    return name.trimmed().toCaseFolded();
}

static QString normalizeModelNameForMatch(const QString &name)
{
    return normalizeLoraNameForMatch(name);
}

static bool isCheckpointModelType(const QString &type, const QString &filePath)
{
    if (type.contains("checkpoint", Qt::CaseInsensitive)) return true;
    return filePath.endsWith(".ckpt", Qt::CaseInsensitive);
}

static QStringList splitCivitaiFullNameForMatch(const QString &name)
{
    QStringList out;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return out;
    out << trimmed;

    static QRegularExpression versionSuffix("\\s*\\[[^\\]]+\\]\\s*$");
    QString withoutVersion = trimmed;
    withoutVersion.remove(versionSuffix);
    withoutVersion = withoutVersion.trimmed();
    if (!withoutVersion.isEmpty() && withoutVersion != trimmed) out << withoutVersion;
    return out;
}

static QStringList extractLoraNamesFromPromptWorker(const QString &prompt)
{
    QStringList names;
    static QRegularExpression loraRegex("<\\s*(?:lora|lyco)\\s*:\\s*([^:>]+)",
                                        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = loraRegex.globalMatch(prompt);
    while (it.hasNext()) {
        const QString name = it.next().captured(1).trimmed();
        if (!name.isEmpty()) names.append(name);
    }
    return names;
}

static QStringList extractLoraNamesFromMetadataWorker(const QString &metadata)
{
    QStringList names;
    if (metadata.isEmpty()) return names;

    static QRegularExpression addNetModelRegex("(?:^|[,\\n\\r])\\s*AddNet\\s+Model\\s+\\d+\\s*:\\s*([^,\\n\\r]+)",
                                               QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator addNetIt = addNetModelRegex.globalMatch(metadata);
    while (addNetIt.hasNext()) {
        const QString name = addNetIt.next().captured(1).trimmed();
        if (!name.isEmpty()) names.append(name);
    }

    static QRegularExpression loraHashesBlockRegex("(?:^|[,\\n\\r])\\s*Lora\\s+hashes\\s*:\\s*(\"[^\"]*\"|[^\\n\\r]*)",
                                                   QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator blockIt = loraHashesBlockRegex.globalMatch(metadata);
    while (blockIt.hasNext()) {
        QString block = blockIt.next().captured(1).trimmed();
        if (block.startsWith('"') && block.endsWith('"') && block.size() >= 2) {
            block = block.mid(1, block.size() - 2);
        }

        static QRegularExpression loraHashNameRegex("([^:,]+?)\\s*:");
        QRegularExpressionMatchIterator nameIt = loraHashNameRegex.globalMatch(block);
        while (nameIt.hasNext()) {
            const QString name = nameIt.next().captured(1).trimmed();
            if (!name.isEmpty()) names.append(name);
        }
    }

    static QRegularExpression comfyLoraBlockRegex("(?:^|[,\\n\\r])\\s*ComfyUI\\s+LoRAs\\s*:\\s*([^\\n\\r]*)",
                                                  QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator comfyIt = comfyLoraBlockRegex.globalMatch(metadata);
    while (comfyIt.hasNext()) {
        const QStringList entries = comfyIt.next().captured(1).split(',', Qt::SkipEmptyParts);
        for (QString entry : entries) {
            entry = entry.trimmed();
            const int colon = entry.indexOf(':');
            if (colon > 0) entry = entry.left(colon).trimmed();
            if (!entry.isEmpty()) names.append(entry);
        }
    }

    return names;
}

static QStringList extractCheckpointNamesFromParametersWorker(const QString &parameters);

static bool promptUsesLoraWorker(const QString &prompt, const QString &parameters, const QSet<QString> &normalizedLoraNames)
{
    if (normalizedLoraNames.isEmpty()) return false;

    QStringList usedLoras = extractLoraNamesFromPromptWorker(prompt);
    usedLoras.append(extractLoraNamesFromMetadataWorker(parameters));
    for (const QString &usedLora : usedLoras) {
        if (normalizedLoraNames.contains(normalizeLoraNameForMatch(usedLora))) {
            return true;
        }
    }
    return false;
}

static bool parametersUseCheckpointWorker(const QString &parameters, const QSet<QString> &normalizedCheckpointNames)
{
    if (normalizedCheckpointNames.isEmpty()) return false;

    const QStringList usedCheckpoints = extractCheckpointNamesFromParametersWorker(parameters);
    for (const QString &usedCheckpoint : usedCheckpoints) {
        if (normalizedCheckpointNames.contains(normalizeModelNameForMatch(usedCheckpoint))) {
            return true;
        }
    }
    return false;
}

static QString normalizeSummaryHashForMatch(QString hash)
{
    hash = hash.trimmed();
    if (hash.isEmpty()) return QString();
    hash.remove(QRegularExpression("[^A-Fa-f0-9]"));
    return hash.toLower();
}

static void collectHashesFromStringWorker(const QString &text, QSet<QString> &out)
{
    if (text.isEmpty()) return;
    static QRegularExpression hexRegex("([A-Fa-f0-9]{8,128})");
    QRegularExpressionMatchIterator it = hexRegex.globalMatch(text);
    while (it.hasNext()) {
        const QString normalized = normalizeSummaryHashForMatch(it.next().captured(1));
        if (!normalized.isEmpty()) out.insert(normalized);
    }
}

static bool looksLikeHashFieldWorker(const QString &key)
{
    const QString folded = key.toCaseFolded();
    return folded.contains("hash")
           || folded == "autov2"
           || folded == "autov3"
           || folded == "sha256"
           || folded == "sha1"
           || folded == "md5";
}

static void collectHashesFromJsonValueWorker(const QJsonValue &value, const QString &keyHint, QSet<QString> &out, bool inHashContext = false)
{
    if (value.isString()) {
        if (inHashContext || looksLikeHashFieldWorker(keyHint)) {
            collectHashesFromStringWorker(value.toString(), out);
        }
        return;
    }

    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        const bool nextHashContext = inHashContext || looksLikeHashFieldWorker(keyHint);
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            collectHashesFromJsonValueWorker(it.value(), it.key(), out, nextHashContext);
        }
        return;
    }

    if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        for (const QJsonValue &entry : arr) {
            collectHashesFromJsonValueWorker(entry, keyHint, out, inHashContext || looksLikeHashFieldWorker(keyHint));
        }
    }
}

static QSet<QString> collectLoraSummaryHashesFromJsonFileWorker(const QString &path)
{
    QSet<QString> out;
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) return out;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isObject()) {
        collectHashesFromJsonValueWorker(doc.object(), QString(), out);
    }
    return out;
}

static QSet<QString> collectCheckpointHashesFromJsonFileWorker(const QString &path)
{
    QSet<QString> out;
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) return out;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return out;

    const QJsonObject root = doc.object();
    const QJsonArray files = root.value("files").toArray();
    for (const QJsonValue &value : files) {
        const QJsonObject fileObj = value.toObject();
        const QJsonObject hashes = fileObj.value("hashes").toObject();
        const QStringList keys = {"SHA256", "AutoV3", "AutoV2", "BLAKE3"};
        for (const QString &key : keys) {
            const QString normalized = normalizeSummaryHashForMatch(hashes.value(key).toString());
            if (!normalized.isEmpty()) out.insert(normalized);
        }
    }

    return out;
}

static QSet<QString> extractLoraHashValuesFromParametersWorker(const QString &parameters)
{
    QSet<QString> hashes;
    if (parameters.isEmpty()) return hashes;

    static QRegularExpression loraHashesBlockRegex("(?:^|[,\\n\\r])\\s*Lora\\s+hashes\\s*:\\s*(\"[^\"]*\"|[^\\n\\r]*)",
                                                   QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator blockIt = loraHashesBlockRegex.globalMatch(parameters);
    while (blockIt.hasNext()) {
        QString block = blockIt.next().captured(1).trimmed();
        if (block.startsWith('"') && block.endsWith('"') && block.size() >= 2) {
            block = block.mid(1, block.size() - 2);
        }
        static QRegularExpression hashInBlockRegex(":\\s*([A-Fa-f0-9]{6,128})");
        QRegularExpressionMatchIterator hashIt = hashInBlockRegex.globalMatch(block);
        while (hashIt.hasNext()) {
            const QString normalized = normalizeSummaryHashForMatch(hashIt.next().captured(1));
            if (!normalized.isEmpty()) hashes.insert(normalized);
        }
    }

    static QRegularExpression comfyHashesBlockRegex("(?:^|[,\\n\\r])\\s*ComfyUI\\s+Lora\\s+hashes\\s*:\\s*([^\\n\\r]*)",
                                                    QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator comfyIt = comfyHashesBlockRegex.globalMatch(parameters);
    while (comfyIt.hasNext()) {
        const QString block = comfyIt.next().captured(1);
        static QRegularExpression hashInComfyRegex(":\\s*([A-Fa-f0-9]{6,128})");
        QRegularExpressionMatchIterator hashIt = hashInComfyRegex.globalMatch(block);
        while (hashIt.hasNext()) {
            const QString normalized = normalizeSummaryHashForMatch(hashIt.next().captured(1));
            if (!normalized.isEmpty()) hashes.insert(normalized);
        }
    }

    static QRegularExpression addNetHashRegex("(?:^|[,\\n\\r])\\s*AddNet\\s+Model\\s+hash\\s+\\d+\\s*:\\s*([A-Fa-f0-9]{6,128})",
                                              QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator addNetIt = addNetHashRegex.globalMatch(parameters);
    while (addNetIt.hasNext()) {
        const QString normalized = normalizeSummaryHashForMatch(addNetIt.next().captured(1));
        if (!normalized.isEmpty()) hashes.insert(normalized);
    }

    return hashes;
}

static QStringList extractCheckpointNamesFromParametersWorker(const QString &parameters)
{
    QStringList names;
    if (parameters.isEmpty()) return names;

    static QRegularExpression modelRegex("(?:^|[,\\n\\r])\\s*Model\\s*:\\s*([^,\\n\\r]+)",
                                         QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator modelIt = modelRegex.globalMatch(parameters);
    while (modelIt.hasNext()) {
        const QString name = modelIt.next().captured(1).trimmed();
        if (!name.isEmpty()) names.append(name);
    }

    static QRegularExpression checkpointRegex("(?:^|[,\\n\\r])\\s*Checkpoint\\s*:\\s*([^,\\n\\r]+)",
                                              QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator checkpointIt = checkpointRegex.globalMatch(parameters);
    while (checkpointIt.hasNext()) {
        const QString name = checkpointIt.next().captured(1).trimmed();
        if (!name.isEmpty()) names.append(name);
    }

    return names;
}

static QSet<QString> extractCheckpointHashValuesFromParametersWorker(const QString &parameters)
{
    QSet<QString> hashes;
    if (parameters.isEmpty()) return hashes;

    static QRegularExpression modelHashRegex("(?:^|[,\\n\\r])\\s*Model\\s+hash\\s*:\\s*([A-Fa-f0-9]{6,128})",
                                             QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = modelHashRegex.globalMatch(parameters);
    while (it.hasNext()) {
        const QString normalized = normalizeSummaryHashForMatch(it.next().captured(1));
        if (!normalized.isEmpty()) hashes.insert(normalized);
    }
    return hashes;
}

static bool hashSetsMatchByPrefixWorker(const QSet<QString> &imageHashes, const QSet<QString> &targetHashes)
{
    if (imageHashes.isEmpty() || targetHashes.isEmpty()) return false;

    for (const QString &imgHash : imageHashes) {
        if (imgHash.size() < 6) continue;
        for (const QString &targetHash : targetHashes) {
            if (targetHash.size() < 6) continue;
            if (imgHash == targetHash || imgHash.startsWith(targetHash) || targetHash.startsWith(imgHash)) {
                return true;
            }
        }
    }
    return false;
}

struct CachedImageUsageInfo {
    QSet<QString> usedLoraNames;
    QSet<QString> loraHashes;
    QSet<QString> usedCheckpointNames;
    QSet<QString> checkpointHashes;
    qint64 lastModified = 0;
};

struct ModelUsageInput {
    QString filePath;
    QString baseName;
    QString type;
    QString civitaiName;
    QString sha256;
};

struct ModelUsageCandidate {
    QString filePath;
    QString baseName;
    bool isCheckpoint = false;
    QSet<QString> normalizedLoraNames;
    QSet<QString> summaryHashes;
    QSet<QString> normalizedCheckpointNames;
    QSet<QString> checkpointHashes;
};

struct ModelUsageStatResult {
    QString filePath;
    int usageCount = 0;
    qint64 lastUsed = 0;
};

static void addLoraNameVariantsWorker(const QString &name, QSet<QString> &out)
{
    QString coreName = name.trimmed();
    if (coreName.isEmpty()) return;
    if (coreName.contains("[")) coreName = coreName.split("[").first().trimmed();
    coreName = QFileInfo(coreName).completeBaseName();
    if (coreName.isEmpty()) return;

    QStringList variants;
    variants << coreName;
    QString spaceToUnder = coreName;
    spaceToUnder.replace(" ", "_");
    variants << spaceToUnder;
    QString underToSpace = coreName;
    underToSpace.replace("_", " ");
    variants << underToSpace;
    QString noSpace = coreName;
    noSpace.remove(" ");
    variants << noSpace;
    QString noUnder = coreName;
    noUnder.remove("_");
    variants << noUnder;
    QString pure = coreName;
    pure.remove(" ").remove("_");
    variants << pure;

    for (const QString &variant : variants) {
        if (variant.length() < 2) continue;
        const QString normalized = normalizeLoraNameForMatch(variant);
        if (!normalized.isEmpty()) out.insert(normalized);
    }
}

static void addModelNameVariantsWorker(const QString &name, QSet<QString> &out)
{
    addLoraNameVariantsWorker(name, out);
}

static ModelUsageCandidate buildModelUsageCandidateWorker(const ModelUsageInput &model)
{
    ModelUsageCandidate candidate;
    candidate.filePath = model.filePath;
    candidate.baseName = model.baseName;
    candidate.isCheckpoint = isCheckpointModelType(model.type, model.filePath);

    const QString internalName = getSafetensorsInternalNameWorker(model.filePath);
    if (!internalName.isEmpty()) {
        addLoraNameVariantsWorker(internalName, candidate.normalizedLoraNames);
    }
    if (candidate.normalizedLoraNames.isEmpty()) {
        addLoraNameVariantsWorker(model.baseName, candidate.normalizedLoraNames);
    }

    addModelNameVariantsWorker(model.baseName, candidate.normalizedCheckpointNames);
    for (const QString &name : splitCivitaiFullNameForMatch(model.civitaiName)) {
        addModelNameVariantsWorker(name, candidate.normalizedCheckpointNames);
    }
    if (!internalName.isEmpty()) {
        addModelNameVariantsWorker(internalName, candidate.normalizedCheckpointNames);
    }

    const QFileInfo fi(model.filePath);
    const QString modelDir = fi.absolutePath();
    const QString modelBaseName = model.baseName.isEmpty() ? fi.completeBaseName() : model.baseName;
    QStringList hashJsonPaths;
    if (!modelDir.isEmpty() && !modelBaseName.isEmpty()) {
        hashJsonPaths.append(QDir(modelDir).filePath(modelBaseName + ".json"));
        hashJsonPaths.append(QDir(modelDir).filePath(modelBaseName + ".metadata.json"));
    }
    if (!model.filePath.isEmpty()) {
        hashJsonPaths.append(model.filePath + ".metadata.json");
    }

    for (const QString &path : hashJsonPaths) {
        const QSet<QString> hashes = collectLoraSummaryHashesFromJsonFileWorker(path);
        for (const QString &hash : hashes) candidate.summaryHashes.insert(hash);
        const QSet<QString> checkpointHashes = collectCheckpointHashesFromJsonFileWorker(path);
        for (const QString &hash : checkpointHashes) candidate.checkpointHashes.insert(hash);
    }
    if (!model.sha256.trimmed().isEmpty()) {
        const QString normalized = normalizeSummaryHashForMatch(model.sha256);
        if (!normalized.isEmpty()) candidate.checkpointHashes.insert(normalized);
    }

    return candidate;
}

static QList<ModelUsageStatResult> calculateModelUsageStatsWorker(
    const QList<ModelUsageInput> &models,
    const QMap<QString, UserImageInfo> &imageCache,
    int matchMode)
{
    QList<CachedImageUsageInfo> imageInfos;
    imageInfos.reserve(imageCache.size());

    for (auto it = imageCache.constBegin(); it != imageCache.constEnd(); ++it) {
        const UserImageInfo &info = it.value();
        CachedImageUsageInfo cached;
        QStringList usedNames = extractLoraNamesFromPromptWorker(info.prompt);
        usedNames.append(extractLoraNamesFromMetadataWorker(info.parameters));
        for (const QString &usedName : usedNames) {
            const QString normalized = normalizeLoraNameForMatch(usedName);
            if (!normalized.isEmpty()) cached.usedLoraNames.insert(normalized);
        }
        cached.loraHashes = extractLoraHashValuesFromParametersWorker(info.parameters);
        const QStringList checkpointNames = extractCheckpointNamesFromParametersWorker(info.parameters);
        for (const QString &checkpointName : checkpointNames) {
            const QString normalized = normalizeModelNameForMatch(checkpointName);
            if (!normalized.isEmpty()) cached.usedCheckpointNames.insert(normalized);
        }
        cached.checkpointHashes = extractCheckpointHashValuesFromParametersWorker(info.parameters);
        cached.lastModified = info.lastModified;

        if (!cached.usedLoraNames.isEmpty() || !cached.loraHashes.isEmpty()
            || !cached.usedCheckpointNames.isEmpty() || !cached.checkpointHashes.isEmpty()) {
            imageInfos.append(cached);
        }
    }

    QList<ModelUsageStatResult> results;
    results.reserve(models.size());

    for (const ModelUsageInput &model : models) {
        const ModelUsageCandidate candidate = buildModelUsageCandidateWorker(model);
        const bool strictSummary = (matchMode == 2);
        bool useSummary = (matchMode == 1 || matchMode == 2);
        const QSet<QString> targetHashes = candidate.isCheckpoint ? candidate.checkpointHashes : candidate.summaryHashes;
        if (useSummary && targetHashes.isEmpty()) {
            if (strictSummary) {
                ModelUsageStatResult empty;
                empty.filePath = candidate.filePath;
                results.append(empty);
                continue;
            }
            useSummary = false;
        }

        ModelUsageStatResult stat;
        stat.filePath = candidate.filePath;

        for (const CachedImageUsageInfo &image : imageInfos) {
            bool matched = false;
            if (useSummary) {
                matched = candidate.isCheckpoint
                              ? hashSetsMatchByPrefixWorker(image.checkpointHashes, targetHashes)
                              : hashSetsMatchByPrefixWorker(image.loraHashes, targetHashes);
            } else {
                const QSet<QString> &targetNames = candidate.isCheckpoint
                                                       ? candidate.normalizedCheckpointNames
                                                       : candidate.normalizedLoraNames;
                const QSet<QString> &imageNames = candidate.isCheckpoint
                                                      ? image.usedCheckpointNames
                                                      : image.usedLoraNames;
                for (const QString &name : targetNames) {
                    if (imageNames.contains(name)) {
                        matched = true;
                        break;
                    }
                }
            }

            if (matched) {
                ++stat.usageCount;
                stat.lastUsed = qMax(stat.lastUsed, image.lastModified);
            }
        }

        results.append(stat);
    }

    return results;
}

static void parsePngInfoWorker(const QString &path, UserImageInfo &info, bool splitOnNewline, const QStringList &filterTags)
{
    info.parserVersion = USER_GALLERY_PARSER_VERSION;
    const ParsedImageMetadata parsed = parseImageMetadataFromFile(path);
    if (!parsed.hasContent()) return;

    info.prompt = parsed.positivePrompt.trimmed();
    info.negativePrompt = parsed.negativePrompt.trimmed();
    if (info.negativePrompt.isEmpty() && !info.prompt.isEmpty()) {
        info.negativePrompt = "(empty)";
    }
    info.parameters = parsed.parametersText.trimmed();
    info.cleanTags = parsePromptsToTagsWorker(info.prompt, splitOnNewline, filterTags);
    info.negativeCleanTags = parsePromptsToTagsWorker(info.negativePrompt, splitOnNewline, filterTags);
}

MainWindow::~MainWindow()
{
    isShuttingDown = true;
    if (userImageThumbLoadTimer) userImageThumbLoadTimer->stop();
    if (bgResizeTimer) bgResizeTimer->stop();
    if (detailGalleryBuildTimer) detailGalleryBuildTimer->stop();
    if (transitionAnim) transitionAnim->stop();
    if (netManager) {
        const QList<QNetworkReply*> replies = netManager->findChildren<QNetworkReply*>();
        for (QNetworkReply *reply : replies) {
            if (!reply) continue;
            reply->disconnect(this);
            reply->abort();
        }
    }
    downloadQueue.clear();
    if (downloadManager) downloadManager->shutdown();
    if (hashWatcher && hashWatcher->isRunning()) hashWatcher->waitForFinished();
    if (metadataScanWatcher && metadataScanWatcher->isRunning()) metadataScanWatcher->waitForFinished();
    if (metadataHealthWatcher && metadataHealthWatcher->isRunning()) metadataHealthWatcher->waitForFinished();
    if (imageLoadWatcher && imageLoadWatcher->isRunning()) imageLoadWatcher->waitForFinished();
    saveGlobalConfig();
    cancelPendingTasks();
    if (backgroundThreadPool) backgroundThreadPool->clear();
    threadPool->waitForDone();
    backgroundThreadPool->waitForDone();
    QCoreApplication::removePostedEvents(this);
    delete ui;
}

// ---------------------------------------------------------
// 主页与收藏夹逻辑
// ---------------------------------------------------------
void MainWindow::onCollectionFilterClicked(const QString &collectionName)
{
    currentCollectionFilter = collectionName;
    refreshHomeGallery();
    refreshHomeCollectionsUI();
}

void MainWindow::onHomeButtonClicked()
{
    cancelPendingTasks();
    ui->mainStack->setCurrentIndex(0); // 切换到主页
    ui->modelList->clearSelection();   // 清除侧边栏选中
    ui->collectionTree->clearSelection();
    currentCollectionFilter = "";      // 重置过滤，显示全部
    refreshHomeFilterChips();
    refreshHomeGallery();
    refreshHomeCollectionsUI();
}

void MainWindow::loadCollections()
{
    collections.clear();
    QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);

    QFile file(configDir + "/collections.json");
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        QJsonObject root = doc.object();
        for (auto it = root.begin(); it != root.end(); ++it) {
            QString name = it.key();
            QStringList files;
            for (auto v : it.value().toArray()) files << v.toString();
            collections.insert(name, files);
        }
    }
    refreshHomeCollectionsUI();
}

void MainWindow::saveCollections()
{
    QJsonObject root;
    for (auto it = collections.begin(); it != collections.end(); ++it) {
        QJsonArray arr;
        for (const QString &f : it.value()) arr.append(f);
        root.insert(it.key(), arr);
    }

    QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);
    QFile file(configDir + "/collections.json");
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson());
    }
    refreshHomeCollectionsUI();
}

void MainWindow::loadModelHighlightColors()
{
    modelHighlightColors.clear();

    const QString configPath = qApp->applicationDirPath() + "/config/model_colors.json";
    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly)) return;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return;

    const QJsonObject root = doc.object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const QString filePath = QFileInfo(it.key()).absoluteFilePath();
        const QColor color(it.value().toString());
        if (!filePath.isEmpty() && color.isValid()) {
            modelHighlightColors.insert(filePath, color);
        }
    }
}

void MainWindow::saveModelHighlightColors()
{
    const QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);

    QJsonObject root;
    for (auto it = modelHighlightColors.begin(); it != modelHighlightColors.end(); ++it) {
        if (it.value().isValid()) {
            root.insert(it.key(), it.value().name(QColor::HexArgb));
        }
    }

    QFile file(configDir + "/model_colors.json");
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson());
    }
}

void MainWindow::onCreateCollection()
{
    bool ok;
    QString text = QInputDialog::getText(this, "新建收藏夹", "收藏夹名称:", QLineEdit::Normal, "", &ok);
    if (ok && !text.trimmed().isEmpty()) {
        if (!collections.contains(text)) {
            collections.insert(text, QStringList());
            saveCollections();
            refreshCollectionTreeView();
        }
    }
}

void MainWindow::refreshHomeCollectionsUI()
{
    // 清除旧按钮 (保留第一个新建按钮)
    QLayout *layout = ui->scrollAreaCollections->widget()->layout();
    QLayoutItem *item;
    while (layout->count() > 1) { // 假设索引0是 "新建" 按钮
        item = layout->takeAt(1);
        if (item->widget()) delete item->widget();
        delete item;
    }

    // === 1. 修改新建按钮样式 ===
    ui->btnAddCollection->setProperty("class", "collectionBtn");

    // === 2. 添加 "全部" 按钮 ===
    QPushButton *btnAll = new QPushButton("ALL\n全部");
    btnAll->setFixedSize(90, 90);
    btnAll->setProperty("class", "collectionBtn");
    btnAll->setCheckable(true);
    btnAll->setChecked(currentCollectionFilter.isEmpty());
    btnAll->setCursor(Qt::PointingHandCursor);

    connect(btnAll, &QPushButton::clicked, this, [this](){
        onCollectionFilterClicked("");
    });
    layout->addWidget(btnAll);

    // === 未分类按钮 ===
    QPushButton *btnUncat = new QPushButton("📦\n未分类");
    btnUncat->setFixedSize(90, 90);
    btnUncat->setProperty("class", "collectionBtn");
    btnUncat->setCheckable(true);
    btnUncat->setChecked(currentCollectionFilter == FILTER_UNCATEGORIZED);
    connect(btnUncat, &QPushButton::clicked, this, [this](){
        currentCollectionFilter = FILTER_UNCATEGORIZED;
        refreshHomeGallery();
        refreshHomeCollectionsUI();
    });
    layout->addWidget(btnUncat);

    // === 3. 添加收藏夹按钮 (带右键功能) ===
    for (auto it = collections.begin(); it != collections.end(); ++it) {
        QString name = it.key();
        if (name == FILTER_UNCATEGORIZED) continue; // 健壮性屏蔽

        // 名字截断
        QString displayName = name;
        if (displayName.length() > 20) displayName = displayName.left(18) + "..";

        QPushButton *btn = new QPushButton(displayName);
        btn->setFixedSize(90, 90);
        btn->setProperty("class", "collectionBtn");
        btn->setCheckable(true);
        btn->setChecked(currentCollectionFilter == name);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setToolTip(name);

        // 左键点击：筛选
        connect(btn, &QPushButton::clicked, this, [this, name](){
            onCollectionFilterClicked(name);
        });

        // === 右键菜单逻辑 ===
        btn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(btn, &QPushButton::customContextMenuRequested, this, [this, btn, name](const QPoint &pos){
            QMenu menu;

            QAction *title = menu.addAction(QString("管理: %1").arg(name));
            title->setEnabled(false);
            menu.addSeparator();

            QAction *actRename = menu.addAction("重命名 / Rename");
            QAction *actDelete = menu.addAction("删除 / Delete");

            QAction *selected = menu.exec(btn->mapToGlobal(pos));

            if (selected == actRename) {
                bool ok;
                QString newName = QInputDialog::getText(this, "重命名收藏夹", "新名称:", QLineEdit::Normal, name, &ok);
                if (ok && !newName.trimmed().isEmpty() && newName != name) {
                    if (collections.contains(newName)) {
                        QMessageBox::warning(this, "错误", "该名称已存在！");
                        return;
                    }
                    // 执行重命名：取出旧值，插入新键，删除旧键
                    QStringList files = collections.value(name);
                    collections.insert(newName, files);
                    collections.remove(name);

                    // 如果当前正选着这个收藏夹，更新过滤名
                    if (currentCollectionFilter == name) currentCollectionFilter = newName;

                    saveCollections(); // 保存并刷新UI
                }
            }
            else if (selected == actDelete) {
                auto reply = QMessageBox::question(this, "确认删除",
                                                   QString("确定要删除收藏夹 \"%1\" 吗？\n(里面的模型不会被删除，仅删除分类)").arg(name),
                                                   QMessageBox::Yes | QMessageBox::No);
                if (reply == QMessageBox::Yes) {
                    // 1. 从数据中移除
                    collections.remove(name);

                    // 2. 如果当前正看着这个收藏夹，被删了就得回到"全部"
                    if (currentCollectionFilter == name) {
                        currentCollectionFilter = "";
                    }

                    // 3. 保存并刷新
                    saveCollections();
                    refreshHomeGallery(); // 刷新一下主页大图，因为过滤条件变了
                }
            }
        });

        layout->addWidget(btn);
    }

    ((QHBoxLayout*)layout)->addStretch();
}

void MainWindow::refreshHomeGallery()
{
    cancelPendingTasks();
    ui->homeGalleryList->clear();

    // 1. 设置图标大小 (正方形)
    int iconSize = 180;
    ui->homeGalleryList->setIconSize(QSize(iconSize, iconSize));

    // 2. 设置网格大小 (正方形)
    // 既然没有文字了，高度不需要留空，设为 200x200 足够容纳 180 的图标加一点边距
    ui->homeGalleryList->setGridSize(QSize(200, 200));

    // 3. 布局模式
    ui->homeGalleryList->setViewMode(QListWidget::IconMode);
    ui->homeGalleryList->setResizeMode(QListWidget::Adjust);
    ui->homeGalleryList->setSpacing(10);
    // 禁用拖拽，防止意外移动
    ui->homeGalleryList->setMovement(QListView::Static);

    ui->homeGalleryList->setContextMenuPolicy(Qt::CustomContextMenu);
    disconnect(ui->homeGalleryList, &QListWidget::customContextMenuRequested, this, &MainWindow::onHomeGalleryContextMenu);
    connect(ui->homeGalleryList, &QListWidget::customContextMenuRequested, this, &MainWindow::onHomeGalleryContextMenu);

    QString searchText = ui->searchEdit->text().trimmed();
    QString targetBaseModel = ui->comboBaseModel->currentText();

    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *sideItem = ui->modelList->item(i);
        if (!isModelListItem(sideItem)) continue;

        int nsfwLevel = sideItem->data(ROLE_NSFW_LEVEL).toInt();
        bool isNSFW = nsfwLevel > optNSFWLevel;
        QString modelKey = sideItem->data(ROLE_MODEL_NAME).toString();
        if (modelKey.isEmpty()) modelKey = QFileInfo(sideItem->data(ROLE_FILE_PATH).toString()).completeBaseName();
        QString displayName = sideItem->text();
        if (displayName.isEmpty()) displayName = modelKey;
        QString previewPath = sideItem->data(ROLE_PREVIEW_PATH).toString();
        QString filePath = sideItem->data(ROLE_FILE_PATH).toString();
        QString itemBaseModel = sideItem->data(ROLE_FILTER_BASE).toString();
        const QString itemCreator = sideItem->data(ROLE_MODEL_CREATOR).toString();
        const QStringList itemModelTags = sideItem->data(ROLE_MODEL_TAGS).toStringList();
        const QStringList itemUserTags = sideItem->data(ROLE_USER_TAGS).toStringList();

        // --- NSFW 拦截逻辑 ---
        if (optFilterNSFW && isNSFW && optNSFWMode == 0) {
            continue; // 完全不显示模式：直接跳过此模型
        }

        if (!searchText.isEmpty()) {
            bool matchDisplay = displayName.contains(searchText, Qt::CaseInsensitive);
            bool matchKey = modelKey.contains(searchText, Qt::CaseInsensitive);
            bool matchCreator = itemCreator.contains(searchText, Qt::CaseInsensitive);
            bool matchModelTags = itemModelTags.join(' ').contains(searchText, Qt::CaseInsensitive);
            bool matchUserTags = itemUserTags.join(' ').contains(searchText, Qt::CaseInsensitive);
            bool matchUserNote = sideItem->data(ROLE_USER_NOTE).toString().contains(searchText, Qt::CaseInsensitive);
            if (!matchDisplay && !matchKey && !matchCreator && !matchModelTags && !matchUserTags && !matchUserNote) continue;
        }

        if (targetBaseModel != "All") {
            if (itemBaseModel != targetBaseModel) continue;
        }

        if (!currentHomeAuthorFilter.isEmpty() &&
            itemCreator.compare(currentHomeAuthorFilter, Qt::CaseInsensitive) != 0) {
            continue;
        }

        if (!currentHomeTagFilters.isEmpty()) {
            QSet<QString> modelTagSet;
            for (const QString &tag : itemModelTags + itemUserTags) {
                const QString key = tag.trimmed().toCaseFolded();
                if (!key.isEmpty()) modelTagSet.insert(key);
            }
            bool allTagsMatched = true;
            for (const QString &tag : currentHomeTagFilters) {
                if (!modelTagSet.contains(tag.trimmed().toCaseFolded())) {
                    allTagsMatched = false;
                    break;
                }
            }
            if (!allTagsMatched) continue;
        }

        if (!currentCollectionFilter.isEmpty()) {
            if (currentCollectionFilter == FILTER_UNCATEGORIZED) {
                // 如果当前选的是“未分类”：
                // 检查这个 baseName 是否存在于任何一个已有的收藏夹 List 中
                bool categorized = false;
                for (auto it = collections.begin(); it != collections.end(); ++it) {
                    if (it.value().contains(modelKey)) {
                        categorized = true;
                        break;
                    }
                }
                if (categorized) continue; // 已分类的模型，不显示在“未分类”中
            } else {
                // 正常的收藏夹筛选逻辑
                QStringList list = collections.value(currentCollectionFilter);
                if (!list.contains(modelKey)) continue;
            }
        }

        QListWidgetItem *item = new QListWidgetItem();
        item->setToolTip(displayName);
        item->setData(ROLE_FILE_PATH, filePath);
        item->setData(ROLE_PREVIEW_PATH, previewPath);
        item->setData(ROLE_NSFW_LEVEL, nsfwLevel);
        item->setData(ROLE_MODEL_NAME, modelKey);
        item->setData(ROLE_CIVITAI_NAME, sideItem->data(ROLE_CIVITAI_NAME));
        item->setData(ROLE_MODEL_TYPE, sideItem->data(ROLE_MODEL_TYPE));
        item->setData(ROLE_MODEL_CREATOR, sideItem->data(ROLE_MODEL_CREATOR));
        item->setData(ROLE_MODEL_TAGS, sideItem->data(ROLE_MODEL_TAGS));
        item->setData(ROLE_USER_RATING, sideItem->data(ROLE_USER_RATING));
        item->setData(ROLE_USER_NOTE, sideItem->data(ROLE_USER_NOTE));
        item->setData(ROLE_USER_TAGS, sideItem->data(ROLE_USER_TAGS));
        item->setData(ROLE_USER_CUSTOM_TRIGGERS, sideItem->data(ROLE_USER_CUSTOM_TRIGGERS));
        item->setToolTip(formatModelUserNoteTooltip(filePath, displayName));

        item->setIcon(placeholderIcon);
        ui->homeGalleryList->addItem(item);

        if (!filePath.isEmpty()) {
            QString pathToSend = previewPath.isEmpty() ? "invalid_path" : previewPath;

            QString taskId = "HOME:" + filePath;

            // 依然使用主 threadPool (因为主页大图需要点击即停，响应优先)
            IconLoaderTask *task = new IconLoaderTask(pathToSend, iconSize, 12, this, taskId);
            task->setAutoDelete(true);
            threadPool->start(task);
        }
    }
}

void MainWindow::refreshHomeFilterChips()
{
    if (!ui || !ui->layoutHomeFilterChips || !ui->layoutHomeFilterSummaryChips) return;

    constexpr int kSummaryChipHeight = 24;
    constexpr int kSummaryAreaHeight = 36;     // chip 高度 + 横向滚动条空间
    constexpr int kSummarySpacing = 5;
    constexpr int kTagsMaxHeight = 170;
    const int previousTagScrollValue = ui->scrollHomeFilterTags
        ? ui->scrollHomeFilterTags->verticalScrollBar()->value()
        : 0;

    clearLayout(ui->layoutHomeFilterSummaryChips);
    clearLayout(ui->layoutHomeFilterChips);

    // =========================
    // 1. 第一行：已选作者 / Tag 摘要区
    // =========================
    ui->scrollHomeFilterSummary->setWidgetResizable(false);
    ui->scrollHomeFilterSummary->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ui->scrollHomeFilterSummary->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->scrollHomeFilterSummary->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    ui->scrollHomeFilterSummary->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ui->scrollHomeFilterSummary->setMinimumWidth(320);
    ui->scrollHomeFilterSummary->setFixedHeight(kSummaryAreaHeight);

    ui->homeFilterSummaryContents->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    ui->homeFilterSummaryContents->setFixedHeight(kSummaryChipHeight);

    ui->layoutHomeFilterSummaryChips->setContentsMargins(0, 0, 0, 0);
    ui->layoutHomeFilterSummaryChips->setSpacing(kSummarySpacing);
    ui->layoutHomeFilterSummaryChips->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    int summaryContentWidth = 0;

    auto appendSummaryWidth = [&](int itemWidth) {
        if (summaryContentWidth > 0) {
            summaryContentWidth += kSummarySpacing;
        }
        summaryContentWidth += itemWidth;
    };

    auto fixSummaryWidgetSize = [&](QWidget *widget) {
        if (!widget) return;

        widget->ensurePolished();
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);

        int width = 0;

        if (auto *btn = qobject_cast<QPushButton *>(widget)) {
            const QString text = btn->text();

    #if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
            width = btn->fontMetrics().horizontalAdvance(text);
    #else
            width = btn->fontMetrics().width(text);
    #endif

            // 不用 QPushButton::sizeHint，因为它会带 Qt 默认按钮最小宽度
            // 这里的 18 对应 QSS 左右 padding: 9px + 9px
            // 再加一点边框和安全余量
            width += 22;
        } else if (auto *label = qobject_cast<QLabel *>(widget)) {
            const QString text = label->text();

    #if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
            width = label->fontMetrics().horizontalAdvance(text);
    #else
            width = label->fontMetrics().width(text);
    #endif

            width += 6;
        }

        width = qMax(width, 18);

        widget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        widget->setMinimumSize(width, kSummaryChipHeight);
        widget->setMaximumSize(width, kSummaryChipHeight);
        widget->resize(width, kSummaryChipHeight);

        appendSummaryWidth(width);
    };

    const QList<HomeFilterTagInfo> availableTagInfos = collectAvailableHomeFilterTags();
    QHash<QString, HomeFilterTagInfo> availableTagInfoByKey;
    for (const HomeFilterTagInfo &info : availableTagInfos) {
        availableTagInfoByKey.insert(normalizedHomeTagKey(info.tag), info);
    }

    auto tagSourceFor = [&availableTagInfoByKey](const QString &tag) {
        const HomeFilterTagInfo info = availableTagInfoByKey.value(normalizedHomeTagKey(tag));
        return info.sourceName();
    };

    auto displayTagWithCount = [&availableTagInfoByKey](const QString &tag) {
        const HomeFilterTagInfo info = availableTagInfoByKey.value(normalizedHomeTagKey(tag));
        if (info.count > 0) return QString("%1(%2)").arg(tag).arg(info.count);
        return tag;
    };

    auto addSummaryChip = [this, &fixSummaryWidgetSize](const QString &text,
                                                        const QString &tooltip,
                                                        const QString &source,
                                                        const std::function<void()> &removeFn) {
        QPushButton *chip = new QPushButton(text);
        chip->setProperty("class", "filterChip");
        if (!source.isEmpty()) chip->setProperty("tagSource", source);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolTip(tooltip);
        chip->setCheckable(false);
        chip->setAutoDefault(false);
        chip->setDefault(false);

        fixSummaryWidgetSize(chip);

        connect(chip, &QPushButton::clicked, this, removeFn);
        ui->layoutHomeFilterSummaryChips->addWidget(chip);
    };

    QStringList selectedTags = currentHomeTagFilters.values();
    std::sort(selectedTags.begin(), selectedTags.end(), [](const QString &a, const QString &b) {
        return QString::localeAwareCompare(a, b) < 0;
    });

    if (!currentHomeAuthorFilter.isEmpty()) {
        addSummaryChip("作者: " + currentHomeAuthorFilter + " ×",
                       "点击移除作者筛选",
                       QString(),
                       [this]() {
                           setHomeAuthorFilter(QString());
                       });
    }

    for (const QString &tag : selectedTags) {
        addSummaryChip(displayTagWithCount(tag) + " ×",
                       "点击移除此 Tag 筛选",
                       tagSourceFor(tag),
                       [this, tag]() {
                           currentHomeTagFilters.remove(tag);
                           refreshHomeFilterChips();
                           refreshHomeGallery();
                       });
    }

    if (currentHomeAuthorFilter.isEmpty() && currentHomeTagFilters.isEmpty()) {
        QLabel *emptySummary = new QLabel("未启用主页作者/Tag 筛选");
        emptySummary->setStyleSheet(AppStyle::MutedBoldLabelStyle);
        emptySummary->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        fixSummaryWidgetSize(emptySummary);

        ui->layoutHomeFilterSummaryChips->addWidget(emptySummary);
    }

    // 这里不要依赖 layout 的 sizeHint，直接使用手动累加的宽度
    const int summaryViewportWidth = qMax(1, ui->scrollHomeFilterSummary->viewport()->width());
    const int finalSummaryWidth = qMax(summaryContentWidth + 2, summaryViewportWidth);

    ui->homeFilterSummaryContents->setFixedSize(finalSummaryWidth, kSummaryChipHeight);
    ui->homeFilterSummaryContents->updateGeometry();

    ui->layoutHomeFilterSummaryChips->invalidate();
    ui->layoutHomeFilterSummaryChips->activate();

    ui->scrollHomeFilterSummary->horizontalScrollBar()->setSingleStep(40);
    ui->scrollHomeFilterSummary->horizontalScrollBar()->setPageStep(summaryViewportWidth);

    // =========================
    // 2. 展开 / 收起按钮和输入区
    // =========================
    ui->btnHomeFilterToggle->setChecked(homeFilterExpanded);
    ui->btnHomeFilterToggle->setText(homeFilterExpanded ? "收起筛选" : "展开筛选");

    auto setLayoutVisible = [](QLayout *layout, bool visible) {
        if (!layout) return;

        for (int i = 0; i < layout->count(); ++i) {
            QLayoutItem *item = layout->itemAt(i);
            if (!item) continue;

            if (QWidget *widget = item->widget()) {
                widget->setVisible(visible);
            }

            if (QLayout *childLayout = item->layout()) {
                for (int j = 0; j < childLayout->count(); ++j) {
                    if (QWidget *childWidget = childLayout->itemAt(j)->widget()) {
                        childWidget->setVisible(visible);
                    }
                }
            }
        }
    };

    setLayoutVisible(ui->horizontalLayout_HomeFilterInputs, homeFilterExpanded);

    // =========================
    // 3. 第二行：展开后的全部 Tag 区域
    // =========================
    ui->homeFilterChipsContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ui->layoutHomeFilterChips->setContentsMargins(0, 0, 0, 0);
    ui->layoutHomeFilterChips->setSpacing(0);
    ui->layoutHomeFilterChips->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    if (ui->scrollHomeFilterTags) {
        ui->scrollHomeFilterTags->setWidgetResizable(true);
        ui->scrollHomeFilterTags->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        ui->scrollHomeFilterTags->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        ui->scrollHomeFilterTags->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        ui->scrollHomeFilterTags->setVisible(homeFilterExpanded);
    } else {
        ui->homeFilterChipsContainer->setVisible(homeFilterExpanded);
    }

    if (!homeFilterExpanded) {
        if (ui->scrollHomeFilterTags) {
            ui->scrollHomeFilterTags->setFixedHeight(0);
            ui->scrollHomeFilterTags->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        }

        ui->homeFilterChipsContainer->setMinimumHeight(0);
        ui->homeFilterChipsContainer->setMaximumHeight(0);
        return;
    }

    QWidget *availableTagsWidget = new QWidget();
    availableTagsWidget->setStyleSheet("background:transparent;");
    availableTagsWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    FlowLayout *availableTagsLayout = new FlowLayout(availableTagsWidget, 0, 6, 6);

    if (ui->btnHomeTagSortMode) {
        ui->btnHomeTagSortMode->setText(homeFilterTagsSortByCount ? "按次数" : "按字母");
        ui->btnHomeTagSortMode->setToolTip(homeFilterTagsSortByCount
            ? "当前按出现次数排序，点击切换为字母序"
            : "当前按字母序排序，点击切换为出现次数");
    }

    for (const HomeFilterTagInfo &info : availableTagInfos) {
        const QString tag = info.tag;
        bool tagSelected = false;

        for (const QString &selected : currentHomeTagFilters) {
            if (selected.compare(tag, Qt::CaseInsensitive) == 0) {
                tagSelected = true;
                break;
            }
        }

        QPushButton *tagButton = new QPushButton(forceWrap(QString("%1(%2)").arg(tag).arg(info.count)));
        tagButton->setProperty("class", "filterChip");
        tagButton->setProperty("tagSource", info.sourceName());
        tagButton->setCheckable(true);
        tagButton->setChecked(tagSelected);
        tagButton->setCursor(Qt::PointingHandCursor);
        const QString sourceText = info.fromModel && info.fromUser
            ? "模型 Tag + 用户 Tag"
            : (info.fromUser ? "用户 Tag" : "模型 Tag");
        tagButton->setToolTip(QString("%1，出现于 %2 个模型。点击添加/移除此 Tag 筛选。")
                                  .arg(sourceText)
                                  .arg(info.count));
        tagButton->setMaximumWidth(180);
        tagButton->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

        connect(tagButton, &QPushButton::clicked, this, [this, tag]() {
            QString existing;

            for (const QString &value : currentHomeTagFilters) {
                if (value.compare(tag, Qt::CaseInsensitive) == 0) {
                    existing = value;
                    break;
                }
            }

            if (existing.isEmpty()) {
                currentHomeTagFilters.insert(tag);
            } else {
                currentHomeTagFilters.remove(existing);
            }

            refreshHomeFilterChips();
            refreshHomeGallery();
        });

        availableTagsLayout->addWidget(tagButton);
    }

    if (availableTagInfos.isEmpty()) {
        QLabel *emptyTags = new QLabel("当前模型库没有可用模型 Tag");
        emptyTags->setStyleSheet(AppStyle::MutedLabelStyle);
        emptyTags->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        availableTagsLayout->addWidget(emptyTags);
    }

    ui->layoutHomeFilterChips->addWidget(availableTagsWidget);

    ui->layoutHomeFilterChips->invalidate();
    ui->layoutHomeFilterChips->activate();

    if (ui->scrollHomeFilterTags) {
        const int tagViewportWidth = qMax(1, ui->scrollHomeFilterTags->viewport()->width());

        int tagContentHeight = availableTagsLayout->heightForWidth(tagViewportWidth);
        tagContentHeight = qMax(tagContentHeight, availableTagsLayout->minimumSize().height());
        tagContentHeight = qMax(tagContentHeight, kSummaryChipHeight + 4);

        availableTagsWidget->setMinimumHeight(tagContentHeight);
        availableTagsWidget->setMaximumHeight(tagContentHeight);

        ui->homeFilterChipsContainer->setMinimumHeight(tagContentHeight);
        ui->homeFilterChipsContainer->setMaximumHeight(tagContentHeight);

        const int visibleHeight = qMin(tagContentHeight, kTagsMaxHeight);

        ui->scrollHomeFilterTags->setFixedHeight(visibleHeight);
        ui->scrollHomeFilterTags->setVerticalScrollBarPolicy(
            tagContentHeight > visibleHeight ? Qt::ScrollBarAsNeeded
                                             : Qt::ScrollBarAlwaysOff
        );

        ui->scrollHomeFilterTags->verticalScrollBar()->setSingleStep(40);
        QScrollBar *tagScrollBar = ui->scrollHomeFilterTags->verticalScrollBar();
        tagScrollBar->setValue(qBound(0, previousTagScrollValue, tagScrollBar->maximum()));
        QTimer::singleShot(0, this, [this, previousTagScrollValue]() {
            if (!ui || !ui->scrollHomeFilterTags || !homeFilterExpanded) return;
            QScrollBar *bar = ui->scrollHomeFilterTags->verticalScrollBar();
            bar->setValue(qBound(0, previousTagScrollValue, bar->maximum()));
        });
    }
}

QList<MainWindow::HomeFilterTagInfo> MainWindow::collectAvailableHomeFilterTags() const
{
    QHash<QString, HomeFilterTagInfo> tagInfoByKey;
    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!isModelListItem(item)) continue;

        QSet<QString> itemTagKeys;
        auto registerTags = [&](const QStringList &tags, bool fromModel) {
            for (const QString &raw : tags) {
                const QString tag = raw.trimmed();
                if (tag.isEmpty()) continue;
                const QString key = normalizedHomeTagKey(tag);
                if (key.isEmpty()) continue;

                HomeFilterTagInfo &info = tagInfoByKey[key];
                if (info.tag.isEmpty()) info.tag = tag;
                if (fromModel) {
                    info.fromModel = true;
                } else {
                    info.fromUser = true;
                }
                itemTagKeys.insert(key);
            }
        };

        registerTags(item->data(ROLE_MODEL_TAGS).toStringList(), true);
        registerTags(item->data(ROLE_USER_TAGS).toStringList(), false);

        for (const QString &key : itemTagKeys) {
            tagInfoByKey[key].count += 1;
        }
    }

    QList<HomeFilterTagInfo> tags = tagInfoByKey.values();
    std::sort(tags.begin(), tags.end(), [this](const HomeFilterTagInfo &a, const HomeFilterTagInfo &b) {
        if (homeFilterTagsSortByCount && a.count != b.count) return a.count > b.count;
        return QString::localeAwareCompare(a.tag, b.tag) < 0;
    });
    return tags;
}

void MainWindow::setHomeAuthorFilter(const QString &author)
{
    currentHomeAuthorFilter = author.trimmed();
    if (ui && ui->editHomeAuthorFilter && ui->editHomeAuthorFilter->text() != currentHomeAuthorFilter) {
        QSignalBlocker blocker(ui->editHomeAuthorFilter);
        ui->editHomeAuthorFilter->setText(currentHomeAuthorFilter);
    }
    refreshHomeFilterChips();
    refreshHomeGallery();
}

void MainWindow::addHomeTagFilter(const QString &tag)
{
    const QString clean = tag.trimmed();
    if (clean.isEmpty()) return;

    QString existing;
    for (const QString &value : currentHomeTagFilters) {
        if (value.compare(clean, Qt::CaseInsensitive) == 0) {
            existing = value;
            break;
        }
    }
    if (existing.isEmpty()) currentHomeTagFilters.insert(clean);
    if (ui && ui->editHomeTagFilter) ui->editHomeTagFilter->clear();
    refreshHomeFilterChips();
    refreshHomeGallery();
}

void MainWindow::clearHomeFilters()
{
    currentHomeAuthorFilter.clear();
    currentHomeTagFilters.clear();
    if (ui && ui->editHomeAuthorFilter) {
        QSignalBlocker blocker(ui->editHomeAuthorFilter);
        ui->editHomeAuthorFilter->clear();
    }
    if (ui && ui->editHomeTagFilter) ui->editHomeTagFilter->clear();
    refreshHomeFilterChips();
    refreshHomeGallery();
}

// 点击主页的大图 -> 跳转详情页
void MainWindow::onHomeGalleryClicked(QListWidgetItem *item)
{
    if (!item) return;

    // 1. 获取点击项的文件路径 (这是最可靠的唯一标识)
    QString targetPath = item->data(ROLE_FILE_PATH).toString();
    if (targetPath.isEmpty()) return;

    cancelPendingTasks();

    // 2. 在侧边栏 (modelList) 中寻找匹配该路径的项
    QListWidgetItem* matchItem = nullptr;
    for(int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem* sideItem = ui->modelList->item(i);
        if (sideItem->data(ROLE_FILE_PATH).toString() == targetPath) {
            matchItem = sideItem;
            break;
        }
    }

    // 3. 如果找到了，选中它并触发加载逻辑
    if (matchItem) {
        ui->modelList->setCurrentItem(matchItem);
        syncTreeSelection(targetPath);
        onModelListClicked(matchItem);
    }
}

// 侧边栏右键菜单
void MainWindow::onSidebarContextMenu(const QPoint &pos)
{
    // 获取当前选中的所有项目
    QList<QListWidgetItem*> selectedItems = ui->modelList->selectedItems();

    // 如果右键点击的位置不在选区内，Qt通常会清除选区并选中新项。
    // 但为了保险，如果 selectedItems 为空，尝试获取点击位置的单项
    if (selectedItems.isEmpty()) {
        QListWidgetItem *item = ui->modelList->itemAt(pos);
        if (item) selectedItems.append(item);
    }

    if (selectedItems.isEmpty()) return;

    // 调用重构后的菜单函数
    showCollectionMenu(selectedItems, ui->modelList->mapToGlobal(pos));
}

void MainWindow::onBtnFavoriteClicked()
{
    // 获取当前选中的模型（支持多选）
    QList<QListWidgetItem*> selectedItems = ui->modelList->selectedItems();
    if (selectedItems.isEmpty()) return;

    QPoint pos = ui->btnFavorite->mapToGlobal(QPoint(0, ui->btnFavorite->height()));
    showCollectionMenu(selectedItems, pos);
}

void MainWindow::onHomeGalleryContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = ui->homeGalleryList->itemAt(pos);
    if (!item) return; // 点击了空白处

    // 构造一个包含当前单项的列表
    QList<QListWidgetItem*> items;
    items.append(item);

    // 复用通用的菜单逻辑
    showCollectionMenu(items, ui->homeGalleryList->mapToGlobal(pos));
}

// ---------------------------------------------------------
// 主页与收藏夹逻辑结束
// ---------------------------------------------------------

// === 辅助：生成正方形图标 ===
QIcon MainWindow::getSquareIcon(const QPixmap &srcPix)
{
    if (srcPix.isNull()) return QIcon();

    // 1. 计算裁剪区域 (短边裁剪)
    int side = qMin(srcPix.width(), srcPix.height());
    // X轴居中，Y轴顶端对齐 (适合人物)
    int x = (srcPix.width() - side) / 2;
    int y = 0;

    // 获取原始的正方形裁剪图
    QPixmap square = srcPix.copy(x, y, side, side);

    // 2. === 核心修改：增加透明内边距 ===
    // 设定输出图标的基础分辨率 (越高越清晰，64x64 对侧边栏足够)
    int fullSize = 64;

    // 设定内边距 (比如 8px，意味着图片四周都有 8px 的透明区域)
    // 这样图片实际显示大小就是 48x48，视觉上就分开了
    int padding = 8;
    int contentSize = fullSize - (padding * 2);

    // 创建透明底图
    QPixmap finalPix(fullSize, fullSize);
    finalPix.fill(Qt::transparent);

    QPainter painter(&finalPix);
    // 开启高质量抗锯齿
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // 将裁剪好的图缩放并画在中间
    painter.drawPixmap(padding, padding,
                       square.scaled(contentSize, contentSize,
                                     Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation));

    return QIcon(finalPix);
}

// === 核心：事件过滤器 (绘图 + 点击) ===
bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->heroFrame) {
        if (event->type() == QEvent::Paint) {
            QPainter painter(ui->heroFrame);

            // 绘制背景黑底（防止透明度叠加时看到底色）
            painter.fillRect(ui->heroFrame->rect(), Qt::black);

            // 辅助 Lambda：用于绘制单张图片 (Cover 模式)
            auto drawPix = [&](const QPixmap &pix, qreal opacity) {
                if (pix.isNull()) return;
                QSize widgetSize = ui->heroFrame->size();
                QSize imgSize = pix.size();
                if (imgSize.isEmpty()) return;

                // Cover 算法
                double scaleW = (double)widgetSize.width() / imgSize.width();
                double scaleH = (double)widgetSize.height() / imgSize.height();
                double scale = qMax(scaleW, scaleH);

                double newW = imgSize.width() * scale;
                double newH = imgSize.height() * scale;
                double offsetX = (widgetSize.width() - newW) / 2.0;
                double offsetY = (widgetSize.height() - newH) / 4.0;

                painter.setOpacity(opacity);
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.drawPixmap(QRectF(offsetX, offsetY, newW, newH), pix, pix.rect());
            };

            // 情况 A: 正在切换到一张新图片 (Next 存在)
            if (!nextHeroPixmap.isNull()) {
                // 1. 底层：画旧图 (始终 1.0，让新图盖在上面，这样没有黑缝)
                drawPix(currentHeroPixmap, 1.0);
                // 2. 顶层：画新图 (透明度从 0 -> 1)
                drawPix(nextHeroPixmap, transitionOpacity);
            }
            // 情况 B: 正在切换到“无图片”状态 (Next 为空，且正在动画中)
            else if (transitionAnim->state() == QAbstractAnimation::Running) {
                // 让旧图慢慢消失 (透明度 1 -> 0)
                drawPix(currentHeroPixmap, 1.0 - transitionOpacity);
            }
            // 情况 C: 静止状态 (动画结束)
            else {
                drawPix(currentHeroPixmap, 1.0);
            }

            return true;
        }

        // --- 处理点击 (查看大图) ---
        if (event->type() == QEvent::MouseButtonPress) {
            QMouseEvent *mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                if (!currentHeroPath.isEmpty() && QFile::exists(currentHeroPath)) {
                    showFullImageDialog(currentHeroPath); // 使用新封装的函数
                    return true;
                }
            }
        }
    }

    if (watched == ui->scrollAreaWidgetContents && event->type() == QEvent::Resize) {
        if (ui->backgroundLabel) {
            bgResizeTimer->start(0);
        }
    }

    if (watched == ui->listUserImages->viewport() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        scheduleVisibleUserImageThumbLoad();
    }

    if (event->type() == QEvent::MouseButtonDblClick) {
        // 尝试将 watched 对象转换为 QPushButton
        QPushButton *btn = qobject_cast<QPushButton*>(watched);
        if (btn) {
            // 获取我们之前绑定的 fullImagePath 属性
            QString path = btn->property("fullImagePath").toString();
            if (!path.isEmpty() && QFile::exists(path)) {
                showFullImageDialog(path); // 打开大图
                return true; // 消费事件
            }
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

// ---------------------------------------------------------
// 业务逻辑
// ---------------------------------------------------------

void MainWindow::scanModels(const QString &path)
{
    scanModels(QStringList() << path);
}

void MainWindow::scanModels(const QStringList &paths)
{
    // 1. 锁定 UI 更新，防止闪烁
    ui->modelList->setUpdatesEnabled(false);
    ui->modelList->clear();

    ui->comboBaseModel->blockSignals(true);
    ui->comboBaseModel->clear();
    ui->comboBaseModel->addItem("All");

    QSet<QString> foundBaseModels; // 用于去重记录发现的底模

    // 2. 准备文件名过滤器
    QStringList nameFilters;
    nameFilters << "*.safetensors" << "*.ckpt" << "*.pt";

    // 3. 准备目录过滤器 (只看文件，不包含 . 和 ..)
    QDir::Filters dirFilters = QDir::Files | QDir::NoDotAndDotDot;

    // 4. 准备迭代器标志 (是否递归)
    QDirIterator::IteratorFlags iterFlags = QDirIterator::NoIteratorFlags;
    if (optLoraRecursive) {
        iterFlags = QDirIterator::Subdirectories; // 开启递归
    }

    int addedCount = 0;

    // 5. 遍历多个路径
    for (const QString &path : paths) {
        if (path.isEmpty() || !QDir(path).exists()) continue;
        const QString rootPath = QFileInfo(path).absoluteFilePath();
        QString rootName = QFileInfo(rootPath).fileName();
        if (rootName.isEmpty()) rootName = rootPath;

        // 构造函数签名: QDirIterator(path, nameFilters, filters, flags)
        QDirIterator it(path, nameFilters, dirFilters, iterFlags);

        while (it.hasNext()) {
            it.next();
            QFileInfo fileInfo = it.fileInfo();

            QString baseName = fileInfo.completeBaseName();
            QString fullPath = fileInfo.absoluteFilePath();

            // 获取当前文件所在的目录 (递归模式下可能是子目录)
            QDir currentFileDir = fileInfo.dir();

            // 6. 寻找预览图
            QString previewPath = "";
            QStringList imgExts = {".preview.png", ".png", ".jpg", ".jpeg"};
            for (const QString &ext : imgExts) {
                // 在当前模型文件的同级目录下找图片
                QString tryPath = currentFileDir.absoluteFilePath(baseName + ext);
                if (QFile::exists(tryPath)) {
                    previewPath = tryPath;
                    break;
                }
            }

            // 7. 创建列表项
            QListWidgetItem *item = new QListWidgetItem(baseName);
            item->setToolTip(fullPath);
            item->setData(ROLE_MODEL_NAME, baseName);
            item->setData(ROLE_FILE_PATH, fullPath);
            item->setData(ROLE_PREVIEW_PATH, previewPath);
            item->setData(ROLE_MODEL_ROOT_PATH, rootPath);
            item->setData(ROLE_MODEL_ROOT_NAME, rootName);
            item->setData(ROLE_MODEL_FILTER_VISIBLE, true);

            QString jsonPath = currentFileDir.filePath(baseName + ".json");
            preloadItemMetadata(item, jsonPath);

            int nsfwLevel = item->data(ROLE_NSFW_LEVEL).toInt();
            bool isNSFW = nsfwLevel > optNSFWLevel;

            if (optFilterNSFW && isNSFW && optNSFWMode == 0) {
                // 如果开启过滤 + 是NSFW + 模式为隐藏(0) -> 直接删除item并跳过
                delete item;
                continue;
            }

            QString civitaiName = item->data(ROLE_CIVITAI_NAME).toString();
            if (optUseCivitaiName && !civitaiName.isEmpty()) {
                item->setText(civitaiName);
            } else {
            item->setText(baseName); // 默认使用文件名
            }

            item->setIcon(placeholderIcon);
            applyModelHighlightColor(item);
            applyModelUserNoteData(item);

            // 9. 处理底模过滤器
            QString baseModel = item->data(ROLE_FILTER_BASE).toString();
            if (!baseModel.isEmpty() && !foundBaseModels.contains(baseModel)) {
                foundBaseModels.insert(baseModel);
                ui->comboBaseModel->addItem(baseModel);
            }

            ui->modelList->addItem(item);
            addedCount++;
            if (!previewPath.isEmpty()) {
                // 【修改】添加 "SIDEBAR:" 前缀
                QString taskId = "SIDEBAR:" + fullPath;

                // 使用 backgroundThreadPool (静默加载)
                IconLoaderTask *task = new IconLoaderTask(previewPath, 64, 8, this, taskId);
                task->setAutoDelete(true);
                backgroundThreadPool->start(task);
            }
        }
    }

    // 10. 恢复 UI 更新
    ui->statusbar->showMessage(QString("扫描完成，共 %1 个模型").arg(addedCount));
    ui->comboBaseModel->blockSignals(false);
    ui->modelList->setUpdatesEnabled(true);

    refreshModelUsageStatsAsync();

    // 11. 刷新主页大图视图
    executeSort();
    refreshHomeGallery();
    // 刷新收藏夹树状视图
    refreshCollectionTreeView();
}

// 更新界面显示
void MainWindow::updateDetailView(const ModelMeta &meta)
{
    // 1. 基础信息
    ui->lblModelName->setWordWrap(true);
    ui->lblModelName->setText(forceWrap(meta.name));
    ui->heroFrame->setProperty("fullImagePath", meta.previewPath);
    ui->btnCheckModelUpdate->setVisible(true);

    if (!meta.modelUrl.isEmpty()) {
        ui->btnOpenUrl->setVisible(true);
        ui->btnOpenUrl->setProperty("url", meta.modelUrl);
        ui->btnOpenUrl->setToolTip(meta.modelUrl.contains("civarchive.com", Qt::CaseInsensitive)
                                   ? "访问 CivArchive"
                                   : "访问 Civitai");
    } else { ui->btnOpenUrl->setVisible(false); }

    // 2. 标签栏 (Badges)
    clearLayout(ui->badgesFrame->layout());

    if (meta.nsfw) addBadge("NSFW", true);
    if (!meta.baseModel.isEmpty()) addBadge(meta.baseModel);
    if (!meta.type.isEmpty()) addBadge(meta.type);
    if (meta.fileSizeMB > 0) addBadge(QString("%1 MB").arg(meta.fileSizeMB, 0, 'f', 1));

    if (!meta.createdAt.isEmpty()) {
        QDateTime dt = QDateTime::fromString(meta.createdAt, Qt::ISODate);
        if (dt.isValid()) {
            addBadge("📅 " + dt.toString("yyyy-MM-dd"));
        }
    }

    if (meta.downloadCount > 0) {
        QString dlStr = (meta.downloadCount > 1000) ? QString::number(meta.downloadCount/1000.0, 'f', 1)+"k" : QString::number(meta.downloadCount);
        addBadge(QString("⇩ %1").arg(dlStr));
    }
    if (meta.thumbsUpCount > 0) addBadge(QString("👍 %1").arg(meta.thumbsUpCount));
    if (meta.isLocalOnly) addBadge("LOCAL");
    if (meta.isLocalEdited) addBadge("EDITED", true);

    ((QHBoxLayout*)ui->badgesFrame->layout())->addStretch(); // 左对齐

    // 3. 动态生成触发词框 (Trigger Words)
    refreshTriggerWordsPanel(meta);

    // 4. 图库 (Gallery)
    clearLayout(ui->layoutGallery);

    // 中止正在进行的画廊预览图下载任务，防止后台积累僵尸队列
    if (netManager) {
        const QList<QNetworkReply*> replies = netManager->findChildren<QNetworkReply*>();
        for (QNetworkReply *reply : replies) {
            if (reply && reply->property("isGalleryDownload").toBool()) {
                reply->abort();
            }
        }
    }

    downloadQueue.clear();
    isDownloading = false;
    beginGalleryBuild(meta);

    // 5. 右侧信息
    ui->textDescription->setHtml(meta.description);
    QFileInfo fi(meta.filePath);
    QDateTime addedTime = fi.birthTime();
    if(!addedTime.isValid()) addedTime = fi.lastModified();
    QString addedStr = addedTime.toString("yyyy-MM-dd");
    ui->lblFileInfo->setWordWrap(true);
    const QString displayFileName = meta.fileNameServer.isEmpty() ? meta.fileName : meta.fileNameServer;
    ui->lblFileInfo->setText(QString("Filename: %1\nSize: %2 MB\nSHA256: %3\nAdded: %4")
                                 .arg(forceWrap(displayFileName))
                                 .arg(meta.fileSizeMB, 0, 'f', 1)
                                 .arg(meta.sha256.left(10) + "...")
                                 .arg(addedStr));
    refreshModelAttributionPanel(meta);
    refreshModelUserNotePanel(meta.filePath);

    updateLocalEditorFromMeta(meta);

    if (!meta.images.isEmpty()) {
        onGalleryImageClicked(0);
    }

    QTimer::singleShot(0, this, [this, meta](){
        fitDetailContentToCurrentPage();
        transitionToImage(meta.previewPath);
    });
}

void MainWindow::fitDetailContentToCurrentPage()
{
    if (!ui || !ui->detailContentStack || !ui->contentAreaWidget) return;

    QWidget *page = ui->detailContentStack->currentWidget();
    if (!page) return;

    int targetHeight = 0;
    const int currentIndex = ui->detailContentStack->currentIndex();
    if (currentIndex == 1) {
        targetHeight = 750;
    } else if (currentIndex == 2) {
        targetHeight = 900;
    } else {
        if (QLayout *pageLayout = page->layout()) {
            pageLayout->activate();
        }
        page->updateGeometry();
        targetHeight = qMax(page->minimumSizeHint().height(), page->sizeHint().height());
    }

    targetHeight = qMax(1, targetHeight);
    ui->detailContentStack->setFixedHeight(targetHeight);
    ui->contentAreaWidget->setFixedHeight(targetHeight);
    ui->detailContentStack->updateGeometry();
    ui->contentAreaWidget->updateGeometry();
    if (ui->scrollAreaWidgetContents) {
        ui->scrollAreaWidgetContents->adjustSize();
    }
}

void MainWindow::cancelGalleryBuild()
{
    ++galleryBuildToken;
    pendingGalleryIndices.clear();
    pendingGalleryMeta = ModelMeta();
    pendingGalleryModelDir.clear();
    pendingGalleryBaseName.clear();
    if (detailGalleryBuildTimer) {
        detailGalleryBuildTimer->stop();
    }
}

void MainWindow::beginGalleryBuild(const ModelMeta &meta)
{
    cancelGalleryBuild();

    if (meta.images.isEmpty()) {
        ui->layoutGallery->addWidget(new QLabel("No preview images."));
        return;
    }

    pendingGalleryMeta = meta;
    QFileInfo modelFileInfo(meta.filePath);
    pendingGalleryModelDir = modelFileInfo.absolutePath();

    if (QListWidgetItem *listItem = ui->modelList->currentItem()) {
        pendingGalleryBaseName = listItem->data(ROLE_MODEL_NAME).toString();
    }
    if (pendingGalleryBaseName.isEmpty()) {
        pendingGalleryBaseName = modelFileInfo.completeBaseName();
    }

    for (int i = 0; i < meta.images.count(); ++i) {
        pendingGalleryIndices.append(i);
    }

    const int initialBatch = qMin(8, pendingGalleryIndices.size());
    for (int i = 0; i < initialBatch; ++i) {
        int imageIndex = pendingGalleryIndices.takeFirst();
        addGalleryThumbButton(pendingGalleryMeta, imageIndex, pendingGalleryModelDir, pendingGalleryBaseName);
    }

    if (ui->layoutGallery->count() > 0) {
        QPushButton *firstBtn = qobject_cast<QPushButton*>(ui->layoutGallery->itemAt(0)->widget());
        if (firstBtn) {
            firstBtn->setChecked(true);
        }
    }

    if (!pendingGalleryIndices.isEmpty()) {
        detailGalleryBuildTimer->start(0);
    } else {
        ui->layoutGallery->addStretch();
    }
}

void MainWindow::buildGalleryBatch()
{
    if (pendingGalleryIndices.isEmpty()) {
        if (ui->layoutGallery->count() == 0 || ui->layoutGallery->itemAt(ui->layoutGallery->count() - 1)->spacerItem() == nullptr) {
            ui->layoutGallery->addStretch();
        }
        return;
    }

    const int batchSize = 12;
    for (int i = 0; i < batchSize && !pendingGalleryIndices.isEmpty(); ++i) {
        int imageIndex = pendingGalleryIndices.takeFirst();
        addGalleryThumbButton(pendingGalleryMeta, imageIndex, pendingGalleryModelDir, pendingGalleryBaseName);
    }

    if (!pendingGalleryIndices.isEmpty()) {
        detailGalleryBuildTimer->start(0);
    } else {
        ui->layoutGallery->addStretch();
    }
}

void MainWindow::addGalleryThumbButton(const ModelMeta &meta, int index, const QString &modelDir, const QString &baseName)
{
    if (index < 0 || index >= meta.images.count()) return;

    const ImageInfo &img = meta.images[index];
    bool isNsfw = (img.nsfwLevel > optNSFWLevel);
    if (optFilterNSFW && isNsfw && optNSFWMode == 0) {
        return;
    }

    QPushButton *thumbBtn = new QPushButton();
    thumbBtn->setFixedSize(100, 150);
    thumbBtn->setCheckable(true);
    thumbBtn->setAutoExclusive(true);
    thumbBtn->setCursor(Qt::PointingHandCursor);
    thumbBtn->setProperty("class", "galleryThumb");
    thumbBtn->setProperty("isNSFW", isNsfw);
    thumbBtn->setContextMenuPolicy(Qt::CustomContextMenu);

    QString suffix = (index == 0) ? ".preview.png" : QString(".preview.%1.png").arg(index);
    QString rawPath = QDir(modelDir).filePath(baseName + suffix);
    QString strictLocalPath = QFileInfo(rawPath).absoluteFilePath();

    QString effectivePath = strictLocalPath;
    if (index == 0 && !QFile::exists(effectivePath) && !meta.previewPath.isEmpty() && QFile::exists(meta.previewPath)) {
        effectivePath = QFileInfo(meta.previewPath).absoluteFilePath();
    }

    thumbBtn->setProperty("fullImagePath", effectivePath);
    thumbBtn->setProperty("downloadUrl", img.url);
    thumbBtn->setProperty("savePath", strictLocalPath);
    thumbBtn->setProperty("localBaseName", baseName);
    thumbBtn->setProperty("imageIndex", index);
    thumbBtn->installEventFilter(this);

    const PreviewMetadataPayload previewPayload = previewPayloadFromImageInfo(img);
    if (QFile::exists(effectivePath) && !(m_forceResyncPreview && index > 0 && !img.url.isEmpty())) {
        thumbBtn->setText("Loading...");
        IconLoaderTask *task = new IconLoaderTask(effectivePath, 100, 0, this, effectivePath, true);
        task->setAutoDelete(true);
        threadPool->start(task);
    } else {
        if (m_skipPreviewSync) {
            thumbBtn->setText("Skipped");
        } else if (img.url.isEmpty()) {
            thumbBtn->setText("Missing");
        } else {
            if (QFile::exists(strictLocalPath) && index > 0 && m_forceResyncPreview) {
                QFile::remove(strictLocalPath);
            }
            thumbBtn->setText(index == 0 ? "Downloading..." : "Queueing...");
            enqueueDownload(img.url, strictLocalPath, thumbBtn, baseName, index, previewPayload);
        }
    }

    connect(thumbBtn, &QPushButton::clicked, this, [this, index](){
        onGalleryImageClicked(index);
    });
    connect(thumbBtn, &QPushButton::customContextMenuRequested, this, [this, thumbBtn](const QPoint &pos){
        if (!thumbBtn) return;

        const QString downloadUrl = thumbBtn->property("downloadUrl").toString();
        const QString savePath = QFileInfo(thumbBtn->property("savePath").toString()).absoluteFilePath();
        const QString localBaseName = thumbBtn->property("localBaseName").toString();
        const int imageIndex = thumbBtn->property("imageIndex").toInt();

        QMenu menu(this);
        QAction *actRedownload = menu.addAction("重新下载 / Re-download");
        actRedownload->setEnabled(!downloadUrl.isEmpty() && !savePath.isEmpty());

        QAction *actOpenLocation = menu.addAction("打开所在位置 / Show in Folder");
        actOpenLocation->setEnabled(!savePath.isEmpty());

        QAction *chosen = menu.exec(thumbBtn->mapToGlobal(pos));
        if (!chosen) return;

        if (chosen == actRedownload) {
            if (QFile::exists(savePath)) {
                QFile::remove(savePath);
            }
            thumbBtn->setIcon(QIcon());
            thumbBtn->setText(imageIndex == 0 ? "Downloading..." : "Queueing...");
            PreviewMetadataPayload payload;
            if (imageIndex >= 0 && imageIndex < currentMeta.images.size()) {
                payload = previewPayloadFromImageInfo(currentMeta.images.at(imageIndex));
            }
            enqueueDownload(downloadUrl, savePath, thumbBtn, localBaseName, imageIndex, payload);
            return;
        }

        if (chosen == actOpenLocation) {
            showFileInFolder(savePath);
        }
    });

    ui->layoutGallery->addWidget(thumbBtn);
}

void MainWindow::onGalleryImageClicked(int index)
{
    if (index < 0 || index >= currentMeta.images.count()) return;

    const ImageInfo &img = currentMeta.images[index];

    // 1. 更新 Prompt 显示
    ui->textImgPrompt->setPlainText(img.prompt.isEmpty() ? "No positive prompt." : img.prompt);
    ui->textImgNegPrompt->setPlainText(img.negativePrompt.isEmpty() ? "No negative prompt." : img.negativePrompt);

    // === 动态获取模型所在的子目录 ===
    QString currentBaseName;
    QString modelDir; // 用于存储该模型实际所在的文件夹路径

    QListWidgetItem *item = ui->modelList->currentItem();
    if (item) {
        // A. 获取模型名称标识
        currentBaseName = item->data(ROLE_MODEL_NAME).toString();
        if (currentBaseName.isEmpty()) currentBaseName = item->text();

        // B. 获取模型文件所在的绝对目录 (支持子文件夹的关键)
        QString fullModelPath = item->data(ROLE_FILE_PATH).toString();
        if (!fullModelPath.isEmpty()) {
            modelDir = QFileInfo(fullModelPath).absolutePath();
        }
    } else {
        // 兜底逻辑：如果侧边栏没选中，尝试从 currentMeta 推断
        currentBaseName = currentMeta.name;
        modelDir = QFileInfo(currentMeta.filePath).absolutePath();
    }

    // 如果因为某种异常没拿到目录，则回退到 Lora 根目录
    if (modelDir.isEmpty()) {
        modelDir = currentLoraPath;
    }

    // 2. 寻找本地图片路径 (使用解析出的 modelDir 而不是全局 currentLoraPath)
    QString localPath = findLocalPreviewPath(modelDir, currentBaseName, currentMeta.fileNameServer, index);

    int width = img.width;
    int height = img.height;
    if ((width <= 0 || height <= 0) && QFile::exists(localPath)) {
        QImageReader reader(localPath);
        const QSize size = reader.size();
        width = size.width();
        height = size.height();
    }

    const QString resolution = (width > 0 && height > 0)
        ? QString("%1 x %2").arg(width).arg(height)
        : "--";
    auto richValue = [](const QString &value) {
        return value.trimmed().isEmpty() ? QStringLiteral("--") : value.toHtmlEscaped();
    };
    const QString sampler = img.sampler.trimmed().isEmpty()
        ? QStringLiteral("--")
        : forceWrap(img.sampler.trimmed()).toHtmlEscaped();
    QString params = QString("Resolution: <span style='color:white'>%1</span> | Steps: <span style='color:white'>%2</span> | CFG: <span style='color:white'>%3</span><br/>Sampler: <span style='color:white'>%4</span> | Seed: <span style='color:white'>%5</span>")
                         .arg(resolution.toHtmlEscaped())
                         .arg(richValue(img.steps))
                         .arg(richValue(img.cfgScale))
                         .arg(sampler)
                         .arg(richValue(img.seed));
    ui->lblImgParams->setText(params);

    // 3. 执行过渡
    if (QFile::exists(localPath)) {
        transitionToImage(localPath);
    } else {
        qDebug() << "[Debug] Preview image not found at:" << localPath;
    }
}

void MainWindow::refreshTriggerWordsPanel(const ModelMeta &meta)
{
    if (!ui || !ui->layoutTriggerStack) return;

    clearLayout(ui->layoutTriggerStack);

    const QString modelNoteKey = meta.filePath.isEmpty() ? QString() : QFileInfo(meta.filePath).absoluteFilePath();
    const QStringList customTriggerGroups = normalizeModelCustomTriggers(modelUserNotes.value(modelNoteKey).customTriggers);

    auto addTriggerGroup = [this](const QString &words, const QString &sourceLabel, const QString &accentColor) {
        QWidget *rowWidget = new QWidget();
        QVBoxLayout *outerLayout = new QVBoxLayout(rowWidget);
        outerLayout->setContentsMargins(0, 0, 0, 10);
        outerLayout->setSpacing(4);

        QLabel *source = new QLabel(sourceLabel);
        source->setStyleSheet(QString("color:%1;font-weight:bold;background:transparent;margin-left:2px;").arg(accentColor));
        outerLayout->addWidget(source);

        QHBoxLayout *rowLayout = new QHBoxLayout();
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(5);

        QTextBrowser *tb = new QTextBrowser();
        tb->setText(words);
        tb->setFixedHeight(90);
        tb->setStyleSheet(QString("QTextBrowser { border: 1px solid %1; border-radius: 8px; }").arg(accentColor));

        QPushButton *btnCopy = new QPushButton("Copy");
        btnCopy->setFixedSize(60, 90);
        btnCopy->setCursor(Qt::PointingHandCursor);
        btnCopy->setProperty("class", "copyBtn");

        connect(btnCopy, &QPushButton::clicked, this, [words, this]() {
            QClipboard *clip = QGuiApplication::clipboard();
            clip->setText(words);
            ui->statusbar->showMessage("Copied trigger words!", 1500);
        });

        rowLayout->addWidget(tb);
        rowLayout->addWidget(btnCopy);
        outerLayout->addLayout(rowLayout);
        ui->layoutTriggerStack->addWidget(rowWidget);
    };

    if (meta.trainedWordsGroups.isEmpty() && customTriggerGroups.isEmpty()) {
        QLabel *lbl = new QLabel("No trigger words provided.");
        lbl->setStyleSheet("color: #666; font-style: italic; margin-left: 10px;");
        ui->layoutTriggerStack->addWidget(lbl);
        return;
    }

    for (const QString &words : meta.trainedWordsGroups) {
        addTriggerGroup(words, "Civitai / Metadata", AppStyle::AccentBlue);
    }
    for (const QString &words : customTriggerGroups) {
        addTriggerGroup(words, "Custom / 用户自定义", AppStyle::CustomTriggerGreen);
    }

    ui->layoutTriggerStack->addStretch(1);
}

// 辅助函数
void MainWindow::addBadge(QString text, bool isRed)
{
    QLabel *lbl = new QLabel(text);
    if (text == "LOCAL") {
        lbl->setProperty("class", "tagGreen");
    } else {
        lbl->setProperty("class", isRed ? "tagRed" : "tag");
    }
    ui->badgesFrame->layout()->addWidget(lbl);
}

void MainWindow::clearLayout(QLayout *layout)
{
    if (!layout) return;
    QLayoutItem *item;
    while ((item = layout->takeAt(0)) != nullptr) {
        if (item->widget()) delete item->widget();
        if (item->layout()) clearLayout(item->layout());
        delete item;
    }
}

void MainWindow::clearDetailView()
{
    cancelGalleryBuild();
    editImagesNeedRefresh = false;
    ui->lblModelName->setText("请选择一个模型 / Select a Model");
    setModelTitleNormal();
    ui->lblModelName->setStyleSheet(AppStyle::modelTitleNormalStyle());
    ui->textDescription->clear();
    ui->textDescription->setPlaceholderText("暂无简介 / No description.");
    ui->lblFileInfo->setText("Filename: --\nSize: --\nHash: --");
    refreshModelAttributionPanel(ModelMeta());
    refreshModelUserNotePanel("");

    ui->textImgPrompt->clear();
    ui->textImgNegPrompt->clear();
    ui->lblImgParams->setText("Resolution: -- | Steps: -- | CFG: --<br/>Sampler: -- | Seed: --");

    ui->btnOpenUrl->setVisible(false);
    ui->btnCopyLoraTag->setVisible(false);

    clearLayout(ui->badgesFrame->layout());

    clearLayout(ui->layoutTriggerStack);
    clearLayout(ui->layoutGallery);

    // 中止正在进行的画廊预览图下载任务
    if (netManager) {
        const QList<QNetworkReply*> replies = netManager->findChildren<QNetworkReply*>();
        for (QNetworkReply *reply : replies) {
            if (reply && reply->property("isGalleryDownload").toBool()) {
                reply->abort();
            }
        }
    }
    downloadQueue.clear();
    isDownloading = false;

    // ui->widgetLocalMeta->setEnabled(false);
    ui->groupEditImages->setEnabled(false);
    ui->groupEditMeta->setEnabled(false);
    ui->editLocalModelName->clear();
    ui->editLocalVersion->clear();
    ui->editLocalBaseModel->clear();
    ui->editLocalType->clear();
    ui->editLocalModelUrl->clear();
    ui->editLocalCreatedAt->clear();
    ui->editLocalDownloads->clear();
    ui->editLocalLikes->clear();
    ui->editLocalCreator->clear();
    ui->textLocalModelTags->clear();
    ui->chkLocalNSFW->setChecked(false);
    ui->textLocalTriggers->clear();
    ui->textLocalDescription->clear();
    ui->lblLocalMetaStatus->setText("状态: -");
    ui->listEditImages->clear();
    ui->lblEditImagePath->setText("文件: -");
    ui->lblEditImageSize->setText("尺寸: -");
    ui->textEditImgPrompt->clear();
    ui->textEditImgNegPrompt->clear();
    ui->editImgSampler->clear();
    ui->editImgSteps->clear();
    ui->editImgCfg->clear();
    ui->editImgSeed->clear();
    ui->spinImgNsfw->setValue(1);
    currentEditImageIndex = -1;
    QTimer::singleShot(0, this, &MainWindow::fitDetailContentToCurrentPage);
}

void MainWindow::setModelTitleNormal()
{
    ui->lblModelName->setStyleSheet(AppStyle::modelTitleNormalStyle());
}

void MainWindow::setModelTitleError(const QString &message)
{
    ui->lblModelName->setText(QString("连接失败 / Error: %1").arg(message));
    ui->lblModelName->setStyleSheet(AppStyle::modelTitleErrorStyle());
}

void MainWindow::showPendingLocalModelDetail(const ModelMeta &meta, const QString &message)
{
    clearDetailView();
    currentMeta = meta;
    // 同步失败/待同步模型：保留复制 LoRA 标签，隐藏联网相关按钮
    ui->btnCopyLoraTag->setVisible(true);
    ui->btnOpenUrl->setVisible(false);
    ui->btnCheckModelUpdate->setVisible(false);
    setModelTitleNormal();
    ui->lblModelName->setWordWrap(true);
    ui->lblModelName->setText(forceWrap(meta.name.isEmpty() ? meta.fileName : meta.name));
    ui->textDescription->setPlainText(message);
    QFileInfo fi(meta.filePath);
    ui->lblFileInfo->setWordWrap(true);
    ui->lblFileInfo->setText(QString("Filename: %1\nSize: %2 MB\nHash: --")
                                 .arg(forceWrap(fi.fileName()))
                                 .arg(fi.exists() ? QString::number(fi.size() / 1024.0 / 1024.0, 'f', 2) : "--"));
    refreshModelAttributionPanel(meta);
    refreshModelUserNotePanel(meta.filePath);
    if (!meta.previewPath.isEmpty() && QFile::exists(meta.previewPath)) {
        transitionToImage(meta.previewPath);
    } else {
        transitionToImage("");
    }
    updateLocalEditorFromMeta(meta);
    QTimer::singleShot(0, this, &MainWindow::fitDetailContentToCurrentPage);
}

void MainWindow::updateLocalEditorFromMeta(const ModelMeta &meta)
{
    // ui->widgetLocalMeta->setEnabled(true);
    ui->groupEditImages->setEnabled(true);
    ui->groupEditMeta->setEnabled(true);

    QString modelName = meta.modelName.isEmpty() ? meta.name : meta.modelName;
    ui->editLocalModelName->setText(modelName);
    ui->editLocalVersion->setText(meta.versionName);
    ui->editLocalBaseModel->setText(meta.baseModel);
    ui->editLocalType->setText(meta.type);
    ui->editLocalModelUrl->setText(meta.modelUrl);
    ui->editLocalCreatedAt->setText(meta.createdAt);
    ui->editLocalDownloads->setText(QString::number(meta.downloadCount));
    ui->editLocalLikes->setText(QString::number(meta.thumbsUpCount));
    ui->editLocalCreator->setText(meta.creatorName);
    ui->textLocalModelTags->setPlainText(meta.modelTags.join("\n"));
    ui->chkLocalNSFW->setChecked(meta.nsfw);
    ui->textLocalTriggers->setPlainText(meta.trainedWordsGroups.join("\n"));

    QString descPlain = meta.description;
    if (!descPlain.isEmpty()) {
        QTextDocument doc;
        doc.setHtml(descPlain);
        descPlain = doc.toPlainText();
    }
    ui->textLocalDescription->setPlainText(descPlain);

    setLocalMetaStatus(meta);
    editImagesNeedRefresh = true;
    if (ui->detailContentStack->currentIndex() == 2) {
        refreshEditImages(meta);
    }
}

void MainWindow::setLocalMetaStatus(const ModelMeta &meta)
{
    QString status;
    QString color = AppStyle::WhiteText;
    if (meta.isLocalOnly && meta.isLocalEdited) {
        status = "状态: 本地模型 (已编辑)";
        color = AppStyle::WarningYellow;
    } else if (meta.isLocalEdited) {
        status = "状态: 已编辑 (本地元数据)";
        color = AppStyle::WarningYellow;
    } else if (meta.isLocalOnly) {
        status = "状态: 本地模型";
        color = AppStyle::AccentBlue;
    } else {
        status = "状态: Civitai 元数据";
    }

    QStringList details;
    QString filePath;
    if (!meta.filePath.isEmpty()) filePath = QFileInfo(meta.filePath).absoluteFilePath();
    if (filePath.isEmpty() && ui && ui->modelList && ui->modelList->currentItem()) {
        const QString itemPath = ui->modelList->currentItem()->data(ROLE_FILE_PATH).toString();
        if (!itemPath.isEmpty()) filePath = QFileInfo(itemPath).absoluteFilePath();
    }

    QString jsonPath;
    QString baseName;
    if (!filePath.isEmpty()) {
        QFileInfo modelInfo(filePath);
        baseName = modelInfo.completeBaseName();
        jsonPath = modelInfo.absoluteDir().filePath(baseName + ".json");
        details << (QFileInfo::exists(jsonPath) ? "JSON: 已缓存" : "JSON: 未缓存");
    }

    if (meta.modelId > 0) details << QString("Model ID: %1").arg(meta.modelId);
    if (meta.versionId > 0) details << QString("Version ID: %1").arg(meta.versionId);
    if (!meta.sha256.isEmpty()) details << "SHA256: 已记录";

    if (ui && ui->modelList && ui->modelList->currentItem()) {
        QListWidgetItem *item = ui->modelList->currentItem();
        const int usageCount = item->data(ROLE_SORT_USAGE_COUNT).toInt();
        const qint64 lastUsed = item->data(ROLE_SORT_LAST_USED).toLongLong();
        details << QString("本地返图: %1 张").arg(usageCount);
        if (lastUsed > 0) {
            details << QString("最近使用: %1").arg(QDateTime::fromMSecsSinceEpoch(lastUsed).toString("yyyy-MM-dd"));
        }
    }

    if (!filePath.isEmpty() && modelSyncFailures.contains(filePath)) {
        const QString error = modelSyncFailures.value(filePath)["error"].toString();
        details << (error.isEmpty() ? "同步失败缓存: 已记录" : QString("同步失败缓存: %1").arg(error));
        color = AppStyle::SoftErrorRed;
    } else if (!jsonPath.isEmpty() && !QFileInfo::exists(jsonPath)) {
        details << "提示: 点击刷新模型详情可同步元数据";
    }

    if (!details.isEmpty()) {
        status += "\n" + details.join(" · ");
    }
    ui->lblLocalMetaStatus->setWordWrap(true);
    ui->lblLocalMetaStatus->setText(status);
    ui->lblLocalMetaStatus->setStyleSheet(QString("color: %1;").arg(color));
    ui->lblLocalMetaStatus->setToolTip(status);
}

void MainWindow::refreshCurrentDetailCacheStatus()
{
    if (!currentMeta.filePath.isEmpty()) setLocalMetaStatus(currentMeta);
}

void MainWindow::refreshUsageAnalysisWidget()
{
    if (!usageAnalysisWidget || !ui || !ui->modelList) return;

    UsageAnalysisData data;
    data.galleryImageCount = imageCache.size();
    data.generatedAt = QDateTime::currentDateTime();

    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!isModelListItem(item)) continue;

        UsageAnalysisModel model;
        model.filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
        model.displayName = item->text();
        model.baseName = item->data(ROLE_MODEL_NAME).toString();
        if (model.baseName.isEmpty()) model.baseName = QFileInfo(model.filePath).completeBaseName();
        model.rootName = item->data(ROLE_MODEL_ROOT_NAME).toString();
        model.baseModel = item->data(ROLE_FILTER_BASE).toString();
        model.previewPath = item->data(ROLE_PREVIEW_PATH).toString();
        model.modelId = item->data(ROLE_CIVITAI_MODEL_ID).toInt();
        model.versionId = item->data(ROLE_CIVITAI_VERSION_ID).toInt();
        model.hasSha256 = !item->data(ROLE_CIVITAI_SHA256).toString().trimmed().isEmpty();
        model.localEdited = item->data(ROLE_LOCAL_EDITED).toBool();
        model.usageCount = item->data(ROLE_SORT_USAGE_COUNT).toInt();
        model.lastUsed = item->data(ROLE_SORT_LAST_USED).toLongLong();
        if (!model.filePath.isEmpty()) {
            QFileInfo fi(model.filePath);
            model.jsonPath = fi.absoluteDir().filePath(fi.completeBaseName() + ".json");
            const QJsonObject failure = modelSyncFailures.value(model.filePath);
            model.syncFailure = failure["error"].toString();
        }
        data.models.append(model);
    }

    for (auto it = imageCache.cbegin(); it != imageCache.cend(); ++it) {
        for (const QString &tag : it.value().cleanTags) {
            const QString clean = tag.trimmed();
            if (!clean.isEmpty()) data.positiveTagCounts[clean] += 1;
        }
    }

    usageAnalysisWidget->setAnalysisData(data);
}

QString MainWindow::currentEditBaseName() const
{
    if (QListWidgetItem *item = ui->modelList->currentItem()) {
        QString baseName = item->data(ROLE_MODEL_NAME).toString();
        if (!baseName.isEmpty()) return baseName;
        return item->text();
    }
    if (!currentMeta.filePath.isEmpty()) {
        return QFileInfo(currentMeta.filePath).completeBaseName();
    }
    return QString();
}

QString MainWindow::currentEditModelDir() const
{
    if (QListWidgetItem *item = ui->modelList->currentItem()) {
        QString filePath = item->data(ROLE_FILE_PATH).toString();
        if (!filePath.isEmpty()) return QFileInfo(filePath).absolutePath();
    }
    if (!currentMeta.filePath.isEmpty()) {
        return QFileInfo(currentMeta.filePath).absolutePath();
    }
    return currentLoraPath;
}

QString MainWindow::editPreviewPathForIndex(int index) const
{
    QString modelDir = currentEditModelDir();
    QString baseName = currentEditBaseName();
    if (modelDir.isEmpty() || baseName.isEmpty()) return QString();
    return findLocalPreviewPath(modelDir, baseName, currentMeta.fileNameServer, index);
}

bool MainWindow::saveImageToPreviewPath(const QString &srcPath, const QString &destPath, int &outW, int &outH)
{
    outW = 0;
    outH = 0;
    if (srcPath.isEmpty() || destPath.isEmpty()) return false;

    QImage img(srcPath);
    if (img.isNull()) return false;

    outW = img.width();
    outH = img.height();

    QFileInfo destInfo(destPath);
    QDir().mkpath(destInfo.absolutePath());
    if (QFile::exists(destPath)) QFile::remove(destPath);
    return img.save(destPath, "PNG");
}

void MainWindow::applyParametersToImage(const QString &params, ImageInfo &img)
{
    if (params.isEmpty()) return;

    QRegularExpression reSteps("Steps:\\s*(\\d+)", QRegularExpression::CaseInsensitiveOption);
    QRegularExpression reSampler("Sampler:\\s*([^,]+)", QRegularExpression::CaseInsensitiveOption);
    QRegularExpression reCfg("CFG\\s*scale:\\s*([0-9.]+)", QRegularExpression::CaseInsensitiveOption);
    QRegularExpression reCfgAlt("CFG:\\s*([0-9.]+)", QRegularExpression::CaseInsensitiveOption);
    QRegularExpression reSeed("Seed:\\s*([-0-9]+)", QRegularExpression::CaseInsensitiveOption);

    QRegularExpressionMatch m;
    m = reSteps.match(params);
    if (m.hasMatch()) img.steps = m.captured(1).trimmed();
    m = reSampler.match(params);
    if (m.hasMatch()) img.sampler = m.captured(1).trimmed();
    m = reCfg.match(params);
    if (m.hasMatch()) img.cfgScale = m.captured(1).trimmed();
    else {
        m = reCfgAlt.match(params);
        if (m.hasMatch()) img.cfgScale = m.captured(1).trimmed();
    }
    m = reSeed.match(params);
    if (m.hasMatch()) img.seed = m.captured(1).trimmed();
}

void MainWindow::applyImageMetadataFromFile(const QString &srcPath, ImageInfo &img)
{
    if (srcPath.isEmpty()) return;
    UserImageInfo info;
    parsePngInfo(srcPath, info);
    if (!info.prompt.isEmpty()) img.prompt = info.prompt;
    if (!info.negativePrompt.isEmpty() && info.negativePrompt != "(empty)") {
        img.negativePrompt = info.negativePrompt;
    }
    if (!info.parameters.isEmpty()) {
        applyParametersToImage(info.parameters, img);
    }
}

void MainWindow::refreshEditImages(const ModelMeta &meta)
{
    editImagesNeedRefresh = false;
    ++editImageLoadToken;
    const int currentToken = editImageLoadToken;

    ui->listEditImages->clear();
    currentEditImageIndex = -1;

    QString baseName = currentEditBaseName();
    QString modelDir = currentEditModelDir();

    QList<ImageInfo> images = meta.images;
    if (images.isEmpty()) {
        QString coverPath = findLocalPreviewPath(modelDir, baseName, meta.fileNameServer, 0);
        if (QFile::exists(coverPath)) {
            QImageReader reader(coverPath);
            ImageInfo img;
            img.width = reader.size().width();
            img.height = reader.size().height();
            img.nsfwLevel = 1;
            images.append(img);
            currentMeta.images = images;
        }
    }

    for (int i = 0; i < images.size(); ++i) {
        QString title = (i == 0) ? "封面 / Cover" : QString("预览 %1").arg(i);
        QListWidgetItem *item = new QListWidgetItem(title);
        item->setData(Qt::UserRole, i);
        item->setData(ROLE_EDIT_IMAGE_PATH, QString());
        item->setTextAlignment(Qt::AlignHCenter);
        item->setSizeHint(QSize(114, 182));

        QString path = findLocalPreviewPath(modelDir, baseName, meta.fileNameServer, i);
        if (QFile::exists(path)) {
            item->setData(ROLE_EDIT_IMAGE_PATH, path);
            item->setIcon(placeholderIcon);
            item->setText(title);
            item->setToolTip(title + " (Loading...)");

            QString taskId = QString("EDITIMG|%1|%2|%3").arg(currentToken).arg(i).arg(path);
            IconLoaderTask *task = new IconLoaderTask(path, 100, 0, this, taskId, true);
            task->setAutoDelete(true);
            threadPool->start(task);
        } else if (i < meta.images.size() && !meta.images[i].url.isEmpty()) {
            item->setText(title);
            item->setToolTip(title + " (Remote)");
        } else {
            item->setText(title);
            item->setToolTip(title + " (Missing)");
        }

        ui->listEditImages->addItem(item);
    }

    if (ui->listEditImages->count() > 0) {
        ui->listEditImages->setCurrentRow(0);
    } else {
        ui->lblEditImagePath->setText("文件: -");
        ui->lblEditImageSize->setText("尺寸: -");
        ui->textEditImgPrompt->clear();
        ui->textEditImgNegPrompt->clear();
        ui->editImgSampler->clear();
        ui->editImgSteps->clear();
        ui->editImgCfg->clear();
        ui->editImgSeed->clear();
        ui->spinImgNsfw->setValue(1);
    }
}

void MainWindow::commitEditImageFields()
{
    if (currentEditImageIndex < 0 || currentEditImageIndex >= currentMeta.images.size()) return;
    ImageInfo &img = currentMeta.images[currentEditImageIndex];

    img.prompt = ui->textEditImgPrompt->toPlainText().trimmed();
    img.negativePrompt = ui->textEditImgNegPrompt->toPlainText().trimmed();
    img.sampler = ui->editImgSampler->text().trimmed();
    img.steps = ui->editImgSteps->text().trimmed();
    img.cfgScale = ui->editImgCfg->text().trimmed();
    img.seed = ui->editImgSeed->text().trimmed();
    img.nsfwLevel = ui->spinImgNsfw->value();
    img.nsfw = (img.nsfwLevel > 1);
}

void MainWindow::loadEditImageFields(int index)
{
    if (index < 0 || index >= currentMeta.images.size()) return;
    const ImageInfo &img = currentMeta.images[index];

    QString path = editPreviewPathForIndex(index);
    ui->lblEditImagePath->setText(
        path.isEmpty()
        ? "文件: -"
        : "文件: " + forceWrap(path)
    );
    if (img.width > 0 && img.height > 0) {
        ui->lblEditImageSize->setText(QString("尺寸: %1 x %2").arg(img.width).arg(img.height));
    } else {
        ui->lblEditImageSize->setText("尺寸: -");
    }

    ui->textEditImgPrompt->setPlainText(img.prompt);
    ui->textEditImgNegPrompt->setPlainText(img.negativePrompt);
    ui->editImgSampler->setText(img.sampler);
    ui->editImgSteps->setText(img.steps);
    ui->editImgCfg->setText(img.cfgScale);
    ui->editImgSeed->setText(img.seed);
    ui->spinImgNsfw->setValue(img.nsfwLevel > 0 ? img.nsfwLevel : 1);
}

void MainWindow::onEditImageSelectionChanged(int row)
{
    if (row == currentEditImageIndex) return;
    commitEditImageFields();
    currentEditImageIndex = row;
    loadEditImageFields(row);
}

int MainWindow::countLocalEditedModels() const
{
    int count = 0;
    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!isModelListItem(item)) continue;
        if (item->data(ROLE_LOCAL_EDITED).toBool()) count++;
    }
    return count;
}

bool MainWindow::confirmLocalEditOverwrite(QListWidgetItem *item)
{
    if (!item) return true;
    if (!item->data(ROLE_LOCAL_EDITED).toBool()) return true;

    QMessageBox::StandardButton reply = QMessageBox::warning(
        this,
        "本地元数据提示",
        "当前模型存在本地/已编辑元数据，继续同步将覆盖本地修改。\n是否继续？",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );
    return reply == QMessageBox::Yes;
}

QString MainWindow::modelSyncFailurePath() const
{
    return qApp->applicationDirPath() + "/config/model_sync_failures.json";
}

void MainWindow::loadModelSyncFailures()
{
    modelSyncFailures.clear();
    QFile file(modelSyncFailurePath());
    if (!file.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const QString path = QFileInfo(it.key()).absoluteFilePath();
        if (!path.isEmpty()) modelSyncFailures.insert(path, it.value().toObject());
    }
}

QString MainWindow::modelUserNotesPath() const
{
    return qApp->applicationDirPath() + "/config/model_user_notes.json";
}

QStringList MainWindow::normalizeModelUserTags(const QStringList &tags) const
{
    QStringList result;
    QSet<QString> seen;
    for (const QString &raw : tags) {
        const QString tag = raw.trimmed();
        if (tag.isEmpty()) continue;
        const QString key = tag.toCaseFolded();
        if (seen.contains(key)) continue;
        seen.insert(key);
        result.append(tag);
    }
    return result;
}

QStringList MainWindow::normalizeModelUserTagsText(const QString &text) const
{
    QString normalizedText = text;
    normalizedText.replace('\r', ',');
    normalizedText.replace('\n', ',');
    return normalizeModelUserTags(normalizedText.split(',', Qt::SkipEmptyParts));
}

QStringList MainWindow::normalizeModelCustomTriggers(const QStringList &triggers) const
{
    QStringList result;
    QSet<QString> seen;
    for (const QString &raw : triggers) {
        const QString trigger = raw.trimmed();
        if (trigger.isEmpty()) continue;
        const QString key = trigger.toCaseFolded();
        if (seen.contains(key)) continue;
        seen.insert(key);
        result.append(trigger);
    }
    return result;
}

void MainWindow::loadModelUserNotes()
{
    modelUserNotes.clear();

    QFile file(modelUserNotesPath());
    if (!file.open(QIODevice::ReadOnly)) return;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return;

    const QJsonObject root = doc.object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const QString filePath = QFileInfo(it.key()).absoluteFilePath();
        const QJsonObject obj = it.value().toObject();
        ModelUserNote note;
        note.rating = obj.value("rating").toDouble(0.0);
        if (note.rating < 0.5) note.rating = 0.0;
        if (note.rating > 5.0) note.rating = 5.0;
        note.rating = std::round(note.rating * 2.0) / 2.0;
        note.note = obj.value("note").toString();
        note.updatedAt = obj.value("updatedAt").toString();
        QStringList tags;
        const QJsonArray arr = obj.value("tags").toArray();
        for (const QJsonValue &val : arr) tags.append(val.toString());
        note.tags = normalizeModelUserTags(tags);
        QStringList customTriggers;
        const QJsonArray triggerArr = obj.value("customTriggers").toArray();
        for (const QJsonValue &val : triggerArr) customTriggers.append(val.toString());
        note.customTriggers = normalizeModelCustomTriggers(customTriggers);
        if (!filePath.isEmpty()) modelUserNotes.insert(filePath, note);
    }
}

void MainWindow::saveModelUserNotes() const
{
    const QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);

    QJsonObject root;
    for (auto it = modelUserNotes.cbegin(); it != modelUserNotes.cend(); ++it) {
        const ModelUserNote &note = it.value();
        if (note.rating <= 0.0 && note.note.trimmed().isEmpty() && note.tags.isEmpty() && note.customTriggers.isEmpty()) continue;

        QJsonObject obj;
        obj["rating"] = note.rating;
        obj["note"] = note.note;
        obj["updatedAt"] = note.updatedAt;
        QJsonArray tags;
        for (const QString &tag : note.tags) tags.append(tag);
        obj["tags"] = tags;
        QJsonArray triggers;
        for (const QString &trigger : note.customTriggers) triggers.append(trigger);
        obj["customTriggers"] = triggers;
        root.insert(it.key(), obj);
    }

    QSaveFile file(modelUserNotesPath());
    if (!file.open(QIODevice::WriteOnly)) return;
    file.write(QJsonDocument(root).toJson());
    file.commit();
}

QString MainWindow::formatModelRating(double rating) const
{
    if (rating <= 0.0) return "未评分";
    return QString("%1 / 5").arg(rating, 0, 'f', (std::fmod(rating, 1.0) == 0.0) ? 0 : 1);
}

QString MainWindow::formatModelUserNoteTooltip(const QString &filePath, const QString &baseTooltip) const
{
    QStringList lines;
    if (!baseTooltip.trimmed().isEmpty()) lines << baseTooltip.trimmed();

    const QString key = QFileInfo(filePath).absoluteFilePath();
    const ModelUserNote note = modelUserNotes.value(key);
    if (note.rating > 0.0) lines << "评分: " + formatModelRating(note.rating);
    if (!note.tags.isEmpty()) lines << "标签: " + note.tags.join(", ");
    if (!note.customTriggers.isEmpty()) lines << "自定义触发词: " + note.customTriggers.join(" | ");
    QString noteText = note.note.simplified();
    if (!noteText.isEmpty()) {
        if (noteText.size() > 120) noteText = noteText.left(117) + "...";
        lines << "备注: " + noteText;
    }
    return lines.join("\n");
}

void MainWindow::applyModelUserNoteData(QListWidgetItem *item)
{
    if (!item || !isModelListItem(item)) return;
    const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
    const ModelUserNote note = modelUserNotes.value(filePath);
    item->setData(ROLE_USER_RATING, note.rating);
    item->setData(ROLE_USER_NOTE, note.note);
    item->setData(ROLE_USER_TAGS, note.tags);
    item->setData(ROLE_USER_CUSTOM_TRIGGERS, note.customTriggers);
    item->setToolTip(formatModelUserNoteTooltip(filePath, filePath));
}

void MainWindow::applyModelUserNoteData(QTreeWidgetItem *item)
{
    if (!item || item->data(0, ROLE_FILE_PATH).toString().isEmpty()) return;
    const QString filePath = QFileInfo(item->data(0, ROLE_FILE_PATH).toString()).absoluteFilePath();
    const ModelUserNote note = modelUserNotes.value(filePath);
    item->setData(0, ROLE_USER_RATING, note.rating);
    item->setData(0, ROLE_USER_NOTE, note.note);
    item->setData(0, ROLE_USER_TAGS, note.tags);
    item->setData(0, ROLE_USER_CUSTOM_TRIGGERS, note.customTriggers);
    item->setToolTip(0, formatModelUserNoteTooltip(filePath, item->text(0)));
}

void MainWindow::refreshModelUserNoteItems(const QString &filePath)
{
    const QString key = QFileInfo(filePath).absoluteFilePath();
    auto refreshList = [&](QListWidget *list) {
        if (!list) return;
        for (int i = 0; i < list->count(); ++i) {
            QListWidgetItem *item = list->item(i);
            if (!item) continue;
            if (QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath() == key) {
                applyModelUserNoteData(item);
            }
        }
    };
    refreshList(ui->modelList);
    refreshList(ui->homeGalleryList);
}

void MainWindow::refreshModelUserNotePanel(const QString &filePath)
{
    const QString sourcePath = filePath.isNull() ? currentMeta.filePath : filePath;
    const QString key = sourcePath.isEmpty() ? QString() : QFileInfo(sourcePath).absoluteFilePath();
    const ModelUserNote note = modelUserNotes.value(key);

    ui->lblUserRating->setText("评分: " + formatModelRating(note.rating));
    ui->textUserNotePreview->setPlainText(note.note);
    ui->textUserNotePreview->setPlaceholderText("暂无备注 / No note.");

    clearLayout(ui->layoutUserTags);
    if (note.tags.isEmpty()) {
        QLabel *empty = new QLabel("无用户标签");
        empty->setStyleSheet(AppStyle::MutedLabelStyle);
        ui->layoutUserTags->addWidget(empty);
    } else {
        for (const QString &tag : note.tags) {
            QPushButton *button = new QPushButton(forceWrap(tag));
            button->setProperty("class", "filterChip");
            button->setProperty("tagSource", "user");
            button->setCursor(Qt::PointingHandCursor);
            button->setToolTip("点击在主页添加该用户 Tag 筛选");
            button->setMaximumWidth(150);
            button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
            connect(button, &QPushButton::clicked, this, [this, tag]() {
                ui->rootStack->setCurrentWidget(ui->pageLibrary);
                ui->mainStack->setCurrentWidget(ui->pageHome);
                addHomeTagFilter(tag);
                ui->statusbar->showMessage("已添加用户 Tag 筛选: " + tag, 2000);
            });
            ui->layoutUserTags->addWidget(button);
        }
    }
    ui->layoutUserTags->addStretch();
}

void MainWindow::refreshModelAttributionPanel(const ModelMeta &meta)
{
    if (!ui || !ui->layoutModelAuthor || !ui->layoutModelTags) return;

    clearLayout(ui->layoutModelAuthor);
    clearLayout(ui->layoutModelTags);

    if (meta.creatorName.trimmed().isEmpty()) {
        QLabel *empty = new QLabel("暂无作者信息");
        empty->setStyleSheet(AppStyle::MutedLabelStyle);
        ui->layoutModelAuthor->addWidget(empty);
    } else {
        QPushButton *author = new QPushButton(forceWrap(meta.creatorName));
        author->setProperty("class", "filterChip");
        author->setCursor(Qt::PointingHandCursor);
        author->setToolTip("点击在主页筛选该作者");
        author->setMaximumWidth(260);
        author->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        connect(author, &QPushButton::clicked, this, [this, name = meta.creatorName]() {
            ui->rootStack->setCurrentWidget(ui->pageLibrary);
            ui->mainStack->setCurrentWidget(ui->pageHome);
            setHomeAuthorFilter(name);
            ui->statusbar->showMessage("已按作者筛选: " + name, 2000);
        });
        ui->layoutModelAuthor->addWidget(author);
    }
    ui->layoutModelAuthor->addStretch();

    if (meta.modelTags.isEmpty()) {
        QLabel *empty = new QLabel("暂无模型标签");
        empty->setStyleSheet(AppStyle::MutedLabelStyle);
        ui->layoutModelTags->addWidget(empty);
    } else {
        QWidget *tagFlowWidget = new QWidget();
        tagFlowWidget->setStyleSheet("background:transparent;");
        tagFlowWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        FlowLayout *tagFlowLayout = new FlowLayout(tagFlowWidget, 0, 6, 6);
        for (const QString &tag : meta.modelTags) {
            QPushButton *tagButton = new QPushButton(forceWrap(tag));
            tagButton->setProperty("class", "filterChip");
            tagButton->setCursor(Qt::PointingHandCursor);
            tagButton->setToolTip("点击在主页添加该 Tag 筛选");
            tagButton->setMaximumWidth(150);
            tagButton->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
            connect(tagButton, &QPushButton::clicked, this, [this, tag]() {
                ui->rootStack->setCurrentWidget(ui->pageLibrary);
                ui->mainStack->setCurrentWidget(ui->pageHome);
                addHomeTagFilter(tag);
                ui->statusbar->showMessage("已添加 Tag 筛选: " + tag, 2000);
            });
            tagFlowLayout->addWidget(tagButton);
        }
        ui->layoutModelTags->addWidget(tagFlowWidget);
    }
}

void MainWindow::openModelNoteDialog(QListWidgetItem *item)
{
    if (!isModelListItem(item)) return;
    const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
    if (filePath.isEmpty()) return;

    ModelUserNote current = modelUserNotes.value(filePath);
    ModelNoteDialog dialog(this);
    dialog.setModelName(item->text());
    dialog.setRating(current.rating);
    dialog.setNote(current.note);
    dialog.setTags(current.tags);
    dialog.setCustomTriggers(current.customTriggers);
    if (dialog.exec() != QDialog::Accepted) return;

    current.rating = dialog.rating();
    if (current.rating < 0.5) current.rating = 0.0;
    current.rating = std::round(current.rating * 2.0) / 2.0;
    current.note = dialog.note();
    current.tags = normalizeModelUserTags(dialog.tags());
    current.customTriggers = normalizeModelCustomTriggers(dialog.customTriggers());
    current.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    if (current.rating <= 0.0 && current.note.isEmpty() && current.tags.isEmpty() && current.customTriggers.isEmpty()) {
        modelUserNotes.remove(filePath);
    } else {
        modelUserNotes.insert(filePath, current);
    }

    refreshModelUserNoteItems(filePath);
    saveModelUserNotes();
    refreshModelUserNotePanel(filePath);
    if (QFileInfo(currentMeta.filePath).absoluteFilePath() == filePath) {
        refreshTriggerWordsPanel(currentMeta);
    }
    executeSort();
    refreshCollectionTreeView();
    refreshHomeGallery();
    if (QListWidgetItem *restoredItem = findModelItemByFilePath(filePath)) {
        ui->modelList->setCurrentItem(restoredItem);
        restoredItem->setSelected(true);
        syncTreeSelection(filePath);
    }
    ui->statusbar->showMessage("模型评分、备注与自定义触发词已保存。", 2000);
}

void MainWindow::setUserRatingForItems(const QList<QListWidgetItem*> &items, double rating)
{
    const double normalizedRating = rating < 0.5 ? 0.0 : qBound(0.0, std::round(rating * 2.0) / 2.0, 5.0);
    for (QListWidgetItem *item : items) {
        if (!isModelListItem(item)) continue;
        const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
        if (filePath.isEmpty()) continue;
        ModelUserNote note = modelUserNotes.value(filePath);
        note.rating = normalizedRating;
        note.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        if (note.rating <= 0.0 && note.note.isEmpty() && note.tags.isEmpty() && note.customTriggers.isEmpty()) {
            modelUserNotes.remove(filePath);
        } else {
            modelUserNotes.insert(filePath, note);
        }
        refreshModelUserNoteItems(filePath);
    }
    saveModelUserNotes();
    refreshModelUserNotePanel();
    executeSort();
    refreshCollectionTreeView();
    refreshHomeGallery();
}

void MainWindow::addUserTagsForItems(const QList<QListWidgetItem*> &items, const QStringList &tags)
{
    const QStringList cleanTags = normalizeModelUserTags(tags);
    if (cleanTags.isEmpty()) return;
    for (QListWidgetItem *item : items) {
        if (!isModelListItem(item)) continue;
        const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
        if (filePath.isEmpty()) continue;
        ModelUserNote note = modelUserNotes.value(filePath);
        note.tags = normalizeModelUserTags(note.tags + cleanTags);
        note.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        modelUserNotes.insert(filePath, note);
        refreshModelUserNoteItems(filePath);
    }
    saveModelUserNotes();
    refreshModelUserNotePanel();
    refreshCollectionTreeView();
    refreshHomeGallery();
}

void MainWindow::removeUserTagsForItems(const QList<QListWidgetItem*> &items, const QStringList &tags)
{
    const QStringList cleanTags = normalizeModelUserTags(tags);
    if (cleanTags.isEmpty()) return;
    QSet<QString> removeSet;
    for (const QString &tag : cleanTags) removeSet.insert(tag.toCaseFolded());
    for (QListWidgetItem *item : items) {
        if (!isModelListItem(item)) continue;
        const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
        if (filePath.isEmpty()) continue;
        ModelUserNote note = modelUserNotes.value(filePath);
        QStringList nextTags;
        for (const QString &tag : note.tags) {
            if (!removeSet.contains(tag.toCaseFolded())) nextTags.append(tag);
        }
        note.tags = nextTags;
        note.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        if (note.rating <= 0.0 && note.note.isEmpty() && note.tags.isEmpty() && note.customTriggers.isEmpty()) {
            modelUserNotes.remove(filePath);
        } else {
            modelUserNotes.insert(filePath, note);
        }
        refreshModelUserNoteItems(filePath);
    }
    saveModelUserNotes();
    refreshModelUserNotePanel();
    refreshCollectionTreeView();
    refreshHomeGallery();
}

void MainWindow::saveModelSyncFailures() const
{
    const QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);
    QSaveFile file(modelSyncFailurePath());
    if (!file.open(QIODevice::WriteOnly)) return;
    QJsonObject root;
    for (auto it = modelSyncFailures.cbegin(); it != modelSyncFailures.cend(); ++it) {
        root.insert(QFileInfo(it.key()).absoluteFilePath(), it.value());
    }
    file.write(QJsonDocument(root).toJson());
    file.commit();
}

void MainWindow::recordModelSyncFailure(const QString &filePath, const QString &baseName, const QString &error)
{
    const QString key = QFileInfo(filePath).absoluteFilePath();
    if (key.isEmpty()) return;
    QJsonObject obj;
    obj["filePath"] = key;
    obj["baseName"] = baseName;
    obj["error"] = error;
    obj["failedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    modelSyncFailures.insert(key, obj);
    saveModelSyncFailures();
    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (item && QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath() == key) {
            item->setData(ROLE_SYNC_FAILED, true);
            item->setData(ROLE_SYNC_ERROR, error);
            break;
        }
    }
}

void MainWindow::clearModelSyncFailure(const QString &filePath)
{
    const QString key = QFileInfo(filePath).absoluteFilePath();
    if (key.isEmpty()) return;
    if (modelSyncFailures.remove(key) > 0) saveModelSyncFailures();
    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (item && QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath() == key) {
            item->setData(ROLE_SYNC_FAILED, false);
            item->setData(ROLE_SYNC_ERROR, QString());
            break;
        }
    }
}

QString MainWindow::modelSyncFailureMessage(const QString &filePath) const
{
    const QString key = QFileInfo(filePath).absoluteFilePath();
    const QJsonObject obj = modelSyncFailures.value(key);
    return obj.value("error").toString();
}

void MainWindow::onScanLocalClicked() {
    int localCount = countLocalEditedModels();
    if (!optSuppressLocalWarnings && localCount > 0) {
        QMessageBox::information(this, "提示",
                                 QString("检测到 %1 个本地/已编辑模型。\n刷新不会删除本地元数据，但后续同步可能覆盖本地修改。").arg(localCount));
    }
    const QStringList activeLoraPaths = collectEnabledPaths(loraPaths, disabledLoraPaths);
    if (!activeLoraPaths.isEmpty()) {
        scanModels(activeLoraPaths);
    } else {
        QMessageBox::information(this, "提示", "请先设置并启用至少一个 LoRA 路径。");
    }
    executeSort();
}

// 点击列表项
void MainWindow::onModelListClicked(QListWidgetItem *item) {
    if (item && item->data(ROLE_IS_FOLDER_HEADER).toBool()) {
        toggleModelFolderCollapsed(item->data(ROLE_MODEL_FOLDER_KEY).toString());
        return;
    }
    if (!isModelListItem(item)) return;

    cancelPendingTasks();

    // === 恢复 UI 状态 ===
    ui->btnForceUpdate->setVisible(true);
    ui->btnFavorite->setVisible(true);
    ui->btnShowUserGallery->setVisible(true);
    ui->btnShowUserGallery->setEnabled(true);
    ui->btnEditMeta->setVisible(true);
    ui->btnCopyLoraTag->setVisible(true);

    QString filePath = item->data(ROLE_FILE_PATH).toString();
    QString modelDir = QFileInfo(filePath).absolutePath();
    ui->modelList->setProperty("current_model_dir", modelDir);

    if (currentMeta.filePath == filePath && !currentMeta.name.isEmpty()) {
        // 如果当前不在详情页（比如在主页），则只切换页面，不重新加载数据
        if (ui->mainStack->currentIndex() != 1) {
            ui->mainStack->setCurrentIndex(1);
        }
        QTimer::singleShot(0, this, &MainWindow::fitDetailContentToCurrentPage);
        // 无论是否切换了页面，都直接返回，不再执行后续繁重的 JSON 解析
        return;
    }

    // 1. 如果正在计算上一个，先取消或忽略
    if (hashWatcher->isRunning()) {
        // 简单处理：提示用户稍等，或者强制让 UI 变动
        // 更好的做法是 cancel，但 SHA 计算很难中途 cancel，所以我们用标志位判断
    }

    if (ui->mainStack->currentIndex() != 1) {
        ui->mainStack->setCurrentIndex(1); // 立即进入详情页
    }

    QString previewPath = item->data(ROLE_PREVIEW_PATH).toString();
    QString baseName = item->data(ROLE_MODEL_NAME).toString();

    ModelMeta meta;
    meta.modelName = baseName;
    meta.versionName = "";
    meta.name = baseName;
    meta.filePath = filePath;
    meta.previewPath = previewPath;
    meta.fileName = QFileInfo(filePath).fileName();

    // 2. 尝试读取本地 JSON
    bool hasLocalData = readLocalJson(modelDir, baseName, meta);

    if (hasLocalData) {
        // === 情况 A: 有本地数据，直接显示 (秒开) ===
        clearModelSyncFailure(filePath);
        currentMeta = meta;
        setModelTitleNormal();
        ui->scrollAreaWidgetContents->setUpdatesEnabled(false);
        updateDetailView(meta);
        QTimer::singleShot(0, this, [this]() {
            ui->scrollAreaWidgetContents->setUpdatesEnabled(true);
            ui->scrollAreaWidgetContents->update();
        });
    } else {
        meta.isLocalOnly = true;
        meta.isLocalEdited = false;
        const QString failedMessage = modelSyncFailureMessage(filePath);
        if (!failedMessage.isEmpty()) {
            showPendingLocalModelDetail(
                meta,
                QString("上次同步失败，请点击刷新模型详情重新同步。\n%1").arg(failedMessage)
            );
            ui->btnForceUpdate->setEnabled(true);
            if (ui->detailContentStack->currentIndex() == 1) {
                scanForUserImages(baseName);
            } else {
                ui->listUserImages->clear();
                ui->textUserPrompt->clear();
                tagFlowWidget->setData({});
            }
            return;
        }

        showPendingLocalModelDetail(meta, "正在分析模型文件 (计算 Hash)...");

        // === 情况 B: 无本地数据，需要计算 Hash 然后联网 ===

        // UI 状态反馈：显示“正在分析模型...”
        ui->lblModelName->setText("正在分析模型文件 (计算 Hash)...");
        ui->btnForceUpdate->setEnabled(false);
        startModelHashSync(filePath, baseName, false);
    }

    // 如果当前正处于 "本地返图" 页面 (Index 1)，立即刷新数据
    if (ui->detailContentStack->currentIndex() == 1) {
        // 使用当前选中的模型名进行扫描
        scanForUserImages(baseName);
    } else {
        // 如果在主详情页，先清空返图页的旧数据，防止用户等会儿切过去看到上一个模型的图
        ui->listUserImages->clear();
        ui->textUserPrompt->clear();
        tagFlowWidget->setData({});
    }
}

// 强制联网
void MainWindow::onForceUpdateClicked() {
    QListWidgetItem *item = ui->modelList->currentItem();
    if (!item) return;

    if (!confirmLocalEditOverwrite(item)) return;
    int localCount = countLocalEditedModels();
    if (!optSuppressLocalWarnings && !item->data(ROLE_LOCAL_EDITED).toBool() && localCount > 0) {
        QMessageBox::information(this, "提示",
                                 QString("检测到 %1 个本地/已编辑模型。\n同步其它模型的元数据不会影响它们，但强制更新时请注意覆盖风险。").arg(localCount));
    }

    ui->btnForceUpdate->setEnabled(false);

    QString baseName = item->data(ROLE_MODEL_NAME).toString();
    if (baseName.isEmpty()) baseName = item->text();
    QString filePath = item->data(ROLE_FILE_PATH).toString();
    ui->modelList->setProperty("current_processing_path", filePath);

    QString modelDir = ui->modelList->property("current_model_dir").toString();
    bool hasRemotePreview = false;
    if (!modelDir.isEmpty()) {
        QString jsonPath = QDir(modelDir).filePath(baseName + ".json");
        QFile jsonFile(jsonPath);
        if (jsonFile.exists() && jsonFile.open(QIODevice::ReadOnly)) {
            QJsonDocument doc = QJsonDocument::fromJson(jsonFile.readAll());
            QJsonObject root = doc.object();
            QJsonArray images = root["images"].toArray();
            if (!images.isEmpty()) {
                QString url = images[0].toObject()["url"].toString();
                if (!url.isEmpty()) hasRemotePreview = true;
            }
            jsonFile.close();
        }
    }
    if (!hasRemotePreview && !currentMeta.images.isEmpty() && !currentMeta.images[0].url.isEmpty()) {
        hasRemotePreview = true;
    }
    QString previewPath = findLocalPreviewPath(modelDir, baseName, currentMeta.fileNameServer, 0);
    bool hasLocalPreview = !previewPath.isEmpty() && QFile::exists(previewPath);
    if (hasRemotePreview && hasLocalPreview) {
        QMessageBox msg(this);
        msg.setWindowTitle("预览图同步");
        msg.setText("检测到已同步的 Civitai 预览图，是否重新同步预览图？");
        QPushButton *btnYes = msg.addButton("是", QMessageBox::YesRole);
        QPushButton *btnMetaOnly = msg.addButton("仅元数据", QMessageBox::AcceptRole);
        QPushButton *btnCancel = msg.addButton("取消同步", QMessageBox::RejectRole);
        msg.setDefaultButton(btnMetaOnly);
        msg.exec();

        if (msg.clickedButton() == btnCancel) {
            m_forceResyncPreview = false;
            m_skipPreviewSync = false;
            ui->btnForceUpdate->setEnabled(true);
            ui->statusbar->showMessage("已取消同步", 2000);
            return;
        }

        if (msg.clickedButton() == btnYes) {
            m_forceResyncPreview = true;
            m_skipPreviewSync = false;
        } else if (msg.clickedButton() == btnMetaOnly) {
            m_forceResyncPreview = false;
            m_skipPreviewSync = true;
        } else {
            m_forceResyncPreview = false;
            m_skipPreviewSync = false;
        }
    } else {
        m_forceResyncPreview = false;
        m_skipPreviewSync = false;
    }

    clearModelSyncFailure(filePath);
    ModelMeta meta;
    meta.modelName = baseName;
    meta.name = baseName;
    meta.filePath = filePath;
    meta.fileName = QFileInfo(filePath).fileName();
    meta.previewPath = item->data(ROLE_PREVIEW_PATH).toString();
    meta.isLocalOnly = true;
    showPendingLocalModelDetail(meta, "正在重新同步模型详情 (计算 Hash)...");
    startModelHashSync(filePath, baseName, true);
}

void MainWindow::onLocalMetaSaveClicked()
{
    QListWidgetItem *item = ui->modelList->currentItem();
    if (!item) {
        QMessageBox::information(this, "提示", "请先选择一个模型。");
        return;
    }

    QString baseName = item->data(ROLE_MODEL_NAME).toString();
    QString filePath = item->data(ROLE_FILE_PATH).toString();
    if (baseName.isEmpty() || filePath.isEmpty()) return;

    QString modelDir = QFileInfo(filePath).absolutePath();
    QString jsonPath = QDir(modelDir).filePath(baseName + ".json");

    QJsonObject root;
    QFile jsonFile(jsonPath);
    if (jsonFile.exists() && jsonFile.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(jsonFile.readAll());
        root = doc.object();
        jsonFile.close();
    }

    QString modelName = ui->editLocalModelName->text().trimmed();
    if (modelName.isEmpty()) modelName = baseName;
    QString versionName = ui->editLocalVersion->text().trimmed();
    QString baseModel = ui->editLocalBaseModel->text().trimmed();
    QString type = ui->editLocalType->text().trimmed();
    QString modelUrl = ui->editLocalModelUrl->text().trimmed();
    QString createdAt = ui->editLocalCreatedAt->text().trimmed();
    bool nsfw = ui->chkLocalNSFW->isChecked();
    QString description = ui->textLocalDescription->toPlainText().trimmed();
    bool okDownloads = false;
    int downloads = ui->editLocalDownloads->text().trimmed().toInt(&okDownloads);
    if (!okDownloads) downloads = 0;
    bool okLikes = false;
    int likes = ui->editLocalLikes->text().trimmed().toInt(&okLikes);
    if (!okLikes) likes = 0;
    const QString creatorName = ui->editLocalCreator->text().trimmed();
    auto parseModelTags = [](const QString &text) {
        QStringList tags;
        QSet<QString> seen;
        const QStringList parts = text.split(QRegularExpression("[,\\n\\r]+"), Qt::SkipEmptyParts);
        for (QString tag : parts) {
            tag = tag.trimmed();
            if (tag.isEmpty()) continue;
            const QString key = tag.toCaseFolded();
            if (seen.contains(key)) continue;
            seen.insert(key);
            tags.append(tag);
        }
        return tags;
    };
    const QStringList modelTags = parseModelTags(ui->textLocalModelTags->toPlainText());

    QJsonObject modelObj = root["model"].toObject();
    modelObj["name"] = modelName;
    if (!type.isEmpty()) modelObj["type"] = type;
    else modelObj.remove("type");
    modelObj["nsfw"] = nsfw;
    QJsonObject creatorObj = modelObj.value("creator").toObject();
    QJsonObject topCreatorObj = root.value("creator").toObject();
    if (!creatorName.isEmpty()) {
        creatorObj["username"] = creatorName;
        topCreatorObj["username"] = creatorName;
        modelObj["creator"] = creatorObj;
        root["creator"] = topCreatorObj;
    } else {
        creatorObj.remove("username");
        creatorObj.remove("name");
        topCreatorObj.remove("username");
        topCreatorObj.remove("name");
        if (creatorObj.isEmpty()) modelObj.remove("creator");
        else modelObj["creator"] = creatorObj;
        if (topCreatorObj.isEmpty()) root.remove("creator");
        else root["creator"] = topCreatorObj;
    }
    QJsonArray tagArray;
    for (const QString &tag : modelTags) tagArray.append(tag);
    if (modelTags.isEmpty()) {
        modelObj.remove("tags");
        root.remove("tags");
    } else {
        modelObj["tags"] = tagArray;
        root["tags"] = tagArray;
    }
    root["model"] = modelObj;

    root["name"] = versionName;
    if (!baseModel.isEmpty()) root["baseModel"] = baseModel;
    else root.remove("baseModel");
    root["description"] = description;

    QStringList triggers;
    const QStringList lines = ui->textLocalTriggers->toPlainText().split('\n', Qt::SkipEmptyParts);
    for (QString line : lines) {
        line = line.trimmed();
        if (line.endsWith(",")) line.chop(1);
        if (!line.isEmpty()) triggers.append(line);
    }
    QJsonArray twArray;
    for (const QString &w : triggers) twArray.append(w);
    root["trainedWords"] = twArray;

    root["localEdited"] = true;
    root["localEditedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    int modelId = root["modelId"].toInt();
    if (modelId <= 0) modelId = root["model"].toObject()["id"].toInt();
    if (!modelUrl.isEmpty()) {
        root["modelUrl"] = modelUrl;
        QRegularExpression re("/models/(\\d+)");
        QRegularExpressionMatch match = re.match(modelUrl);
        if (match.hasMatch()) {
            modelId = match.captured(1).toInt();
        }
    } else {
        root.remove("modelUrl");
        modelId = 0;
    }
    root["modelId"] = modelId;

    bool localOnly = root["localOnly"].toBool(false);
    if (modelId <= 0) localOnly = true;
    root["localOnly"] = localOnly;

    QFileInfo fi(filePath);
    double sizeKB = fi.exists() ? fi.size() / 1024.0 : 0.0;
    QJsonArray files = root["files"].toArray();
    QJsonObject fileObj = files.isEmpty() ? QJsonObject() : files[0].toObject();
    if (sizeKB > 0) fileObj["sizeKB"] = sizeKB;
    fileObj["name"] = fi.fileName();
    if (files.isEmpty()) files.append(fileObj);
    else files[0] = fileObj;
    root["files"] = files;

    if (!createdAt.isEmpty()) {
        root["createdAt"] = createdAt;
    } else {
        root.remove("createdAt");
    }

    QJsonObject statsObj = root["stats"].toObject();
    statsObj["downloadCount"] = downloads;
    statsObj["thumbsUpCount"] = likes;
    root["stats"] = statsObj;

    commitEditImageFields();
    QJsonArray imagesArr;
    const QJsonArray oldImagesArr = root["images"].toArray();
    for (int imageIndex = 0; imageIndex < currentMeta.images.size(); ++imageIndex) {
        const ImageInfo &img = currentMeta.images.at(imageIndex);
        QJsonObject imgObj = imageIndex < oldImagesArr.size() ? oldImagesArr.at(imageIndex).toObject() : QJsonObject();
        if (!img.url.isEmpty()) imgObj["url"] = img.url;
        if (!img.hash.isEmpty()) imgObj["hash"] = img.hash;
        if (img.width > 0) imgObj["width"] = img.width;
        if (img.height > 0) imgObj["height"] = img.height;
        if (img.nsfwLevel > 0) imgObj["nsfwLevel"] = img.nsfwLevel;
        QJsonObject imgMeta = imgObj["meta"].toObject();
        if (!img.prompt.isEmpty()) imgMeta["prompt"] = img.prompt;
        else imgMeta.remove("prompt");
        if (!img.negativePrompt.isEmpty()) imgMeta["negativePrompt"] = img.negativePrompt;
        else imgMeta.remove("negativePrompt");
        if (!img.sampler.isEmpty()) imgMeta["sampler"] = img.sampler;
        else imgMeta.remove("sampler");
        bool okSteps = false;
        int steps = img.steps.toInt(&okSteps);
        if (okSteps) imgMeta["steps"] = steps;
        else if (!img.steps.isEmpty()) imgMeta["steps"] = img.steps;
        else imgMeta.remove("steps");
        bool okCfg = false;
        double cfg = img.cfgScale.toDouble(&okCfg);
        if (okCfg) imgMeta["cfgScale"] = cfg;
        else if (!img.cfgScale.isEmpty()) imgMeta["cfgScale"] = img.cfgScale;
        else imgMeta.remove("cfgScale");
        bool okSeed = false;
        qlonglong seed = img.seed.toLongLong(&okSeed);
        if (okSeed) imgMeta["seed"] = seed;
        else if (!img.seed.isEmpty()) imgMeta["seed"] = img.seed;
        else imgMeta.remove("seed");
        if (!imgMeta.isEmpty()) imgObj["meta"] = imgMeta;
        else imgObj.remove("meta");
        imagesArr.append(imgObj);
    }
    root["images"] = imagesArr;

    QFile saveFile(jsonPath);
    if (!saveFile.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, "保存失败", "无法写入本地元数据文件。");
        return;
    }
    saveFile.write(QJsonDocument(root).toJson());
    saveFile.close();

    // 重新读取并刷新详情
    ModelMeta meta;
    meta.filePath = filePath;
    meta.previewPath = item->data(ROLE_PREVIEW_PATH).toString();
    meta.fileName = fi.fileName();
    if (readLocalJson(modelDir, baseName, meta)) {
        currentMeta = meta;
        updateDetailView(meta);
    } else {
        updateLocalEditorFromMeta(meta);
    }

    preloadItemMetadata(item, jsonPath);
    refreshCollectionTreeView();
    refreshHomeFilterChips();
    refreshHomeGallery();
    QString civitaiName = item->data(ROLE_CIVITAI_NAME).toString();
    if (optUseCivitaiName && !civitaiName.isEmpty()) {
        item->setText(civitaiName);
    } else {
        item->setText(baseName);
    }

    ui->statusbar->showMessage("本地元数据已保存。", 2000);
}

void MainWindow::onLocalMetaResetClicked()
{
    QListWidgetItem *item = ui->modelList->currentItem();
    if (!item) return;

    QString baseName = item->data(ROLE_MODEL_NAME).toString();
    QString filePath = item->data(ROLE_FILE_PATH).toString();
    if (baseName.isEmpty() || filePath.isEmpty()) return;

    QString modelDir = QFileInfo(filePath).absolutePath();
    QString jsonPath = QDir(modelDir).filePath(baseName + ".json");

    ModelMeta meta;
    meta.modelName = baseName;
    meta.versionName = "";
    meta.name = baseName;
    meta.filePath = filePath;
    meta.previewPath = item->data(ROLE_PREVIEW_PATH).toString();
    meta.fileName = QFileInfo(filePath).fileName();

    if (QFile::exists(jsonPath) && readLocalJson(modelDir, baseName, meta)) {
        currentMeta = meta;
        updateDetailView(meta);
        ui->statusbar->showMessage("已从本地元数据恢复。", 2000);
    } else {
        updateLocalEditorFromMeta(meta);
        ui->statusbar->showMessage("未找到本地元数据文件，已恢复默认值。", 2000);
    }
}

void MainWindow::onEditMetaTabClicked()
{
    if (!ui->modelList->currentItem()) {
        QMessageBox::information(this, "提示", "请先选择一个模型。");
        return;
    }

    int currentIndex = ui->detailContentStack->currentIndex();
    int targetIndex = (currentIndex == 2) ? 0 : 2;

    ui->scrollAreaWidgetContents->removeEventFilter(this);
    ui->detailContentStack->setCurrentIndex(targetIndex);
    if (targetIndex == 2 && editImagesNeedRefresh) {
        refreshEditImages(currentMeta);
    }

    fitDetailContentToCurrentPage();

    QTimer::singleShot(50, this, [this, targetIndex](){
        ui->scrollAreaWidgetContents->installEventFilter(this);
        if (targetIndex == 0) {
            fitDetailContentToCurrentPage();
        }
        updateBackgroundImage();
    });
}

void MainWindow::onEditAddImageClicked()
{
    if (!ui->modelList->currentItem()) {
        QMessageBox::information(this, "提示", "请先选择一个模型。");
        return;
    }

    commitEditImageFields();

    QString srcPath = QFileDialog::getOpenFileName(this, "选择预览图片",
                                                   currentEditModelDir(),
                                                   "Image Files (*.png *.jpg *.jpeg *.webp)");
    if (srcPath.isEmpty()) return;

    QString baseName = currentEditBaseName();
    QString modelDir = currentEditModelDir();
    int index = currentMeta.images.size();
    QString destPath = findLocalPreviewPath(modelDir, baseName, currentMeta.fileNameServer, index);

    int w = 0, h = 0;
    if (!saveImageToPreviewPath(srcPath, destPath, w, h)) {
        QMessageBox::warning(this, "错误", "图片保存失败，请检查文件格式。");
        return;
    }

    ImageInfo img;
    img.url = "";
    img.hash = "";
    img.width = w;
    img.height = h;
    img.nsfwLevel = 1;
    img.nsfw = false;
    applyImageMetadataFromFile(srcPath, img);
    currentMeta.images.append(img);
    if (index == 0) {
        currentMeta.previewPath = destPath;
    }

    refreshEditImages(currentMeta);
    ui->listEditImages->setCurrentRow(index);
}

void MainWindow::onEditReplaceImageClicked()
{
    int row = ui->listEditImages->currentRow();
    if (row < 0 || row >= currentMeta.images.size()) return;

    commitEditImageFields();

    QString srcPath = QFileDialog::getOpenFileName(this, "替换预览图片",
                                                   currentEditModelDir(),
                                                   "Image Files (*.png *.jpg *.jpeg *.webp)");
    if (srcPath.isEmpty()) return;

    QString destPath = editPreviewPathForIndex(row);
    int w = 0, h = 0;
    if (!saveImageToPreviewPath(srcPath, destPath, w, h)) {
        QMessageBox::warning(this, "错误", "图片保存失败，请检查文件格式。");
        return;
    }

    currentMeta.images[row].width = w;
    currentMeta.images[row].height = h;
    applyImageMetadataFromFile(srcPath, currentMeta.images[row]);
    if (row == 0) {
        currentMeta.previewPath = destPath;
    }

    refreshEditImages(currentMeta);
    ui->listEditImages->setCurrentRow(row);
}

void MainWindow::onEditRemoveImageClicked()
{
    int row = ui->listEditImages->currentRow();
    if (row < 0 || row >= currentMeta.images.size()) return;

    QMessageBox::StandardButton reply = QMessageBox::question(
        this, "删除预览图", "确定删除选中的预览图吗？", QMessageBox::Yes | QMessageBox::No, QMessageBox::No
    );
    if (reply != QMessageBox::Yes) return;

    commitEditImageFields();

    QString pathToRemove = editPreviewPathForIndex(row);
    if (QFile::exists(pathToRemove)) QFile::remove(pathToRemove);

    // 重新编号后续图片文件
    for (int i = row + 1; i < currentMeta.images.size(); ++i) {
        QString oldPath = editPreviewPathForIndex(i);
        QString newPath = editPreviewPathForIndex(i - 1);
        if (QFile::exists(oldPath)) {
            if (QFile::exists(newPath)) QFile::remove(newPath);
            QFile::rename(oldPath, newPath);
        }
    }

    currentMeta.images.removeAt(row);
    if (!currentMeta.images.isEmpty()) {
        currentMeta.previewPath = editPreviewPathForIndex(0);
    } else {
        currentMeta.previewPath.clear();
    }
    refreshEditImages(currentMeta);
    int nextRow = qMin(row, ui->listEditImages->count() - 1);
    if (nextRow >= 0) ui->listEditImages->setCurrentRow(nextRow);
}

void MainWindow::onEditSetCoverClicked()
{
    int row = ui->listEditImages->currentRow();
    if (row <= 0 || row >= currentMeta.images.size()) return;

    commitEditImageFields();

    qSwap(currentMeta.images[0], currentMeta.images[row]);

    QString path0 = editPreviewPathForIndex(0);
    QString pathN = editPreviewPathForIndex(row);
    QString tempPath = path0 + ".swap";

    if (QFile::exists(tempPath)) QFile::remove(tempPath);
    if (QFile::exists(path0)) QFile::rename(path0, tempPath);
    if (QFile::exists(pathN)) QFile::rename(pathN, path0);
    if (QFile::exists(tempPath)) QFile::rename(tempPath, pathN);

    currentMeta.previewPath = editPreviewPathForIndex(0);
    refreshEditImages(currentMeta);
    ui->listEditImages->setCurrentRow(0);
}

QString MainWindow::civitaiApiKey() const
{
    return optCivitaiApiKey.trimmed();
}

bool MainWindow::isCivitaiUrl(const QUrl &url) const
{
    const QString host = url.host().toLower();
    return host == "civitai.com" || host.endsWith(".civitai.com");
}

bool MainWindow::shouldUseCivitaiBearerAuth(const QUrl &url) const
{
    if (!isCivitaiUrl(url)) return false;
    const QString host = url.host().toLower();

    // 图片/CDN 链接通常使用 token query 参数认证，Bearer 在这些 host 上容易触发 401。
    if (host == "image.civitai.com" || host == "imagecache.civitai.com") return false;
    if (host.startsWith("image.") || host.contains("imagecache")) return false;

    return true;
}

QUrl MainWindow::civitaiUrlWithToken(const QUrl &url) const
{
    const QString key = civitaiApiKey();
    if (key.isEmpty() || !isCivitaiUrl(url)) return url;

    QUrl out(url);
    QUrlQuery query(out);
    if (query.hasQueryItem("token")) return out;
    query.addQueryItem("token", key);
    out.setQuery(query);
    return out;
}

QNetworkRequest MainWindow::makeNetworkRequest(const QUrl &url, bool allowCivitaiAuth) const
{
    QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::UserAgentHeader, currentUserAgent);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    #if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        request.setTransferTimeout(30000); // 30秒超时，防止异常挂起导致队列卡死
    #endif
        if (allowCivitaiAuth && isCivitaiUrl(url) && shouldUseCivitaiBearerAuth(url)) {
                const QString key = civitaiApiKey();
            if (!key.isEmpty()) {
                request.setRawHeader("Authorization", QString("Bearer %1").arg(key).toUtf8());
            }
        }
    return request;
}

QString MainWindow::civitaiNetworkErrorMessage(QNetworkReply *reply) const
{
    if (!reply) return "未知网络错误";
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString text = reply->errorString();
    if ((status == 401 || status == 403) && isCivitaiUrl(reply->url())) {
        text += "。请检查 Civitai API Key，或确认该资源是否需要登录权限。";
    }
    if (status > 0) text += QString(" (HTTP %1)").arg(status);
    return text;
}

QStringList MainWindow::readModelTagsFromJson(const QJsonObject &root) const
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

QString MainWindow::readModelCreatorFromJson(const QJsonObject &root) const
{
    QJsonObject creator = root.value("model").toObject().value("creator").toObject();
    if (creator.isEmpty()) creator = root.value("creator").toObject();
    QString name = creator.value("username").toString().trimmed();
    if (name.isEmpty()) name = creator.value("name").toString().trimmed();
    return name;
}

QString MainWindow::readModelCreatorAvatarFromJson(const QJsonObject &root) const
{
    QJsonObject creator = root.value("model").toObject().value("creator").toObject();
    if (creator.isEmpty()) creator = root.value("creator").toObject();
    return creator.value("image").toString().trimmed();
}

QJsonObject MainWindow::mergeCivitaiModelIntoVersion(const QJsonObject &versionRoot, const QJsonObject &modelRoot) const
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

void MainWindow::applyCivitaiAttributionToItem(QListWidgetItem *item, const QString &creator, const QStringList &tags)
{
    if (!item) return;
    item->setData(ROLE_MODEL_CREATOR, creator);
    item->setData(ROLE_MODEL_TAGS, tags);
}

void MainWindow::fetchModelInfoFromCivitai(const QString &hash) {
    // 获取当前正在处理的文件名 (从属性或当前选中项)
    // 建议直接传参进来，或者确保 ui->modelList->property("current_processing_file") 是本地文件名(BaseName)
    QString localBaseName = ui->modelList->property("current_processing_file").toString();
    QString modelDir = ui->modelList->property("current_model_dir").toString();
    QString urlStr = QString("https://civitai.com/api/v1/model-versions/by-hash/%1").arg(hash);
    QString filePath = ui->modelList->property("current_processing_path").toString();
    if (modelDir.isEmpty() && !filePath.isEmpty()) {
        modelDir = QFileInfo(filePath).absolutePath();
    }
    QNetworkRequest request = makeNetworkRequest(QUrl(urlStr));
    QNetworkReply *reply = netManager->get(request);
    // 将本地文件名绑定到 Reply 对象上，确保回调时知道是哪个模型
    reply->setProperty("localBaseName", localBaseName);
    reply->setProperty("modelDir", modelDir);
    reply->setProperty("localFilePath", filePath);
    reply->setProperty("filePath", filePath);
    reply->setProperty("currentSha256", hash.trimmed());

    connect(reply, &QNetworkReply::finished, this, [this, reply](){
        this->onApiMetadataReceived(reply);
    });
}

// 解析 JSON
bool MainWindow::readLocalJson(const QString &dirPath, const QString &baseName, ModelMeta &meta)
{
    if (dirPath.isEmpty()) return false;
    QString jsonPath = QDir(dirPath).filePath(baseName + ".json");

    QFile file(jsonPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) return false;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    QJsonObject root = doc.object();

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
    meta.creatorName = readModelCreatorFromJson(root);
    meta.creatorAvatarUrl = readModelCreatorAvatarFromJson(root);
    meta.modelTags = readModelTagsFromJson(root);

    // 2. 解析触发词组
    QJsonArray twArray = root["trainedWords"].toArray();
    for(auto val : twArray) {
        QString w = val.toString().trimmed();
        if(w.endsWith(",")) w.chop(1);
        if(!w.isEmpty()) meta.trainedWordsGroups.append(w);
    }

    // 3. 解析图片 (补全了 width, height, nsfw 的读取)
    QJsonArray images = root["images"].toArray();
    for (auto val : images) {
        QJsonObject imgObj = val.toObject();
        QString type = imgObj["type"].toString();
        QString url = imgObj["url"].toString();
        if (type == "video" || url.endsWith(".mp4", Qt::CaseInsensitive) || url.endsWith(".webm", Qt::CaseInsensitive)) {
            continue; // 跳过，不加入列表
        }
        ImageInfo imgInfo;
        imgInfo.url = imgObj["url"].toString();
        imgInfo.hash = imgObj["hash"].toString();
        imgInfo.width = imgObj["width"].toInt();       // 补全
        imgInfo.height = imgObj["height"].toInt();     // 补全
        imgInfo.nsfwLevel = imgObj["nsfwLevel"].toInt();
        imgInfo.nsfw = (imgInfo.nsfwLevel > 1);

        QJsonObject imgMeta = imgObj["meta"].toObject();
        if(!imgMeta.isEmpty()) {
            imgInfo.prompt = imgMeta["prompt"].toString();
            imgInfo.negativePrompt = imgMeta["negativePrompt"].toString();
            imgInfo.sampler = imgMeta["sampler"].toString();
            imgInfo.steps = QString::number(imgMeta["steps"].toInt());
            imgInfo.cfgScale = QString::number(imgMeta["cfgScale"].toDouble());
            imgInfo.seed = QString::number(imgMeta["seed"].toVariant().toLongLong());
        }
        meta.images.append(imgInfo);
    }

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
        // 通常取第一个文件信息
        QJsonObject f = files[0].toObject();
        meta.fileSizeMB = f["sizeKB"].toDouble() / 1024.0;
        meta.fileNameServer = f["name"].toString();
        meta.sha256 = f["hashes"].toObject()["SHA256"].toString();
    }

    QString bestPreviewPath = findLocalPreviewPath(dirPath, baseName, meta.fileNameServer, 0);

    if (QFile::exists(bestPreviewPath)) {
        QImageReader reader(bestPreviewPath);
        if (reader.canRead()) {
            meta.previewPath = bestPreviewPath;
        } else {
            meta.previewPath = ""; // 文件坏了或不是图片
        }
    } else {
        meta.previewPath = ""; // 没找到文件
    }

    currentMeta = meta;
    return true;
}

// 联网回调
void MainWindow::onApiMetadataReceived(QNetworkReply *reply)
{
    QString localBaseName = reply->property("localBaseName").toString();
    QString modelDir = reply->property("modelDir").toString();
    QString filePath = reply->property("filePath").toString();
    QString currentSha256 = reply->property("currentSha256").toString();
    reply->deleteLater();
    ui->btnForceUpdate->setEnabled(true);
    currentHashSyncForceRefresh = false;

    if (modelDir.isEmpty() && !filePath.isEmpty()) {
        modelDir = QFileInfo(filePath).absolutePath();
    }

    QString currentSelectedPath;
    if (QListWidgetItem *currentItem = ui->modelList->currentItem()) {
        currentSelectedPath = currentItem->data(ROLE_FILE_PATH).toString();
    }
    if (!filePath.isEmpty() && !currentSelectedPath.isEmpty() && currentSelectedPath != filePath) {
        m_forceResyncPreview = false;
        m_skipPreviewSync = false;
        return;
    }

    const QByteArray versionJsonBytes = reply->property("versionJson").toByteArray();
    if (reply->error() != QNetworkReply::NoError) {
        if (!versionJsonBytes.isEmpty()) {
            qDebug() << "Civitai model detail fetch failed, falling back to version metadata:" << reply->errorString();
        } else {
            clearLayout(ui->layoutTriggerStack); // 清空触发词区域
            const QString err = civitaiNetworkErrorMessage(reply);
            if (startDetailCivArchiveFallback(filePath, localBaseName, modelDir, err, currentSha256)) {
                ui->statusbar->showMessage("Civitai 获取失败，正在尝试 CivArchive...", 4000);
                return;
            }
            recordModelSyncFailure(filePath, localBaseName, err);
            setModelTitleError(err);
            ui->textDescription->setPlainText(QString("上次同步失败，请点击刷新模型详情重新同步。\n%1").arg(err));
            transitionToImage("");
            m_forceResyncPreview = false;
            m_skipPreviewSync = false;
            ui->statusbar->showMessage("元数据获取失败: " + err, 4000);
            return;
        }
    }

    const QByteArray responseBody = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(responseBody);
    const bool usingVersionFallback = !versionJsonBytes.isEmpty() && reply->error() != QNetworkReply::NoError;
    QJsonObject root = usingVersionFallback ? QJsonDocument::fromJson(versionJsonBytes).object() : doc.object();
    if ((!usingVersionFallback && doc.isNull()) || root.isEmpty()) {
        const QString err = "返回数据为空或不是有效 JSON";
        if (startDetailCivArchiveFallback(filePath, localBaseName, modelDir, err, currentSha256)) {
            ui->statusbar->showMessage("Civitai 元数据解析失败，正在尝试 CivArchive...", 4000);
            return;
        }
        recordModelSyncFailure(filePath, localBaseName, err);
        setModelTitleError(err);
        ui->textDescription->setPlainText(QString("上次同步失败，请点击刷新模型详情重新同步。\n%1").arg(err));
        transitionToImage("");
        m_forceResyncPreview = false;
        m_skipPreviewSync = false;
        ui->statusbar->showMessage("元数据解析失败", 3000);
        return;
    }

    if (!versionJsonBytes.isEmpty()) {
        const QJsonObject versionRoot = QJsonDocument::fromJson(versionJsonBytes).object();
        if (reply->error() == QNetworkReply::NoError) {
            root = mergeCivitaiModelIntoVersion(versionRoot, root);
        } else {
            root = versionRoot;
        }
    } else {
        const int modelIdForDetail = root["modelId"].toInt();
        if (modelIdForDetail > 0) {
            QNetworkReply *detailReply = netManager->get(makeNetworkRequest(QUrl(QString("https://civitai.com/api/v1/models/%1").arg(modelIdForDetail))));
            detailReply->setProperty("localBaseName", localBaseName);
            detailReply->setProperty("modelDir", modelDir);
            detailReply->setProperty("localFilePath", filePath);
            detailReply->setProperty("filePath", filePath);
            detailReply->setProperty("currentSha256", currentSha256);
            detailReply->setProperty("versionJson", QJsonDocument(root).toJson(QJsonDocument::Compact));
            connect(detailReply, &QNetworkReply::finished, this, [this, detailReply]() {
                this->onApiMetadataReceived(detailReply);
            });
            return;
        }
    }

    clearModelSyncFailure(filePath);
    setModelTitleNormal();
    root["syncedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    ModelMeta meta;

    // 1. 基础信息
    QString modelRealName = root["model"].toObject()["name"].toString();
    QString versionName = root["name"].toString();
    if (modelRealName.isEmpty()) modelRealName = localBaseName;
    QString fullName = modelRealName;
    if (!versionName.isEmpty()) fullName += " [" + versionName + "]";
    meta.modelName = modelRealName;
    meta.versionName = versionName;
    meta.name = fullName;
    meta.filePath = filePath;
    meta.fileName = QFileInfo(filePath).fileName();
    meta.isLocalEdited = false;
    meta.isLocalOnly = false;
    meta.creatorName = readModelCreatorFromJson(root);
    meta.creatorAvatarUrl = readModelCreatorAvatarFromJson(root);
    meta.modelTags = readModelTagsFromJson(root);

    // 更新 UI 列表项
    // 找到对应的 Item (可能通过 localBaseName 查找)
    for(int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (item->data(ROLE_MODEL_NAME).toString() == localBaseName) {
            item->setData(ROLE_CIVITAI_NAME, fullName); // 更新缓存的名称
            item->setData(ROLE_LOCAL_EDITED, false);
            item->setData(ROLE_CIVITAI_MODEL_ID, root["modelId"].toInt());
            item->setData(ROLE_CIVITAI_VERSION_ID, root["id"].toInt());
            item->setData(ROLE_MODEL_TYPE, meta.type);
            applyCivitaiAttributionToItem(item, meta.creatorName, meta.modelTags);

            // 如果开启了选项，立即更新显示文本
            if (optUseCivitaiName) {
                item->setText(fullName);
            }
            break;
        }
    }

    // 2. 触发词 (保存为列表)
    meta.trainedWordsGroups.clear();
    QJsonArray twArray = root["trainedWords"].toArray();
    for(auto val : twArray) {
        QString w = val.toString().trimmed();
        if(w.endsWith(",")) w.chop(1);
        if(!w.isEmpty()) meta.trainedWordsGroups.append(w);
    }

    int modelId = root["modelId"].toInt();
    meta.modelId = modelId;
    meta.versionId = root["id"].toInt();
    if (modelId > 0) meta.modelUrl = QString("https://civitai.com/models/%1").arg(modelId);

    meta.baseModel = root["baseModel"].toString();
    meta.type = root["model"].toObject()["type"].toString();
    meta.nsfw = root["model"].toObject()["nsfw"].toBool();
    meta.description = root["description"].toString();
    meta.createdAt = root["createdAt"].toString();

    QJsonObject stats = root["stats"].toObject();
    meta.downloadCount = stats["downloadCount"].toInt();
    meta.thumbsUpCount = stats["thumbsUpCount"].toInt();

    // 3. 文件信息 (计算大小, Hash)
    QJsonArray files = root["files"].toArray();
    if (!files.isEmpty()) {
        QJsonObject f = files[0].toObject(); // 默认取第一个
        meta.fileSizeMB = f["sizeKB"].toDouble() / 1024.0;
        meta.sha256 = f["hashes"].toObject()["SHA256"].toString();
        meta.fileNameServer = f["name"].toString();
        for(int i = 0; i < ui->modelList->count(); ++i) {
            QListWidgetItem *item = ui->modelList->item(i);
            if (item && item->data(ROLE_MODEL_NAME).toString() == localBaseName) {
                item->setData(ROLE_CIVITAI_SHA256, meta.sha256);
                break;
            }
        }
    }

    // 4. 图片信息 (非常重要)
    QJsonArray images = root["images"].toArray();
    for (auto val : images) {
        QJsonObject imgObj = val.toObject();
        QString type = imgObj["type"].toString();
        QString url = imgObj["url"].toString();
        if (type == "video" || url.endsWith(".mp4", Qt::CaseInsensitive) || url.endsWith(".webm", Qt::CaseInsensitive)) {
            continue; // 跳过视频，不加入列表
        }

        ImageInfo imgInfo;
        imgInfo.url = imgObj["url"].toString();
        imgInfo.hash = imgObj["hash"].toString(); // blurhash
        imgInfo.width = imgObj["width"].toInt();
        imgInfo.height = imgObj["height"].toInt();
        imgInfo.nsfwLevel = imgObj["nsfwLevel"].toInt();
        imgInfo.nsfw = (imgInfo.nsfwLevel > 1);

        QJsonObject imgMeta = imgObj["meta"].toObject();
        if (!imgMeta.isEmpty()) {
            imgInfo.prompt = imgMeta["prompt"].toString();
            imgInfo.negativePrompt = imgMeta["negativePrompt"].toString();
            imgInfo.sampler = imgMeta["sampler"].toString();
            imgInfo.steps = QString::number(imgMeta["steps"].toInt());
            imgInfo.cfgScale = QString::number(imgMeta["cfgScale"].toDouble());
            imgInfo.seed = QString::number(imgMeta["seed"].toVariant().toLongLong());
        }
        meta.images.append(imgInfo);
    }

    if (!meta.images.isEmpty()) {
        // 强制使用本地文件名构造图片路径，解决重名和冲突问题
        QString savePath = QDir::cleanPath(QDir(modelDir).filePath(localBaseName + ".preview.png"));

        if (m_skipPreviewSync) {
            if (QFile::exists(savePath)) {
                meta.previewPath = savePath;
            }
        } else {
            meta.previewPath = savePath;
        }
    }

    // 保留本地私有状态字段，避免同步 Civitai metadata 时丢失用户本地信息。
    QFile existingMetadata(QDir(modelDir).filePath(localBaseName + ".json"));
    if (existingMetadata.exists() && existingMetadata.open(QIODevice::ReadOnly)) {
        const QJsonObject oldRoot = QJsonDocument::fromJson(existingMetadata.readAll()).object();
        const QStringList localKeys = {"localEdited", "localEditedAt", "localOnly", "modelUrl"};
        for (const QString &key : localKeys) {
            if (oldRoot.contains(key)) root[key] = oldRoot.value(key);
        }
    } else {
        root["localOnly"] = false;
    }
    meta.isLocalEdited = root["localEdited"].toBool(false);
    meta.isLocalOnly = root["localOnly"].toBool(false);
    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (item && item->data(ROLE_MODEL_NAME).toString() == localBaseName) {
            item->setData(ROLE_LOCAL_EDITED, meta.isLocalEdited || meta.isLocalOnly);
            applyCivitaiAttributionToItem(item, meta.creatorName, meta.modelTags);
            break;
        }
    }

    // 保存并更新UI
    saveLocalMetadata(modelDir, localBaseName, root);
    if (!m_skipPreviewSync && m_forceResyncPreview && !meta.images.isEmpty()) {
        const QString coverPath = QFileInfo(QDir(modelDir).filePath(localBaseName + ".preview.png")).absoluteFilePath();
        const ImageInfo &cover = meta.images.first();
        if (QFile::exists(coverPath) && !cover.url.isEmpty()) {
            enqueueDownload(cover.url,
                            coverPath,
                            nullptr,
                            localBaseName,
                            0,
                            previewPayloadFromImageInfo(cover),
                            true,
                            true,
                            false);
        }
    }

    currentMeta = meta; // 缓存到成员变量
    ui->scrollAreaWidgetContents->setUpdatesEnabled(false);
    updateDetailView(meta);
    QTimer::singleShot(0, this, [this]() {
        ui->scrollAreaWidgetContents->setUpdatesEnabled(true);
        ui->scrollAreaWidgetContents->update();
    });
    m_forceResyncPreview = false;
    m_skipPreviewSync = false;
    ui->statusbar->showMessage("元数据已更新", 2000);
}

void MainWindow::applyDownloadedPreviewToUi(const QString &localBaseName, const QString &savePath)
{
    if (localBaseName.isEmpty() || savePath.isEmpty()) return;

    const QString downloadedFileName = QFileInfo(savePath).fileName();
    const bool isCoverPreview = (downloadedFileName == localBaseName + ".preview.png");

    QIcon fitIcon = getFitIcon(savePath);
    bool modelListUpdated = false;

    if (isCoverPreview) {
        QIcon newIcon = getSquareIcon(QPixmap(savePath));

        for (int i = 0; i < ui->modelList->count(); ++i) {
            QListWidgetItem *item = ui->modelList->item(i);
            if (item->data(ROLE_MODEL_NAME).toString() == localBaseName) {
                item->setData(ROLE_PREVIEW_PATH, savePath);
                item->setIcon(newIcon);
                applyModelHighlightColor(item);
                modelListUpdated = true;
            }
        }

        for (int i = 0; i < ui->homeGalleryList->count(); ++i) {
            QListWidgetItem *item = ui->homeGalleryList->item(i);
            QString itemPath = item->data(ROLE_FILE_PATH).toString();
            QFileInfo fi(itemPath);
            if (fi.completeBaseName() == localBaseName) {
                item->setData(ROLE_PREVIEW_PATH, savePath);
                item->setIcon(newIcon);
            }
        }

        if (modelListUpdated) {
            std::function<void(QTreeWidgetItem*)> updateTreeNode = [&](QTreeWidgetItem *node) {
                if (!node) return;
                if (node->data(0, ROLE_MODEL_NAME).toString() == localBaseName) {
                    node->setData(0, ROLE_PREVIEW_PATH, savePath);
                    node->setIcon(0, newIcon);
                    applyModelHighlightColor(node);
                }
                for (int i = 0; i < node->childCount(); ++i) updateTreeNode(node->child(i));
            };
            for (int i = 0; i < ui->collectionTree->topLevelItemCount(); ++i) {
                updateTreeNode(ui->collectionTree->topLevelItem(i));
            }
            ui->modelList->viewport()->update();
            ui->modelList->update();
            ui->collectionTree->viewport()->update();
        }
        ui->homeGalleryList->viewport()->update();
    }

    if (ui->layoutGallery) {
        for (int k = 0; k < ui->layoutGallery->count(); ++k) {
            if (QLayoutItem *li = ui->layoutGallery->itemAt(k)) {
                if (QPushButton *btn = qobject_cast<QPushButton*>(li->widget())) {
                    QString btnPath = QFileInfo(btn->property("savePath").toString()).absoluteFilePath();
                    if (btnPath == savePath) {
                        btn->setProperty("fullImagePath", savePath);
                        btn->setIcon(fitIcon);
                        btn->setIconSize(QSize(90, 135));
                        btn->setText("");
                    }
                }
            }
        }
    }

    QListWidgetItem *currentItem = ui->modelList->currentItem();
    if (currentItem && currentItem->data(ROLE_MODEL_NAME).toString() == localBaseName) {
        if (isCoverPreview) {
            currentMeta.previewPath = savePath;
            currentHeroPath = "";
            transitionToImage(savePath);
        }
    }
}

void MainWindow::saveLocalMetadata(const QString &modelDir, const QString &baseName, const QJsonObject &data) {
    if (modelDir.isEmpty()) return;
    QString savePath = QDir(modelDir).filePath(baseName + ".json");
    QFile file(savePath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(data).toJson());
        file.close();
    }
}

void MainWindow::startModelHashSync(const QString &filePath, const QString &baseName, bool forceRefresh)
{
    if (filePath.isEmpty() || baseName.isEmpty()) {
        ui->btnForceUpdate->setEnabled(true);
        return;
    }
    currentProcessingPath = filePath;
    currentHashSyncForceRefresh = forceRefresh;
    ui->modelList->setProperty("current_processing_file", baseName);
    ui->modelList->setProperty("current_processing_path", filePath);
    ui->modelList->setProperty("current_model_dir", QFileInfo(filePath).absolutePath());
    ui->btnForceUpdate->setEnabled(false);
    ui->statusbar->showMessage("正在计算 Hash...", 0);
    ui->lblModelName->setText("正在分析模型文件 (计算 Hash)...");

    hashWatcher->setFuture(QtConcurrent::run(backgroundThreadPool, [filePath]() {
        return FileUtils::calculateSha256Hex(filePath);
    }));
}

void MainWindow::onOpenUrlClicked() {
    QString url = ui->btnOpenUrl->property("url").toString();
    if (!url.isEmpty()) QDesktopServices::openUrl(QUrl(url));
}

QString MainWindow::currentModelLoraTagName() const
{
    QString filePath;
    if (QListWidgetItem *item = ui->modelList->currentItem()) {
        filePath = item->data(ROLE_FILE_PATH).toString();
    }
    if (filePath.isEmpty()) filePath = currentMeta.filePath;

    QString tagName;
    if (!filePath.isEmpty()) {
        tagName = const_cast<MainWindow*>(this)->getSafetensorsInternalName(filePath);
        if (tagName.isEmpty()) tagName = QFileInfo(filePath).completeBaseName();
    }
    if (tagName.isEmpty()) tagName = currentMeta.fileNameServer;
    if (tagName.isEmpty()) tagName = currentMeta.name;

    if (tagName.endsWith(".safetensors", Qt::CaseInsensitive) ||
        tagName.endsWith(".ckpt", Qt::CaseInsensitive) ||
        tagName.endsWith(".pt", Qt::CaseInsensitive)) {
        tagName = QFileInfo(tagName).completeBaseName();
    }
    return tagName.trimmed();
}

void MainWindow::onCopyLoraTagClicked()
{
    const QString tagName = currentModelLoraTagName();
    if (tagName.isEmpty()) {
        ui->statusbar->showMessage("无法生成 LoRA 标签：未选中模型", 2500);
        return;
    }

    const QString tag = QString("<lora:%1:1>").arg(tagName);
    QApplication::clipboard()->setText(tag);
    ui->statusbar->showMessage("已复制 LoRA 标签: " + tag, 2500);
}

void MainWindow::downloadThumbnail(const QString &url, const QString &savePath, QPushButton *button)
{
    const QUrl requestUrl(url);
    QNetworkRequest req = makeNetworkRequest(requestUrl);

    QNetworkReply *reply = netManager->get(req);
    reply->setProperty("tokenRetry", false);
    QPointer<QPushButton> safeBtn = button;

    connect(reply, &QNetworkReply::finished, this, [this, reply, savePath, safeBtn, requestUrl]() {
        reply->deleteLater();

        // 检查网络错误
        if (reply->error() != QNetworkReply::NoError) {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if ((status == 401 || status == 403) && !reply->property("tokenRetry").toBool() && !civitaiApiKey().isEmpty()) {
                QUrl retryUrl = civitaiUrlWithToken(requestUrl);
                if (retryUrl != requestUrl) {
                    QNetworkReply *retryReply = netManager->get(makeNetworkRequest(retryUrl, false));
                    retryReply->setProperty("tokenRetry", true);
                    connect(retryReply, &QNetworkReply::finished, this, [this, retryReply, savePath, safeBtn]() {
                        retryReply->deleteLater();
                        if (retryReply->error() != QNetworkReply::NoError) {
                            if (safeBtn) safeBtn->setText("Error");
                            qDebug() << "Download retry error:" << civitaiNetworkErrorMessage(retryReply);
                            return;
                        }
                        QByteArray data = retryReply->readAll();
                        if (data.isEmpty()) {
                            if (safeBtn) safeBtn->setText("Empty");
                            return;
                        }
                        QFile file(savePath);
                        if (file.open(QIODevice::WriteOnly)) {
                            file.write(data);
                            file.close();
                            IconLoaderTask *task = new IconLoaderTask(savePath, 100, 0, this, savePath, true);
                            task->setAutoDelete(true);
                            threadPool->start(task);
                            if (safeBtn) safeBtn->setText("");
                        }
                    });
                    return;
                }
            }
            if (safeBtn) safeBtn->setText("Error");
            qDebug() << "Download error:" << civitaiNetworkErrorMessage(reply);
            return;
        }

        QByteArray data = reply->readAll();

        // 再次检查数据是否为空 (防止 User-Agent 还是被拦截的情况)
        if (data.isEmpty()) {
            if (safeBtn) safeBtn->setText("Empty");
            return;
        }

        QFile file(savePath);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(data);
            file.close();

            // 启动异步加载图标任务
            IconLoaderTask *task = new IconLoaderTask(savePath, 100, 0, this, savePath, true);
            task->setAutoDelete(true);
            threadPool->start(task);

            if (safeBtn) {
                safeBtn->setText(""); // 清除 Loading 文字
            }
        }
    });
}

void MainWindow::showFullImageDialog(const QString &imagePath)
{
    if (imagePath.isEmpty() || !QFile::exists(imagePath)) return;

    QDialog *dlg = new QDialog(this);
    dlg->setWindowTitle("Preview (Esc to close)");
    dlg->resize(1200, 900);

    // 使用黑色背景
    QVBoxLayout *layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(0,0,0,0);

    QLabel *imgLabel = new QLabel;
    imgLabel->setStyleSheet("background-color: black;");
    imgLabel->setAlignment(Qt::AlignCenter);

    QPixmap pix(imagePath);
    // 缩放以适应屏幕/窗口
    imgLabel->setPixmap(pix.scaled(dlg->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));

    layout->addWidget(imgLabel);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->exec();
}

// 新增：适应比例图标 (Fit 模式)
QIcon MainWindow::getFitIcon(const QString &path)
{
    QPixmap pix(path);
    if (pix.isNull()) return QIcon();

    // 目标尺寸 (根据你的图库按钮大小设定，这里是 100x150)
    QSize targetSize(100, 150);

    // 创建一个透明底的容器
    QPixmap base(targetSize);
    base.fill(Qt::transparent); // 或者使用 Qt::black

    QPainter painter(&base);
    // 开启抗锯齿
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::Antialiasing);

    // 计算适应比例 (KeepAspectRatio)
    QPixmap scaled = pix.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    // 计算居中位置
    int x = (targetSize.width() - scaled.width()) / 2;
    int y = (targetSize.height() - scaled.height()) / 2;

    // 绘制图片
    painter.drawPixmap(x, y, scaled);

    return QIcon(base);
}

void MainWindow::onIconLoaded(const QString &id, const QImage &image)
{
    if (isShuttingDown) return;
    if (id.startsWith("EDITIMG|")) {
        const QStringList parts = id.split('|');
        if (parts.size() >= 4) {
            bool okToken = false;
            bool okIndex = false;
            const int token = parts[1].toInt(&okToken);
            const int index = parts[2].toInt(&okIndex);
            const QString path = parts[3];

            if (okToken && okIndex &&
                token == editImageLoadToken &&
                index >= 0 && index < ui->listEditImages->count()) {
                QListWidgetItem *item = ui->listEditImages->item(index);
                if (item &&
                    item->data(Qt::UserRole).toInt() == index &&
                    item->data(ROLE_EDIT_IMAGE_PATH).toString() == path) {
                    item->setIcon(QIcon(QPixmap::fromImage(image)));
                    const QString title = (index == 0) ? "封面 / Cover" : QString("预览 %1").arg(index);
                    item->setText(title);
                    item->setToolTip(path);
                }
            }
        }
        return;
    }

    // =========================================================
    // 1. 解析任务来源 (Parsing ID)
    // =========================================================
    QString filePath = id;
    bool isSidebarTask = false;
    bool isHomeTask = false;

    // 判断是否有特定前缀
    if (id.startsWith("SIDEBAR:")) {
        isSidebarTask = true;
        filePath = id.mid(8); // 去掉 "SIDEBAR:" 前缀 (长度为8)
    } else if (id.startsWith("HOME:")) {
        isHomeTask = true;
        filePath = id.mid(5); // 去掉 "HOME:" 前缀 (长度为5)
    } else {
        // 兼容其他没有加前缀的情况（比如用户返图、详情页点击），默认都允许更新
        isSidebarTask = true;
        isHomeTask = true;
    }

    // =========================================================
    // 2. 准备图片和 NSFW 处理逻辑
    // =========================================================
    QPixmap originalPix = QPixmap::fromImage(image);
    QIcon originalIcon(originalPix); // 默认图标

    // 延迟模糊计算 (Lambda)
    QPixmap blurredPix;
    auto getDisplayPix = [&](bool isNSFW) {
        if (optFilterNSFW && isNSFW && optNSFWMode == 1) {
            if (blurredPix.isNull()) blurredPix = applyNSFWBlur(originalPix);
            return blurredPix;
        }
        return originalPix;
    };

    // =========================================================
    // 3. 更新主页列表 (Home Gallery) - 仅限 HOME 或 通用任务
    // =========================================================
    if (isHomeTask) {
        for(int i = 0; i < ui->homeGalleryList->count(); ++i) {
            QListWidgetItem *item = ui->homeGalleryList->item(i);
            // 匹配路径
            if (item->data(ROLE_FILE_PATH).toString() == filePath) {
                // 处理 NSFW
                bool isNSFW = item->data(ROLE_NSFW_LEVEL).toInt() > optNSFWLevel;
                if (optFilterNSFW && isNSFW && optNSFWMode == 1) {
                    if (blurredPix.isNull()) blurredPix = applyNSFWBlur(originalPix);
                    // 主页使用圆角遮罩
                    QPixmap roundedBlur = applyRoundedMask(blurredPix, 12);
                    item->setIcon(QIcon(roundedBlur));
                } else {
                    item->setIcon(QIcon(originalPix));
                }
            }
        }
    }

    // =========================================================
    // 4. 更新侧边栏 (Sidebar List & Tree) - 仅限 SIDEBAR 或 通用任务
    // =========================================================
    if (isSidebarTask) {
        // --- A. 更新侧边栏列表 ---
        for(int i = 0; i < ui->modelList->count(); ++i) {
            QListWidgetItem *item = ui->modelList->item(i);
            if (item->data(ROLE_FILE_PATH).toString() == filePath) {
                bool isNSFW = item->data(ROLE_NSFW_LEVEL).toInt() > optNSFWLevel;
                if (optFilterNSFW && isNSFW && optNSFWMode == 1) {
                    if (blurredPix.isNull()) blurredPix = applyNSFWBlur(originalPix);
                    // 侧边栏使用 getSquareIcon 处理样式 (方形+内边距)
                    QPixmap roundedBlur = applyRoundedMask(blurredPix, 12);
                    item->setIcon(getSquareIcon(roundedBlur));
                } else {
                    item->setIcon(getSquareIcon(originalPix));
                }
                applyModelHighlightColor(item);
            }
        }

        // --- B. 更新收藏夹树状图 ---
        std::function<void(QTreeWidgetItem*)> updateTreeIcon = [&](QTreeWidgetItem *node) {
            if (!node) return;
            if (node->data(0, ROLE_FILE_PATH).toString() == filePath) {
                bool isNSFW = node->data(0, ROLE_NSFW_LEVEL).toInt() > optNSFWLevel;
                if (optFilterNSFW && isNSFW && optNSFWMode == 1) {
                    if (blurredPix.isNull()) blurredPix = applyNSFWBlur(originalPix);
                    QPixmap roundedBlur = applyRoundedMask(blurredPix, 12);
                    node->setIcon(0, getSquareIcon(roundedBlur));
                } else {
                    node->setIcon(0, getSquareIcon(originalPix));
                }
                applyModelHighlightColor(node);
            }
            for (int j = 0; j < node->childCount(); ++j) updateTreeIcon(node->child(j));
        };
        for(int i = 0; i < ui->collectionTree->topLevelItemCount(); ++i) {
            updateTreeIcon(ui->collectionTree->topLevelItem(i));
        }
    }

    // =========================================================
    // 5. 更新详情页、返图、Hero
    // =========================================================
    // 逻辑：如果这是一个纯粹的 "SIDEBAR" 任务 (通常是64px小图)，
    // 我们不希望它更新 Detail(100px+) 或 Hero(大图)，因为会变糊。
    // 如果是 "HOME" (180px) 或 通用 (原图)，则允许更新。

    bool allowHighResUpdate = !id.startsWith("SIDEBAR:");

    if (allowHighResUpdate) {
        // --- A. 详情页预览列表 (Detail Gallery) ---
        QLayout *layout = ui->layoutGallery;
        if (layout) {
            for (int i = 0; i < layout->count(); ++i) {
                QLayoutItem *item = layout->itemAt(i);
                if (item->widget()) {
                    QPushButton *btn = qobject_cast<QPushButton*>(item->widget());
                    if (btn) {
                        if (btn->property("fullImagePath").toString() == filePath) {
                            bool isNSFW = btn->property("isNSFW").toBool();
                            QPixmap p = getDisplayPix(isNSFW);
                            btn->setIcon(QIcon(p));
                            btn->setIconSize(QSize(90, 135));
                            btn->setText("");
                        }
                    }
                }
            }
        }

        // --- B. 用户返图列表 (User Gallery) ---
        bool userImageUpdated = false;
        if (ui->listUserImages) {
            for (int i = 0; i < ui->listUserImages->count(); ++i) {
                QListWidgetItem *item = ui->listUserImages->item(i);
                if (item->data(ROLE_USER_IMAGE_PATH).toString() == filePath) {
                    item->setIcon(originalIcon);
                    userImageUpdated = true;
                }
            }
        }
        if (userImageUpdated) {
            queuedUserImageThumbPaths.remove(filePath);
            loadedUserImageThumbPaths.insert(filePath);
            scheduleVisibleUserImageThumbLoad();
        }

        // --- C. Hero 大图过渡 ---
        if (filePath == currentMeta.previewPath) {
            // 只有当路径匹配且当前没有显示这张图时才刷新
            if (currentHeroPath != filePath) {
                transitionToImage(filePath);
            }
        }
    }
}

QString MainWindow::findLocalPreviewPath(const QString &dirPath, const QString &currentBaseName, const QString &serverFileName, int imgIndex) const
{
    if (dirPath.isEmpty()) return "";
    QDir dir(dirPath);
    QString suffix = (imgIndex == 0) ? ".preview.png" : QString(".preview.%1.png").arg(imgIndex);
    return QFileInfo(dir.filePath(currentBaseName + suffix)).absoluteFilePath();
}

void MainWindow::onHashCalculated()
{
    // 获取后台线程的返回值
    QString hash = hashWatcher->result();
    const QString filePath = currentProcessingPath;
    const QString baseName = ui->modelList->property("current_processing_file").toString();

    QString currentSelectedPath;
    if (QListWidgetItem *currentItem = ui->modelList->currentItem()) {
        currentSelectedPath = currentItem->data(ROLE_FILE_PATH).toString();
    }
    if (!filePath.isEmpty() && !currentSelectedPath.isEmpty() && filePath != currentSelectedPath) {
        ui->btnForceUpdate->setEnabled(true);
        currentHashSyncForceRefresh = false;
        return;
    }

    // 检查：如果计算出来的 Hash 为空，说明文件可能被锁或读失败
    if (hash.isEmpty()) {
        const QString err = "无法读取文件或计算 Hash 失败";
        recordModelSyncFailure(filePath, baseName, err);
        setModelTitleError(err);
        ui->textDescription->setPlainText(QString("上次同步失败，请点击刷新模型详情重新同步。\n%1").arg(err));
        ui->btnForceUpdate->setEnabled(true);
        currentHashSyncForceRefresh = false;
        return;
    }

    // Hash 算完了，现在开始联网
    setModelTitleNormal();
    ui->lblModelName->setText("Hash 计算完成，正在获取元数据...");
    ui->statusbar->showMessage("Hash 计算完成，正在获取元数据...", 2000);
    fetchModelInfoFromCivitai(hash); // 调用你原来的联网函数
}

void MainWindow::updateBackgroundImage()
{
    if (!ui->backgroundLabel || !ui->heroFrame || !ui->scrollAreaWidgetContents) return;

    // The background remains in content coordinates so it scrolls together
    // with the hero, while the blur composition itself uses stable absolute
    // fade positions and no QLabel stretching.
    QSize targetSize = ui->scrollAreaWidgetContents->size();
    if (QWidget *viewport = ui->scrollAreaWidgetContents->parentWidget()) {
        targetSize.setWidth(qMax(targetSize.width(), viewport->width()));
        targetSize.setHeight(qMax(targetSize.height(), viewport->height()));
    }
    if (targetSize.isEmpty()) targetSize = QSize(1920, 1080);
    const QRect bgRect(QPoint(0, 0), targetSize);
    if (ui->backgroundLabel->geometry() != bgRect) {
        ui->backgroundLabel->setGeometry(bgRect);
    }

    // 如果正在动画，不处理 Resize，由动画循环处理
    if (transitionAnim && transitionAnim->state() == QAbstractAnimation::Running) return;

    // 获取 Hero 尺寸用于对齐
    QSize heroSize = ui->heroFrame->size();
    if (heroSize.isEmpty()) heroSize = QSize(targetSize.width(), 400);

    if (!currentHeroPixmap.isNull()) {
        currentBlurredBgPix = applyBlurToImage(currentHeroPixmap.toImage(), targetSize, heroSize);
        ui->backgroundLabel->setPixmap(currentBlurredBgPix);
    } else if (!currentHeroPath.isEmpty() && QFile::exists(currentHeroPath)) {
        // 如果缓存丢了但有路径，重新读图生成
        QImage img(currentHeroPath);
        currentBlurredBgPix = applyBlurToImage(img, targetSize, heroSize);
        ui->backgroundLabel->setPixmap(currentBlurredBgPix);
    } else {
        ui->backgroundLabel->clear();
        QPixmap empty(targetSize);
        empty.fill(QColor(AppStyle::MainBackground));
        ui->backgroundLabel->setPixmap(empty);
    }
}

void MainWindow::onSearchTextChanged(const QString &text)
{
    QString query = text.trimmed();
    QString targetBaseModel = ui->comboBaseModel->currentText();

    // 1. 自动重置收藏夹 (保留逻辑)
    if (!query.isEmpty() && !currentCollectionFilter.isEmpty()) {
        currentCollectionFilter = "";
        refreshHomeCollectionsUI();
    }

    // 2. 遍历筛选
    for(int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!isModelListItem(item)) {
            if (item) item->setHidden(true);
            if (item) item->setData(ROLE_MODEL_FILTER_VISIBLE, false);
            continue;
        }

        // === 修改：获取名称的逻辑 ===
        // 优先用 UserRole (排序用的也是这个，保持一致)，如果为空则用显示的文本
        QString modelName = item->data(ROLE_MODEL_NAME).toString();
        if (modelName.isEmpty()) modelName = item->text();

        QStringList searchable;
        searchable << modelName
                   << item->text()
                   << item->data(ROLE_CIVITAI_NAME).toString()
                   << item->data(ROLE_MODEL_CREATOR).toString()
                   << item->data(ROLE_MODEL_TAGS).toStringList()
                   << item->data(ROLE_USER_NOTE).toString()
                   << item->data(ROLE_USER_TAGS).toStringList()
                   << item->data(ROLE_USER_CUSTOM_TRIGGERS).toStringList();

        bool nameMatch = query.isEmpty();
        if (!nameMatch) {
            for (const QString &part : searchable) {
                if (part.contains(query, Qt::CaseInsensitive)) {
                    nameMatch = true;
                    break;
                }
            }
        }

        // B. 底模匹配
        bool baseMatch = true;
        if (targetBaseModel != "All") {
            QString itemBase = item->data(ROLE_FILTER_BASE).toString();
            if (itemBase != targetBaseModel) baseMatch = false;
        }

        // 综合判断：只记录过滤结果，实际显示由文件夹折叠逻辑统一处理
        item->setData(ROLE_MODEL_FILTER_VISIBLE, nameMatch && baseMatch);
    }
    applyModelFolderVisibility();

    // 3. 刷新主页
    refreshHomeGallery();

    // 4. 切回主页优化 (保留逻辑)
    if (ui->mainStack->currentIndex() == 1) {
        QListWidgetItem *currentItem = ui->modelList->currentItem();
        if (currentItem && !currentItem->data(ROLE_MODEL_FILTER_VISIBLE).toBool()) {
            ui->mainStack->setCurrentIndex(0);
        }
    }

    refreshCollectionTreeView();
}

void MainWindow::showCollectionMenu(const QList<QListWidgetItem*> &items, const QPoint &globalPos)
{
    QList<QListWidgetItem*> modelItems;
    for (QListWidgetItem *item : items) {
        if (isModelListItem(item)) modelItems.append(item);
    }
    if (modelItems.isEmpty()) return;

    QMenu menu(this);

    // 1. 标题逻辑
    if (modelItems.count() == 1) {
        QListWidgetItem *first = modelItems.first();
        QString name = first->text();

        if (name.isEmpty()) {
            // 如果 text 为空（主页大图模式），尝试获取 Civitai 名
            name = first->data(ROLE_CIVITAI_NAME).toString();
            // 如果 Civitai 名也为空，获取文件名
            if (name.isEmpty()) {
                name = first->data(ROLE_MODEL_NAME).toString();
            }
        }

        if (name.length() > 20) name = name.left(18) + "..";
        QAction *titleAct = menu.addAction(name);
        titleAct->setEnabled(false);
    } else {
        QAction *titleAct = menu.addAction(QString("已选中 %1 个模型").arg(modelItems.count()));
        titleAct->setEnabled(false);
    }

    menu.addSeparator();

    QColor defaultColor(102, 192, 244, 96);
    if (modelItems.count() == 1) {
        const QString filePath = QFileInfo(modelItems.first()->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
        if (modelHighlightColors.contains(filePath)) {
            defaultColor = modelHighlightColors.value(filePath);
        }
    }

    QAction *actSetHighlightColor = menu.addAction("设置高亮颜色... / Set Highlight Color...");
    connect(actSetHighlightColor, &QAction::triggered, this, [this, modelItems, defaultColor]() {
        QColorDialog dialog(defaultColor, this);
        dialog.setWindowTitle("设置模型高亮颜色");
        dialog.setOption(QColorDialog::ShowAlphaChannel, true);
        if (dialog.exec() != QDialog::Accepted) return;

        const QColor color = dialog.selectedColor();
        if (!color.isValid()) return;
        setHighlightColorForItems(modelItems, color);
    });

    QAction *actClearHighlightColor = menu.addAction("清除高亮颜色 / Clear Highlight Color");
    bool hasHighlightColor = false;
    for (QListWidgetItem *item : modelItems) {
        const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
        if (modelHighlightColors.contains(filePath)) {
            hasHighlightColor = true;
            break;
        }
    }
    actClearHighlightColor->setEnabled(hasHighlightColor);
    connect(actClearHighlightColor, &QAction::triggered, this, [this, modelItems]() {
        clearHighlightColorForItems(modelItems);
    });

    menu.addSeparator();

    QAction *actEditUserNote = menu.addAction("编辑评分、备注与触发词... / Edit Rating, Note && Triggers...");
    actEditUserNote->setEnabled(modelItems.count() == 1);
    connect(actEditUserNote, &QAction::triggered, this, [this, modelItems]() {
        if (modelItems.count() == 1) openModelNoteDialog(modelItems.first());
    });

    QMenu *ratingMenu = menu.addMenu("设置评分 / Set Rating");
    QAction *clearRatingAct = ratingMenu->addAction("未评分 / Clear Rating");
    connect(clearRatingAct, &QAction::triggered, this, [this, modelItems]() {
        setUserRatingForItems(modelItems, 0.0);
    });
    ratingMenu->addSeparator();
    for (int half = 2; half <= 10; ++half) {
        const double rating = half / 2.0;
        QAction *act = ratingMenu->addAction(formatModelRating(rating));
        connect(act, &QAction::triggered, this, [this, modelItems, rating]() {
            setUserRatingForItems(modelItems, rating);
        });
    }

    QAction *actAddUserTags = menu.addAction("添加用户标签... / Add User Tags...");
    connect(actAddUserTags, &QAction::triggered, this, [this, modelItems]() {
        bool ok = false;
        const QString text = QInputDialog::getMultiLineText(this,
                                                            "添加用户标签",
                                                            "输入标签，逗号或换行分隔：",
                                                            QString(),
                                                            &ok);
        if (!ok) return;
        addUserTagsForItems(modelItems, normalizeModelUserTagsText(text));
    });

    QAction *actRemoveUserTags = menu.addAction("移除用户标签... / Remove User Tags...");
    connect(actRemoveUserTags, &QAction::triggered, this, [this, modelItems]() {
        bool ok = false;
        const QString text = QInputDialog::getMultiLineText(this,
                                                            "移除用户标签",
                                                            "输入要移除的标签，逗号或换行分隔：",
                                                            QString(),
                                                            &ok);
        if (!ok) return;
        removeUserTagsForItems(modelItems, normalizeModelUserTagsText(text));
    });

    menu.addSeparator();

    // 打开模型文件所在位置
    QStringList targetFilePaths;
    for (QListWidgetItem *item : modelItems) {
        if (!item) continue;
        QString path = item->data(ROLE_FILE_PATH).toString().trimmed();
        if (path.isEmpty()) continue;
        if (!targetFilePaths.contains(path)) targetFilePaths.append(path);
    }

    QAction *actOpenModelLocation = menu.addAction("打开模型位置 / Show Model in Folder");
    actOpenModelLocation->setEnabled(!targetFilePaths.isEmpty());
    connect(actOpenModelLocation, &QAction::triggered, this, [this, targetFilePaths]() {
        if (targetFilePaths.isEmpty()) return;

        const QString filePath = targetFilePaths.first();
        showFileInFolder(filePath);
        if (targetFilePaths.size() > 1) {
            ui->statusbar->showMessage(QString("已打开首个模型位置（共选中 %1 个）").arg(targetFilePaths.size()), 2500);
        }
    });

    menu.addSeparator();

    // 辅助 Lambda：获取 items 对应的所有 BaseName (用于收藏夹数据存储)
    // 收藏夹系统始终使用 ROLE_MODEL_NAME (文件名) 作为 Key，不受显示名称影响
    QStringList targetBaseNames;
    for(auto *item : modelItems) {
        targetBaseNames.append(item->data(ROLE_MODEL_NAME).toString());
    }

    // =========================================================
    // 2. "从收藏夹移除..."
    // =========================================================
    QMenu *removeMenu = menu.addMenu("从指定收藏夹移除...");
    bool canRemoveAny = false;

    for (auto it = collections.begin(); it != collections.end(); ++it) {
        QString colName = it.key();
        if (colName == FILTER_UNCATEGORIZED) continue;

        // 检查选中的模型中，是否有任何一个在这个收藏夹里
        // 逻辑：只要有一个在，就允许点击移除（移除操作只移除在里面的）
        int countInCol = 0;
        for (const QString &bn : targetBaseNames) {
            if (it.value().contains(bn)) countInCol++;
        }

        if (countInCol > 0) {
            canRemoveAny = true;
            QString actionText = QString("%1 (%2)").arg(colName).arg(countInCol);
            QAction *actRemove = removeMenu->addAction(actionText);

            connect(actRemove, &QAction::triggered, this, [this, colName, targetBaseNames](){
                int removedCount = 0;
                for (const QString &bn : targetBaseNames) {
                    if (collections[colName].contains(bn)) {
                        collections[colName].removeAll(bn);
                        removedCount++;
                    }
                }
                saveCollections();
                refreshHomeGallery();
                refreshCollectionTreeView();
                ui->statusbar->showMessage(QString("已从 %1 移除 %2 个模型").arg(colName).arg(removedCount), 2000);
            });
        }
    }
    if (!canRemoveAny) removeMenu->setEnabled(false);

    // =========================================================
    // 3. "添加至收藏夹..."
    // =========================================================
    QMenu *addMenu = menu.addMenu("添加至收藏夹...");

    for (auto it = collections.begin(); it != collections.end(); ++it) {
        QString colName = it.key();
        if (colName == FILTER_UNCATEGORIZED) continue;

        QAction *action = addMenu->addAction(colName);
        action->setCheckable(true);

        // 状态检查逻辑：
        // - 如果所有选中项都在该收藏夹 -> Checked
        // - 如果部分在 -> (可选: PartiallyChecked，这里简单处理为 Unchecked)
        // - 如果都不在 -> Unchecked
        bool allIn = true;
        for (const QString &bn : targetBaseNames) {
            if (!it.value().contains(bn)) {
                allIn = false;
                break;
            }
        }
        action->setChecked(allIn);

        connect(action, &QAction::triggered, this, [this, colName, targetBaseNames, action](){
            bool isAdding = action->isChecked(); // 触发后的状态
            int count = 0;

            if (isAdding) {
                // 批量添加
                for (const QString &bn : targetBaseNames) {
                    if (!collections[colName].contains(bn)) {
                        collections[colName].append(bn);
                        count++;
                    }
                }
                ui->statusbar->showMessage(QString("已将 %1 个模型加入 %2").arg(count).arg(colName), 2000);
            } else {
                // 批量移除（取消勾选）
                for (const QString &bn : targetBaseNames) {
                    if (collections[colName].contains(bn)) {
                        collections[colName].removeAll(bn);
                        count++;
                    }
                }
                ui->statusbar->showMessage(QString("已从 %1 移除 %2 个模型").arg(colName).arg(count), 2000);
            }
            saveCollections();

            // 如果影响了当前视图，刷新
            if (currentCollectionFilter == colName) refreshHomeGallery();
            refreshCollectionTreeView();
        });
    }

    addMenu->addSeparator();
    QAction *newAction = addMenu->addAction("新建收藏夹...");
    connect(newAction, &QAction::triggered, this, [this, targetBaseNames](){
        bool ok;
        QString text = QInputDialog::getText(this, "新建", "名称:", QLineEdit::Normal, "", &ok);
        if(ok && !text.isEmpty()) {
            if(!collections.contains(text)) {
                // 直接将选中的模型全部加入新收藏夹
                collections[text] = targetBaseNames;
                saveCollections();
                refreshHomeCollectionsUI();
                refreshCollectionTreeView();
                ui->statusbar->showMessage(QString("新建收藏夹并加入 %1 个模型").arg(targetBaseNames.count()), 2000);
            }
        }
    });

    menu.exec(globalPos);
}

void MainWindow::applyModelHighlightColor(QListWidgetItem *item)
{
    if (!isModelListItem(item)) return;

    const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
    if (modelHighlightColors.contains(filePath)) {
        item->setData(ROLE_MODEL_HIGHLIGHT_COLOR, modelHighlightColors.value(filePath));
    } else {
        item->setData(ROLE_MODEL_HIGHLIGHT_COLOR, QVariant());
    }
}

void MainWindow::applyModelHighlightColor(QTreeWidgetItem *item)
{
    if (!item || item->data(0, ROLE_FILE_PATH).toString().isEmpty()) return;

    const QString filePath = QFileInfo(item->data(0, ROLE_FILE_PATH).toString()).absoluteFilePath();
    if (modelHighlightColors.contains(filePath)) {
        item->setData(0, ROLE_MODEL_HIGHLIGHT_COLOR, modelHighlightColors.value(filePath));
    } else {
        item->setData(0, ROLE_MODEL_HIGHLIGHT_COLOR, QVariant());
    }
}

void MainWindow::setHighlightColorForItems(const QList<QListWidgetItem*> &items, const QColor &color)
{
    if (!color.isValid()) return;

    int changed = 0;
    for (QListWidgetItem *item : items) {
        if (!isModelListItem(item)) continue;
        const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
        if (filePath.isEmpty()) continue;
        modelHighlightColors.insert(filePath, color);
        applyModelHighlightColor(item);
        changed++;
    }
    if (changed <= 0) return;

    saveModelHighlightColors();
    refreshCollectionTreeView();
    ui->modelList->viewport()->update();
    ui->statusbar->showMessage(QString("已设置 %1 个模型的高亮颜色").arg(changed), 2000);
}

void MainWindow::clearHighlightColorForItems(const QList<QListWidgetItem*> &items)
{
    int changed = 0;
    for (QListWidgetItem *item : items) {
        if (!isModelListItem(item)) continue;
        const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
        if (filePath.isEmpty()) continue;
        if (modelHighlightColors.remove(filePath) > 0) {
            applyModelHighlightColor(item);
            changed++;
        }
    }
    if (changed <= 0) return;

    saveModelHighlightColors();
    refreshCollectionTreeView();
    ui->modelList->viewport()->update();
    ui->statusbar->showMessage(QString("已清除 %1 个模型的高亮颜色").arg(changed), 2000);
}

void MainWindow::preloadItemMetadata(QListWidgetItem *item, const QString &jsonPath)
{
    // 初始化默认值 (方便排序)
    item->setData(ROLE_SORT_DATE, 0);
    item->setData(ROLE_SORT_DOWNLOADS, 0);
    item->setData(ROLE_SORT_LIKES, 0);
    item->setData(ROLE_SORT_USAGE_COUNT, 0);
    item->setData(ROLE_SORT_LAST_USED, 0);
    item->setData(ROLE_FILTER_BASE, "Unknown");
    item->setData(ROLE_NSFW_LEVEL, 1);
    item->setData(ROLE_LOCAL_EDITED, false);
    item->setData(ROLE_CIVITAI_MODEL_ID, 0);
    item->setData(ROLE_CIVITAI_VERSION_ID, 0);
    item->setData(ROLE_CIVITAI_SHA256, QString());
    item->setData(ROLE_MODEL_CREATOR, QString());
    item->setData(ROLE_MODEL_TAGS, QStringList());
    item->setData(ROLE_MODEL_TYPE, QString());

    // === 读取本地文件时间 (下载/添加时间) ===
    QString filePath = item->data(ROLE_FILE_PATH).toString();
    QFileInfo fi(filePath);
    QDateTime birthTime = fi.birthTime(); // 获取创建时间
    // 在某些系统(如Linux部分文件系统) birthTime 可能无效，回退到 lastModified
    if (!birthTime.isValid()) {
        birthTime = fi.lastModified();
    }
    item->setData(ROLE_SORT_ADDED, birthTime.toMSecsSinceEpoch());
    // ============================================

    QFile file(jsonPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        // 如果没有 JSON，尝试用文件修改时间作为日期
        QFileInfo fi(item->data(ROLE_FILE_PATH).toString());
        item->setData(ROLE_SORT_DATE, fi.lastModified().toMSecsSinceEpoch());
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    QJsonObject root = doc.object();
    applyCivitaiAttributionToItem(item, readModelCreatorFromJson(root), readModelTagsFromJson(root));
    item->setData(ROLE_MODEL_TYPE, root["model"].toObject()["type"].toString());

    // 读取模型真实名称
    QString modelName = root["model"].toObject()["name"].toString();
    QString versionName = root["name"].toString();
    if (!modelName.isEmpty()) {
        QString fullName = modelName;
        if (!versionName.isEmpty()) fullName += " [" + versionName + "]";
        item->setData(ROLE_CIVITAI_NAME, fullName); // 存入 UserRole
    }
    bool localEdited = root["localEdited"].toBool(false) || root["localOnly"].toBool(false);
    item->setData(ROLE_LOCAL_EDITED, localEdited);
    item->setData(ROLE_CIVITAI_MODEL_ID, root["modelId"].toInt());
    item->setData(ROLE_CIVITAI_VERSION_ID, root["id"].toInt());

    // NSFW
    int coverLevel = 1; // 默认 Safe

    QJsonArray images = root["images"].toArray();
    if (!images.isEmpty()) {
        // 优先：读取 images[0] 的 nsfwLevel
        // 因为侧边栏和主页显示的都是这张图，我们只关心这张图是否违规
        QJsonObject coverObj = images[0].toObject();
        if (coverObj.contains("nsfwLevel")) {
            coverLevel = coverObj["nsfwLevel"].toInt();
        }
        // 兼容旧数据：有的旧 JSON image 里只有 nsfw (None/Soft/Mature/X)
        else if (coverObj.contains("nsfw")) {
            QString val = coverObj["nsfw"].toString().toLower();
            if (val == "x" || val == "mature") coverLevel = 16;
            else if (val == "soft") coverLevel = 2;
            else coverLevel = 1;
        }
    }
    else {
        // 后备：如果 images 数组为空（极少见），才回退到读取整个模型的等级
        if (root.contains("nsfwLevel")) coverLevel = root["nsfwLevel"].toInt();
        else if (root["nsfw"].toBool()) coverLevel = 16;
    }

    // 存入 Item，供后续判断
    item->setData(ROLE_NSFW_LEVEL, coverLevel);

    // 1. 底模 (Base Model)
    QString baseModel = root["baseModel"].toString();
    if (!baseModel.isEmpty()) item->setData(ROLE_FILTER_BASE, baseModel);

    // 2. 时间 (Created At)
    QString dateStr = root["createdAt"].toString();
    if (!dateStr.isEmpty()) {
        QDateTime dt = QDateTime::fromString(dateStr, Qt::ISODate);
        if (dt.isValid()) item->setData(ROLE_SORT_DATE, dt.toMSecsSinceEpoch());
    } else {
        // 后备：使用文件时间
        QFileInfo fi(item->data(ROLE_FILE_PATH).toString());
        item->setData(ROLE_SORT_DATE, fi.lastModified().toMSecsSinceEpoch());
    }

    // 3. 数据 (Stats)
    QJsonObject stats = root["stats"].toObject();
    item->setData(ROLE_SORT_DOWNLOADS, stats["downloadCount"].toInt());
    item->setData(ROLE_SORT_LIKES, stats["thumbsUpCount"].toInt());
    QJsonArray files = root["files"].toArray();
    if (!files.isEmpty()) {
        item->setData(ROLE_CIVITAI_SHA256, files[0].toObject()["hashes"].toObject()["SHA256"].toString());
    }
}

void MainWindow::refreshModelUsageStatsAsync()
{
    if (!ui || !ui->modelList) return;

    const int currentToken = ++modelUsageStatsToken;

    QList<ModelUsageInput> models;
    models.reserve(ui->modelList->count());

    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!isModelListItem(item)) continue;

        item->setData(ROLE_SORT_USAGE_COUNT, 0);
        item->setData(ROLE_SORT_LAST_USED, 0);

        const QString filePath = item->data(ROLE_FILE_PATH).toString();
        const QString baseName = item->data(ROLE_MODEL_NAME).toString();
        if (!filePath.isEmpty()) {
            ModelUsageInput input;
            input.filePath = filePath;
            input.baseName = baseName;
            input.type = item->data(ROLE_MODEL_TYPE).toString();
            input.civitaiName = item->data(ROLE_CIVITAI_NAME).toString();
            input.sha256 = item->data(ROLE_CIVITAI_SHA256).toString();
            models.append(input);
        }
    }

    if (models.isEmpty()) return;

    if (imageCache.isEmpty()) {
        if (ui->comboSort->currentIndex() == 5 || ui->comboSort->currentIndex() == 6) {
            executeSort();
            refreshHomeGallery();
            refreshCollectionTreeView();
        }
        refreshCurrentDetailCacheStatus();
        refreshUsageAnalysisWidget();
        return;
    }

    const QMap<QString, UserImageInfo> cacheCopy = imageCache;
    const int matchMode = optUserGalleryMatchMode;

    auto *watcher = new QFutureWatcher<QList<ModelUsageStatResult>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, currentToken]() {
        const QList<ModelUsageStatResult> stats = watcher->result();
        watcher->deleteLater();
        if (currentToken != modelUsageStatsToken) return;

        QMap<QString, ModelUsageStatResult> statsByPath;
        for (const ModelUsageStatResult &stat : stats) {
            statsByPath.insert(QFileInfo(stat.filePath).absoluteFilePath(), stat);
        }

        for (int i = 0; i < ui->modelList->count(); ++i) {
            QListWidgetItem *item = ui->modelList->item(i);
            if (!isModelListItem(item)) continue;

            const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
            const ModelUsageStatResult stat = statsByPath.value(filePath);
            item->setData(ROLE_SORT_USAGE_COUNT, stat.usageCount);
            item->setData(ROLE_SORT_LAST_USED, stat.lastUsed);
        }

        const int sortType = ui->comboSort->currentIndex();
        if (sortType == 5 || sortType == 6) {
            executeSort();
            refreshHomeGallery();
            refreshCollectionTreeView();
        }
        refreshCurrentDetailCacheStatus();
        refreshUsageAnalysisWidget();
    });

    watcher->setFuture(QtConcurrent::run(
        backgroundThreadPool,
        [models, cacheCopy, matchMode]() {
            return calculateModelUsageStatsWorker(models, cacheCopy, matchMode);
        }));
}

void MainWindow::onSortIndexChanged(int index) {
    executeSort();
}

bool MainWindow::isModelListItem(const QListWidgetItem *item) const
{
    return item
           && !item->data(ROLE_IS_FOLDER_HEADER).toBool()
           && !item->data(ROLE_FILE_PATH).toString().isEmpty();
}

QListWidgetItem *MainWindow::createModelFolderHeader(const QString &folderName, const QString &folderKey) const
{
    QListWidgetItem *header = new QListWidgetItem(folderName);
    header->setData(ROLE_IS_FOLDER_HEADER, true);
    header->setData(ROLE_MODEL_FOLDER_KEY, folderKey);
    header->setData(ROLE_MODEL_FOLDER_COLLAPSED, collapsedModelFolders.contains(folderKey));
    header->setData(ROLE_MODEL_ROOT_NAME, folderName);
    header->setFlags(Qt::ItemIsEnabled);
    QFont font = header->font();
    font.setBold(true);
    header->setFont(font);
    header->setForeground(QColor(AppStyle::AccentBlue));
    header->setBackground(QColor(AppStyle::HeaderBackground));
    return header;
}

void MainWindow::applyModelFolderVisibility()
{
    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!item) continue;
        if (!item->data(ROLE_IS_FOLDER_HEADER).toBool()) {
            if (!optModelListFolderGrouping && isModelListItem(item)) {
                item->setHidden(!item->data(ROLE_MODEL_FILTER_VISIBLE).toBool());
            }
            continue;
        }

        const QString folderKey = item->data(ROLE_MODEL_FOLDER_KEY).toString();
        const bool collapsed = collapsedModelFolders.contains(folderKey);
        int visibleChildCount = 0;
        for (int j = i + 1; j < ui->modelList->count(); ++j) {
            QListWidgetItem *child = ui->modelList->item(j);
            if (!child) continue;
            if (child->data(ROLE_IS_FOLDER_HEADER).toBool()) break;
            if (!isModelListItem(child)) continue;

            const bool filterVisible = child->data(ROLE_MODEL_FILTER_VISIBLE).toBool();
            if (filterVisible) {
                visibleChildCount++;
            }
            child->setHidden(!filterVisible || collapsed);
        }

        const QString folderName = item->data(ROLE_MODEL_ROOT_NAME).toString().isEmpty()
                                       ? item->text().mid(2).section(" (", 0, 0).trimmed()
                                       : item->data(ROLE_MODEL_ROOT_NAME).toString();
        item->setData(ROLE_MODEL_FOLDER_COLLAPSED, collapsed);
        item->setText(QString("%1 %2 (%3)").arg(collapsed ? " + " : " - ", folderName).arg(visibleChildCount));
        item->setHidden(!optModelListFolderGrouping || visibleChildCount <= 0);
    }
}

void MainWindow::toggleModelFolderCollapsed(const QString &folderKey)
{
    if (folderKey.isEmpty()) return;

    if (collapsedModelFolders.contains(folderKey)) {
        collapsedModelFolders.remove(folderKey);
    } else {
        collapsedModelFolders.insert(folderKey);
    }
    applyModelFolderVisibility();
    saveGlobalConfig();
}

void MainWindow::executeSort()
{
    // 0: Name, 1: Date(New), 2: Downloads, 3: Likes, 4: Date Added, 5: Usage, 6: Recently Used, 7: User Rating
    int sortType = ui->comboSort->currentIndex();

    // 1. 取出所有 Item
    QList<QListWidgetItem*> items;
    while(ui->modelList->count() > 0) {
        QListWidgetItem *item = ui->modelList->takeItem(0);
        if (item && item->data(ROLE_IS_FOLDER_HEADER).toBool()) {
            delete item;
        } else if (item) {
            items.append(item);
        }
    }

    // === 准备自然排序器 (用于 Case 0) ===
    QCollator collator;
    collator.setNumericMode(true); // 开启数字模式 (让 v2 排在 v10 前面)
    collator.setCaseSensitivity(Qt::CaseInsensitive); // 忽略大小写 (让 a 和 A 排在一起)
    collator.setIgnorePunctuation(false); // 不忽略标点 (保证 [ 能参与排序)

    // 2. 使用 Lambda 表达式排序
    std::sort(items.begin(), items.end(),
        [sortType, &collator](QListWidgetItem *a, QListWidgetItem *b) { // 注意这里捕获了 &collator
        auto compareByName = [&collator](QListWidgetItem *left, QListWidgetItem *right) {
            return collator.compare(left->text(), right->text()) < 0;
        };

        switch (sortType) {
            case 0: // Name (A-Z)
                return compareByName(a, b);

            case 1: // Date (Newest First -> Descending)
                return a->data(ROLE_SORT_DATE).toLongLong() > b->data(ROLE_SORT_DATE).toLongLong();

            case 2: // Downloads (High -> Descending)
                return a->data(ROLE_SORT_DOWNLOADS).toInt() > b->data(ROLE_SORT_DOWNLOADS).toInt();

            case 3: // Likes (High -> Descending)
                return a->data(ROLE_SORT_LIKES).toInt() > b->data(ROLE_SORT_LIKES).toInt();

            case 4: // Date Added (Local Created At -> Descending)
                return a->data(ROLE_SORT_ADDED).toLongLong() > b->data(ROLE_SORT_ADDED).toLongLong();

            case 5: // Usage Count (Local Gallery)
            {
                const int usageA = a->data(ROLE_SORT_USAGE_COUNT).toInt();
                const int usageB = b->data(ROLE_SORT_USAGE_COUNT).toInt();
                if (usageA != usageB) return usageA > usageB;
                return compareByName(a, b);
            }

            case 6: // Recently Used (Local Gallery)
            {
                const qint64 usedA = a->data(ROLE_SORT_LAST_USED).toLongLong();
                const qint64 usedB = b->data(ROLE_SORT_LAST_USED).toLongLong();
                if (usedA != usedB) return usedA > usedB;
                return compareByName(a, b);
            }

            case 7: // Rating (User)
            {
                const double ratingA = a->data(ROLE_USER_RATING).toDouble();
                const double ratingB = b->data(ROLE_USER_RATING).toDouble();
                if (!qFuzzyCompare(ratingA + 1.0, ratingB + 1.0)) return ratingA > ratingB;
                return compareByName(a, b);
            }

            default:
                return compareByName(a, b);
            }
        }
    );

    // 3. 放回 ListWidget
    if (optModelListFolderGrouping) {
        QMap<QString, QList<QListWidgetItem*>> grouped;
        QMap<QString, QString> folderLabels;
        QStringList folderOrder;
        for (QListWidgetItem *item : items) {
            const QString rootPath = item->data(ROLE_MODEL_ROOT_PATH).toString();
            const QString rootKey = rootPath.isEmpty() ? "__unknown__" : QFileInfo(rootPath).absoluteFilePath();
            QString rootName = item->data(ROLE_MODEL_ROOT_NAME).toString();
            if (rootName.isEmpty()) rootName = "未指定文件夹";
            if (!grouped.contains(rootKey)) {
                folderOrder.append(rootKey);
                folderLabels.insert(rootKey, rootName);
            }
            grouped[rootKey].append(item);
        }

        QCollator folderCollator;
        folderCollator.setNumericMode(true);
        folderCollator.setCaseSensitivity(Qt::CaseInsensitive);
        std::sort(folderOrder.begin(), folderOrder.end(), [&](const QString &a, const QString &b) {
            return folderCollator.compare(folderLabels.value(a), folderLabels.value(b)) < 0;
        });

        for (const QString &folderKey : folderOrder) {
            ui->modelList->addItem(createModelFolderHeader(folderLabels.value(folderKey), folderKey));
            for (QListWidgetItem *item : grouped.value(folderKey)) {
                ui->modelList->addItem(item);
            }
        }
    } else {
        for(auto *item : items) {
            ui->modelList->addItem(item);
        }
    }

    // 4. 同步刷新主页
    onSearchTextChanged(ui->searchEdit->text());
}

void MainWindow::onFilterBaseModelChanged(const QString &text) {
    onSearchTextChanged(ui->searchEdit->text());
}

// 静态函数，运行在后台线程
ImageLoadResult MainWindow::processImageTask(const QString &path)
{
    ImageLoadResult result;
    result.path = path;

    QImageReader reader(path);
    reader.setAutoTransform(true);
    result.originalImg = reader.read();
    result.valid = !result.originalImg.isNull();
    return result;
}

void MainWindow::transitionToImage(const QString &path)
{
    //if (path == currentHeroPath && transitionAnim->state() != QAbstractAnimation::Running) return;
    // 下面这行替换了上面的，修复了切换模型导致的动画闪烁问题，不过尚不清楚有无其他bug
    if (path == currentHeroPath) {
        return;
    }

    // qDebug() << "Transition to:" << path;

    currentHeroPath = path;

    // 如果正在动画中，立即停止并将状态快进到“当前显示的是上一张图”
    if (transitionAnim->state() == QAbstractAnimation::Running) {
        transitionAnim->stop();
        // 如果 next 已经准备好了，就把它作为 current，以此为基础过渡到最新的 new
        if (!nextHeroPixmap.isNull()) {
            currentHeroPixmap = nextHeroPixmap;
            currentBlurredBgPix = nextBlurredBgPix;
        }
    }

    // 清理 Watcher
    if (imageLoadWatcher->isRunning()) {
        // 虽然 cancel 不能杀线程，但能断开一部分连接，配合上面的 path 校验双重保险
        imageLoadWatcher->cancel();
    }

    // 重置动画参数
    nextHeroPixmap = QPixmap();
    nextBlurredBgPix = QPixmap();
    transitionOpacity = 0.0;

    // 5. 根据路径执行
    if (path.isEmpty()) {
        transitionAnim->start(); // 淡出到黑
    } else {
        QFuture<ImageLoadResult> future = QtConcurrent::run(threadPool, &MainWindow::processImageTask, path);
        imageLoadWatcher->setFuture(future);
    }
}

QPixmap MainWindow::applyBlurToImage(const QImage &srcImg, const QSize &bgSize, const QSize &heroSize)
{
    if (srcImg.isNull()) return QPixmap();

    QPixmap tempPix;

    // === 修改点：根据设置决定是否缩小 ===
    if (optDownscaleBlur) {
        // 使用配置的缩小尺寸
        tempPix = QPixmap::fromImage(srcImg.scaledToWidth(optBlurProcessWidth, Qt::SmoothTransformation));
    } else {
        // 不缩小，直接使用原图（注意：这在模糊半径较大时非常耗时）
        tempPix = QPixmap::fromImage(srcImg);
    }

    // 2. 高斯模糊
    QGraphicsBlurEffect *blur = new QGraphicsBlurEffect;
    blur->setBlurRadius(optBlurRadius);
    blur->setBlurHints(QGraphicsBlurEffect::PerformanceHint);
    QGraphicsScene scene;
    QGraphicsPixmapItem *item = new QGraphicsPixmapItem(tempPix);
    item->setGraphicsEffect(blur);
    scene.addItem(item);
    QPixmap blurredResult(tempPix.size());
    blurredResult.fill(Qt::transparent);
    QPainter ptr(&blurredResult);
    scene.render(&ptr);

    // 3. 合成最终背景
    QPixmap finalBg(bgSize);
    finalBg.fill(QColor(AppStyle::MainBackground)); // 填充底色
    QPainter painter(&finalBg);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::Antialiasing);

    // === 核心修复：使用 heroSize 进行计算 ===
    // 这样算法就和 eventFilter 里的 Hero 绘制逻辑完全一致了

    // 保底：防止 heroSize 为空导致除以0
    int heroW = heroSize.width() > 0 ? heroSize.width() : bgSize.width();
    int heroH = heroSize.height() > 0 ? heroSize.height() : 400;

    double scaleW = (double)heroW / blurredResult.width();
    double scaleH = (double)heroH / blurredResult.height();
    double scale = qMax(scaleW, scaleH); // Cover 模式

    int newW = blurredResult.width() * scale;
    int newH = blurredResult.height() * scale;

    // 使用 heroH 来计算 Y 轴偏移
    int offsetX = (heroW - newW) / 2;
    int offsetY = (heroH - newH) / 4;

    // 绘制图片
    painter.drawPixmap(QRect(offsetX, offsetY, newW, newH), blurredResult);

    // 4. 绘制渐变遮罩 (自然融合到底部背景色)
    // Keep fade positions in content coordinates. This avoids a visible jump
    // when trigger words change the total detail-page height.
    QLinearGradient gradient(0, 0, 0, bgSize.height());
    gradient.setColorAt(0.0, AppStyle::steamBackground(120)); // 顶部半透

    const double bgH = qMax(1, bgSize.height());
    const double imgBottomY = offsetY + newH;
    const double fadeStartY = qMax(0.0, imgBottomY - qMax(120.0, heroH * 0.25));
    const double fadeEndY = qMax(fadeStartY + 1.0, imgBottomY);
    gradient.setColorAt(qBound(0.0, fadeStartY / bgH, 1.0), AppStyle::steamBackground(210));
    gradient.setColorAt(qBound(0.0, fadeEndY / bgH, 1.0), AppStyle::steamBackground());
    gradient.setColorAt(1.0, AppStyle::steamBackground());

    painter.fillRect(finalBg.rect(), gradient);
    painter.end();

    return finalBg;
}

void MainWindow::updateBackgroundDuringTransition()
{
    if (!ui->backgroundLabel) return;
    QSize bgSize = ui->backgroundLabel->size();
    if (bgSize.isEmpty()) return;

    QPixmap canvas(bgSize);
    canvas.fill(QColor(AppStyle::MainBackground)); // 纯色打底，防止交叉淡化时露出底色

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // === 核心修改：使用交叉淡化 (Cross-Fade) ===

    // 情况 A: 正在切换新图
    if (!nextBlurredBgPix.isNull()) {
        // 旧图：随着 transitionOpacity 增加而减少 (1.0 -> 0.0)
        if (!currentBlurredBgPix.isNull()) {
            painter.setOpacity(1.0 - transitionOpacity);
            painter.drawPixmap(0, 0, currentBlurredBgPix);
        }

        // 新图：随着 transitionOpacity 增加而增加 (0.0 -> 1.0)
        painter.setOpacity(transitionOpacity);
        painter.drawPixmap(0, 0, nextBlurredBgPix);
    }
    // 情况 B: 正在变为空图 (Fade out)
    else {
        if (!currentBlurredBgPix.isNull()) {
            qreal alpha = 1.0 - transitionOpacity;
            if (alpha < 0.0) alpha = 0.0;
            painter.setOpacity(alpha);
            painter.drawPixmap(0, 0, currentBlurredBgPix);
        }
    }

    painter.end();
    ui->backgroundLabel->setPixmap(canvas);
}

QString MainWindow::buildPreviewParametersText(const PreviewMetadataPayload &payload) const
{
    QStringList lines;
    if (!payload.prompt.trimmed().isEmpty()) {
        lines << payload.prompt.trimmed();
    }
    if (!payload.negativePrompt.trimmed().isEmpty()) {
        lines << "Negative prompt: " + payload.negativePrompt.trimmed();
    }

    QStringList params;
    if (!payload.steps.trimmed().isEmpty() && payload.steps.trimmed() != "0") params << "Steps: " + payload.steps.trimmed();
    if (!payload.sampler.trimmed().isEmpty()) params << "Sampler: " + payload.sampler.trimmed();
    if (!payload.cfgScale.trimmed().isEmpty() && payload.cfgScale.trimmed() != "0") params << "CFG scale: " + payload.cfgScale.trimmed();
    if (!payload.seed.trimmed().isEmpty() && payload.seed.trimmed() != "0") params << "Seed: " + payload.seed.trimmed();
    if (payload.width > 0 && payload.height > 0) params << QString("Size: %1x%2").arg(payload.width).arg(payload.height);
    if (!params.isEmpty()) lines << params.join(", ");

    return lines.join('\n').trimmed();
}

bool MainWindow::savePreviewImageWithMetadata(const QByteArray &data, const QString &savePath, const PreviewMetadataPayload &payload) const
{
    const QString parameters = buildPreviewParametersText(payload);
    if (parameters.isEmpty()) {
        QSaveFile raw(savePath);
        if (!raw.open(QIODevice::WriteOnly)) return false;
        raw.write(data);
        return raw.commit();
    }

    QImage image;
    image.loadFromData(data);
    if (image.isNull()) {
        QSaveFile raw(savePath);
        if (!raw.open(QIODevice::WriteOnly)) return false;
        raw.write(data);
        raw.commit();
        return false;
    }

    QSaveFile output(savePath);
    if (!output.open(QIODevice::WriteOnly)) return false;
    QImageWriter writer(&output, "png");
    writer.setText("parameters", parameters);
    writer.setText("civitai_prompt", payload.prompt);
    writer.setText("civitai_negative_prompt", payload.negativePrompt);
    if (writer.write(image) && output.commit()) return true;

    QSaveFile raw(savePath);
    if (raw.open(QIODevice::WriteOnly)) {
        raw.write(data);
        raw.commit();
    }
    return false;
}

bool MainWindow::ensurePreviewImageMetadata(const QString &path, const PreviewMetadataPayload &payload) const
{
    const QString parameters = buildPreviewParametersText(payload);
    return writePreviewMetadataToPath(path, parameters, payload.prompt, payload.negativePrompt);
}

void MainWindow::syncPreviewImagesFromMetadata(const QString &modelDir,
                                               const QString &baseName,
                                               const QVector<ImageInfo> &images,
                                               bool forceNonCoverDownload,
                                               bool countForMetadataSync)
{
    if (modelDir.isEmpty() || baseName.isEmpty()) return;
    for (int index = 0; index < images.size(); ++index) {
        const ImageInfo &img = images.at(index);
        const QString suffix = (index == 0) ? ".preview.png" : QString(".preview.%1.png").arg(index);
        const QString savePath = QFileInfo(QDir(modelDir).filePath(baseName + suffix)).absoluteFilePath();
        const PreviewMetadataPayload payload = previewPayloadFromImageInfo(img);
        const bool exists = QFile::exists(savePath);

        if (exists && !(forceNonCoverDownload && index > 0)) {
            enqueueDownload(img.url, savePath, nullptr, baseName, index, payload, true, true, countForMetadataSync);
            continue;
        }

        if (img.url.isEmpty()) continue;
        if (exists && index > 0 && forceNonCoverDownload) {
            QFile::remove(savePath);
        }
        enqueueDownload(img.url, savePath, nullptr, baseName, index, payload, true, false, countForMetadataSync);
    }
}

// 1. 入队函数
void MainWindow::enqueueDownload(const QString &url,
                                 const QString &savePath,
                                 QPushButton *btn,
                                 const QString &localBaseName,
                                 int imageIndex,
                                 const PreviewMetadataPayload &previewMeta,
                                 bool allowNoButton,
                                 bool metadataOnly,
                                 bool countForMetadataSync)
{
    DownloadTask task;
    task.url = url;
    task.savePath = savePath;
    task.localBaseName = localBaseName;
    task.button = btn;
    task.imageIndex = imageIndex;
    task.previewMeta = previewMeta;
    task.allowNoButton = allowNoButton;
    task.metadataOnly = metadataOnly;
    task.countForMetadataSync = countForMetadataSync;
    if (countForMetadataSync) {
        ++metadataPreviewTasksPending;
    }

    downloadQueue.enqueue(task);

    // 如果当前没有在下载，立即开始处理
    if (!isDownloading) {
        processNextDownload();
    }
}

void MainWindow::finishMetadataSyncBatch()
{
    metadataSyncWaitingForPreviews = false;
    metadataSyncPreviewImages = false;
    downloadsPage->setStatusText(QString("元信息同步完成，共处理 %1 个模型。").arg(metadataSyncTotal));
    refreshHomeGallery();
    refreshCollectionTreeView();
    startMetadataScan();
    refreshUsageAnalysisWidget();
}

void MainWindow::markMetadataPreviewTaskFinished()
{
    if (metadataPreviewTasksPending > 0) {
        --metadataPreviewTasksPending;
    }
    if (metadataSyncWaitingForPreviews && metadataPreviewTasksPending > 0 && downloadsPage) {
        downloadsPage->setStatusText(QString("正在同步预览图元信息... 剩余 %1 个任务").arg(metadataPreviewTasksPending));
    }
    if (metadataSyncWaitingForPreviews && metadataPreviewTasksPending <= 0) {
        finishMetadataSyncBatch();
    }
}

// 2. 队列处理函数 (核心：一张张下)
void MainWindow::processNextDownload()
{
    while (!downloadQueue.isEmpty()) {
        DownloadTask task = downloadQueue.dequeue();

        if (task.button.isNull() && !task.allowNoButton) {
            continue; // 按钮已销毁，直接跳过此任务处理下一个
        }

        isDownloading = true;

        // 设置按钮状态
        if (task.button) task.button->setText("Waiting...");

        QString cleanedSavePath = QFileInfo(task.savePath).absoluteFilePath();

        if (task.metadataOnly || task.url.trimmed().isEmpty()) {
            if (!task.url.trimmed().isEmpty()) {
                if (previewFileAlreadyHasPromptMetadata(cleanedSavePath)) {
                    if (task.countForMetadataSync) markMetadataPreviewTaskFinished();
                } else {
                    DownloadTask retryTask = task;
                    retryTask.metadataOnly = false;
                    retryTask.allowNoButton = true;
                    downloadQueue.enqueue(retryTask);
                }
                QTimer::singleShot(0, this, &MainWindow::processNextDownload);
                return;
            }

            auto *watcher = new QFutureWatcher<bool>(this);
            connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher, task]() {
                const bool ok = watcher->result();
                watcher->deleteLater();

                if (!ok && !task.url.trimmed().isEmpty()) {
                    DownloadTask retryTask = task;
                    retryTask.metadataOnly = false;
                    retryTask.allowNoButton = true;
                    downloadQueue.enqueue(retryTask);
                    if (downloadsPage && task.countForMetadataSync) {
                        downloadsPage->setStatusText(QString("本地预览图元信息写入失败，正在重新下载: %1")
                                                         .arg(QFileInfo(task.savePath).fileName()));
                    }
                } else if (task.countForMetadataSync) {
                    markMetadataPreviewTaskFinished();
                }

                QTimer::singleShot(0, this, &MainWindow::processNextDownload);
            });
            watcher->setFuture(QtConcurrent::run(backgroundThreadPool, [this, path = cleanedSavePath, payload = task.previewMeta]() {
                return ensurePreviewImageMetadata(path, payload);
            }));
            return;
        }

        const QUrl taskUrl(task.url);
        const bool hasToken = QUrlQuery(taskUrl).hasQueryItem("token");
        QNetworkRequest req = makeNetworkRequest(taskUrl, !hasToken);

        QNetworkReply *reply = netManager->get(req);
        reply->setProperty("isGalleryDownload", true); // 加上标识以便切换页面时精准 abort()

        // --- 新增：测速计时器 ---
        QSharedPointer<QElapsedTimer> timer = QSharedPointer<QElapsedTimer>::create();
        timer->start();

        // --- 新增：进度和网速回调 ---
        connect(reply, &QNetworkReply::downloadProgress, this, [task, timer](qint64 received, qint64 total) {
            if (task.button.isNull()) return;

            if (total > 0) {
                int percent = static_cast<int>((received * 100) / total);
                double seconds = timer->elapsed() / 1000.0;
                QString speedStr;

                // 超过0.1秒再计算速度避免除零或数据跳动
                if (seconds > 0.1) {
                    double speedKB = (received / 1024.0) / seconds;
                    if (speedKB > 1024.0) {
                        speedStr = QString::number(speedKB / 1024.0, 'f', 1) + " MB/s";
                    } else {
                        speedStr = QString::number(speedKB, 'f', 0) + " KB/s";
                    }
                }
                // 按钮上显示如： "45%\n1.2 MB/s"
                task.button->setText(QString("%1%\n%2").arg(percent).arg(speedStr));
            } else if (received > 0) {
                // 如果总大小未知
                task.button->setText(QString("%1 KB").arg(received / 1024));
            }
        });

        connect(reply, &QNetworkReply::finished, this, [this, reply, task, cleanedSavePath](){
            reply->deleteLater();

            // 如果任务是被中止的（例如用户切换了模型清空了旧队列），直接退出回调，阻止僵尸循环继续排队
            if (reply->error() == QNetworkReply::OperationCanceledError) {
                if (task.countForMetadataSync) markMetadataPreviewTaskFinished();
                return;
            }

            if (reply->error() != QNetworkReply::NoError) {
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if ((status == 401 || status == 403) && !task.url.contains("token=", Qt::CaseInsensitive) && !civitaiApiKey().isEmpty()) {
                    DownloadTask retryTask = task;
                    retryTask.url = civitaiUrlWithToken(QUrl(task.url)).toString();
                    downloadQueue.enqueue(retryTask);
                    if (task.button) task.button->setText("Retrying...");
                    QTimer::singleShot(0, this, &MainWindow::processNextDownload);
                    return;
                }

                // --- 新增：暴露具体的 SSL / 网络错误到界面 ---
                QString errStr = reply->errorString();
                if (errStr.contains("TLS", Qt::CaseInsensitive) || errStr.contains("SSL", Qt::CaseInsensitive) || errStr.contains("handshake", Qt::CaseInsensitive)) {
                    if (task.button) task.button->setText("SSL Error");
                    ui->statusbar->showMessage("HTTPS 连接失败: 缺失 OpenSSL 运行库 (libcrypto / libssl)", 8000);
                } else {
                    if (task.button) task.button->setText(QString("Err: %1").arg(status > 0 ? QString::number(status) : "Net"));
                }

                qDebug() << "Queued preview download failed:" << civitaiNetworkErrorMessage(reply) << errStr;
                if (task.countForMetadataSync) markMetadataPreviewTaskFinished();
                QTimer::singleShot(500, this, &MainWindow::processNextDownload);
                return;
            }

            QByteArray data = reply->readAll();
            if (!data.isEmpty()) {
                const bool saved = savePreviewImageWithMetadata(data, cleanedSavePath, task.previewMeta);
                if (saved) {
                    applyDownloadedPreviewToUi(task.localBaseName, cleanedSavePath);

                    if (task.button) {
                        QString currentBtnPath = QFileInfo(task.button->property("fullImagePath").toString()).absoluteFilePath();
                        QString savePathProp = QFileInfo(task.button->property("savePath").toString()).absoluteFilePath();
                        if (currentBtnPath == cleanedSavePath || savePathProp == cleanedSavePath) {
                            IconLoaderTask *iconTask = new IconLoaderTask(cleanedSavePath, 100, 0, this, cleanedSavePath, true);
                            iconTask->setAutoDelete(true);
                            threadPool->start(iconTask);
                            task.button->setText("");
                        }
                    }
                } else {
                    ui->statusbar->showMessage("预览图已保存，但未能写入可解析元信息: " + QFileInfo(cleanedSavePath).fileName(), 5000);
                }
            }
            if (task.countForMetadataSync) markMetadataPreviewTaskFinished();
            QTimer::singleShot(500, this, &MainWindow::processNextDownload);
        });

        return; // 成功发送请求并等待回调，退出 while 循环
    }

    // 队列为空
    isDownloading = false;
}

// ==========================================
//  User Gallery (Tab Page 2) Implementation
// ==========================================
void MainWindow::onToggleDetailTab() {
    int currentIndex = ui->detailContentStack->currentIndex();
    int nextIndex = (currentIndex == 1) ? 0 : 1;

    ui->scrollAreaWidgetContents->removeEventFilter(this);

    // 1. 切换页面
    ui->detailContentStack->setCurrentIndex(nextIndex);

    // 2. 按当前页实际内容收紧外壳高度，避免主详情页底部留下可滚动空白
    fitDetailContentToCurrentPage();

    QTimer::singleShot(50, this, [this, nextIndex](){
        // 恢复事件监听
        ui->scrollAreaWidgetContents->installEventFilter(this);

        // 如果切换到详情页，调整容器大小
        if (nextIndex == 0) {
            fitDetailContentToCurrentPage();
        }

        // 强制更新一次背景（避免尺寸不对）
        updateBackgroundImage();
    });

    // 3. 自动扫描逻辑 (保持不变)
    if (nextIndex == 1 && ui->listUserImages->count() == 0) {
        onRescanUserClicked();
    }
}

void MainWindow::onRescanUserClicked() {
    QListWidgetItem *item = ui->modelList->currentItem();
    if (item) {
        // 如果有选中项，说明是在查看特定模型，按名称扫描
        scanForUserImages(item->text());
    } else {
        // 如果没有选中项，说明是在 "Global Gallery" 模式
        // 传入空字符串进行全量扫描
        scanForUserImages("");
    }
}

void MainWindow::onSetSdFolderClicked() {
    editGalleryPaths(true);
}

void MainWindow::onClearUserGalleryCacheClicked()
{
    QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        "清除图库缓存",
        "这会删除本地图库提示词缓存文件，下次扫描图库时会重新解析所有图片。\n是否继续？",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );
    if (reply != QMessageBox::Yes) return;

    imageCache.clear();
    QString cachePath = qApp->applicationDirPath() + "/config/user_gallery_cache.json";
    QFile::remove(cachePath);
    refreshModelUsageStatsAsync();
    ui->statusbar->showMessage("本地图库缓存已清除", 3000);
}

void MainWindow::resetUserImageThumbLoading()
{
    queuedUserImageThumbPaths.clear();
    loadedUserImageThumbPaths.clear();
    if (userImageThumbLoadTimer) {
        userImageThumbLoadTimer->stop();
    }
}

void MainWindow::scheduleVisibleUserImageThumbLoad()
{
    if (!userImageThumbLoadTimer) return;
    userImageThumbLoadTimer->start(30);
}

void MainWindow::dispatchVisibleUserImageThumbLoad()
{
    if (!ui->listUserImages || ui->listUserImages->count() == 0) return;

    const QRect visibleRect = ui->listUserImages->viewport()->rect();
    if (visibleRect.isEmpty()) return;

    const QRect prefetchRect = visibleRect.adjusted(-visibleRect.width(),
                                                    -visibleRect.height(),
                                                    visibleRect.width(),
                                                    visibleRect.height());
    const QPoint centerPoint = visibleRect.center();

    struct ThumbCandidate {
        int priority = 2;
        int distance = 0;
        QString path;
    };

    QList<ThumbCandidate> candidates;
    candidates.reserve(ui->listUserImages->count());

    for (int i = 0; i < ui->listUserImages->count(); ++i) {
        QListWidgetItem *item = ui->listUserImages->item(i);
        if (!item || item->isHidden()) continue;

        const QString path = item->data(ROLE_USER_IMAGE_PATH).toString();
        if (path.isEmpty()) continue;
        if (loadedUserImageThumbPaths.contains(path) || queuedUserImageThumbPaths.contains(path)) continue;

        const QRect itemRect = ui->listUserImages->visualItemRect(item);
        if (!itemRect.isValid() || itemRect.isEmpty()) continue;

        ThumbCandidate candidate;
        candidate.path = path;
        candidate.distance = qAbs(itemRect.center().y() - centerPoint.y()) + qAbs(itemRect.center().x() - centerPoint.x());
        if (itemRect.intersects(visibleRect)) {
            candidate.priority = 0;
        } else if (itemRect.intersects(prefetchRect)) {
            candidate.priority = 1;
        }
        candidates.append(candidate);
    }

    if (candidates.isEmpty()) return;

    std::sort(candidates.begin(), candidates.end(), [](const ThumbCandidate &a, const ThumbCandidate &b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.distance < b.distance;
    });

    const int threadCount = qMax(1, threadPool ? threadPool->maxThreadCount() : 4);
    const int maxLaunchPerPass = qMax(8, threadCount * 2);
    int launched = 0;
    for (const ThumbCandidate &candidate : candidates) {
        if (candidate.priority > 1 && launched >= threadCount) break;
        if (launched >= maxLaunchPerPass) break;

        IconLoaderTask *task = new IconLoaderTask(candidate.path, 140, 4, this, candidate.path);
        task->setAutoDelete(true);
        threadPool->start(task);
        queuedUserImageThumbPaths.insert(candidate.path);
        ++launched;
    }
}


void MainWindow::scanForUserImages(const QString &loraBaseName) {
    ui->listUserImages->clear();
    ui->textUserPrompt->clear();
    tagFlowWidget->setData({}); // 清空 Tag
    resetUserImageThumbLoading();

    // 1. 检查目录
    const QStringList activeGalleryPaths = collectEnabledPaths(galleryPaths, disabledGalleryPaths);
    QStringList validGalleryPaths = collectValidPaths(activeGalleryPaths);
    if (validGalleryPaths.isEmpty()) {
        ui->textUserPrompt->setText("<span style='color:orange'>请先点击右上方按钮设置并启用 Stable Diffusion 图片输出目录。</span>");
        QMessageBox::warning(this, "目录无效", "设置的 SD 输出目录不存在、为空，或全部被禁用。");
        return;
    }

    const bool isGlobalMode = loraBaseName.isEmpty();
    const bool wantSummaryHashMatch = (!isGlobalMode && (optUserGalleryMatchMode == 1 || optUserGalleryMatchMode == 2));
    const bool strictSummaryHashMatch = (!isGlobalMode && optUserGalleryMatchMode == 2);
    bool useSummaryHashMatch = wantSummaryHashMatch;
    QListWidgetItem *currentItem = ui->modelList->currentItem();
    QString selectedFilePath;
    QString selectedModelDir;
    QString selectedBaseName;
    QString selectedModelType;
    QString selectedCivitaiName;
    QString selectedSha256;
    if (currentItem) {
        selectedFilePath = currentItem->data(ROLE_FILE_PATH).toString();
        selectedModelDir = QFileInfo(selectedFilePath).absolutePath();
        selectedBaseName = currentItem->data(ROLE_MODEL_NAME).toString().trimmed();
        selectedModelType = currentItem->data(ROLE_MODEL_TYPE).toString();
        selectedCivitaiName = currentItem->data(ROLE_CIVITAI_NAME).toString();
        selectedSha256 = currentItem->data(ROLE_CIVITAI_SHA256).toString();
    }
    if (selectedFilePath.isEmpty()) selectedFilePath = currentMeta.filePath;
    if (selectedModelDir.isEmpty() && !currentMeta.filePath.isEmpty()) {
        selectedModelDir = QFileInfo(currentMeta.filePath).absolutePath();
    }
    if (selectedBaseName.isEmpty() && !selectedFilePath.isEmpty()) {
        selectedBaseName = QFileInfo(selectedFilePath).completeBaseName();
    }
    if (selectedBaseName.isEmpty()) selectedBaseName = loraBaseName;
    if (selectedModelType.isEmpty()) selectedModelType = currentMeta.type;
    if (selectedCivitaiName.isEmpty()) selectedCivitaiName = currentMeta.name;
    if (selectedSha256.isEmpty()) selectedSha256 = currentMeta.sha256;
    const bool selectedIsCheckpoint = !isGlobalMode && isCheckpointModelType(selectedModelType, selectedFilePath);

    QString scanPrefix;
    if (isGlobalMode) {
        scanPrefix = "正在扫描所有本地图片";
    } else if (strictSummaryHashMatch) {
        scanPrefix = QString("正在扫描使用 '%1' 的图片 (严格摘要值匹配)").arg(loraBaseName);
    } else if (wantSummaryHashMatch) {
        scanPrefix = QString("正在扫描使用 '%1' 的图片 (摘要值匹配)").arg(loraBaseName);
    } else {
        scanPrefix = QString("正在扫描使用 '%1' 的图片").arg(loraBaseName);
    }

    ui->statusbar->showMessage(scanPrefix + "...");

    // =========================================================
    // 2. 构建模糊匹配关键字列表 (仅在非全局模式下)
    // =========================================================
    QStringList searchKeys;
    QSet<QString> normalizedLoraNames;
    QSet<QString> targetSummaryHashes;

    if(!isGlobalMode){
        QSet<QString> uniqueKeys; // 使用 Set 自动去重

        if (wantSummaryHashMatch) {
            QSet<QString> candidateHashes;

            if (!selectedSha256.isEmpty()) {
                const QString normalized = normalizeSummaryHashForMatch(selectedSha256);
                if (!normalized.isEmpty()) candidateHashes.insert(normalized);
            }

            QStringList hashJsonPaths;
            if (!selectedModelDir.isEmpty() && !selectedBaseName.isEmpty()) {
                hashJsonPaths.append(QDir(selectedModelDir).filePath(selectedBaseName + ".json"));
                hashJsonPaths.append(QDir(selectedModelDir).filePath(selectedBaseName + ".metadata.json"));
            }
            if (!selectedFilePath.isEmpty()) {
                hashJsonPaths.append(selectedFilePath + ".metadata.json");
            }

            for (const QString &path : hashJsonPaths) {
                const QSet<QString> fromFile = selectedIsCheckpoint
                                                   ? collectCheckpointHashesFromJsonFileWorker(path)
                                                   : collectLoraSummaryHashesFromJsonFileWorker(path);
                for (const QString &hash : fromFile) candidateHashes.insert(hash);
            }

            targetSummaryHashes = candidateHashes;
            if (targetSummaryHashes.isEmpty()) {
                if (strictSummaryHashMatch) {
                    ui->statusbar->showMessage("未读取到模型摘要值，严格模式下不会回退，结果可能为空。", 5000);
                } else {
                    useSummaryHashMatch = false;
                    ui->statusbar->showMessage("未读取到模型摘要值，已回退到当前匹配逻辑。", 4000);
                }
            } else {
                qDebug() << "Model summary hashes for match:" << targetSummaryHashes.values();
            }
        }

        if (selectedIsCheckpoint) {
            addModelNameVariantsWorker(selectedBaseName, uniqueKeys);
            addModelNameVariantsWorker(loraBaseName, uniqueKeys);
            for (const QString &name : splitCivitaiFullNameForMatch(selectedCivitaiName)) {
                addModelNameVariantsWorker(name, uniqueKeys);
            }
        } else if (currentItem) {
            // === 获取 Safetensors 内部名称 ===
            // 获取当前选中项的完整路径
            QString fullPath = currentItem->data(ROLE_FILE_PATH).toString();
            QString internalName = getSafetensorsInternalName(fullPath);

            if (!internalName.isEmpty()) {
                qDebug() << "Found internal LoRA name:" << internalName;
                uniqueKeys.insert(internalName);
                // 对内部名称也生成变体（例如把下划线转空格），以防万一
                QString spaceVer = internalName; spaceVer.replace("_", " "); uniqueKeys.insert(spaceVer);
                QString underVer = internalName; underVer.replace(" ", "_"); uniqueKeys.insert(underVer);
            }
        }

        // --- 回退逻辑 (Fallback) ---
        // 只有当内部名称为空时（例如 .pt 文件，或没有写入 metadata 的旧模型），
        // 我们才退而求其次，使用文件名作为筛选依据。
        if (uniqueKeys.isEmpty()) {
            // A. 获取核心名称 (去除版本号、括号、扩展名)
            // 例如: "Korean_Doll_Likeness [v1.5].safetensors" -> "Korean_Doll_Likeness"
            QString rawName = loraBaseName;
            // 去除 [xxx]
            if (rawName.contains("[")) rawName = rawName.split("[").first().trimmed();
            // 去除 .safetensors / .pt
            QFileInfo fi(rawName);
            QString coreName = fi.completeBaseName();
            // B. 生成变体
            if (!coreName.isEmpty()) {
                uniqueKeys.insert(coreName);// 1. 原始核心名
                QString spaceToUnder = coreName;// 2. 空格 -> 下划线 (My Lora -> My_Lora)
                spaceToUnder.replace(" ", "_");uniqueKeys.insert(spaceToUnder);
                QString underToSpace = coreName;// 3. 下划线 -> 空格 (My_Lora -> My Lora)
                underToSpace.replace("_", " ");uniqueKeys.insert(underToSpace);
                QString noSpace = coreName;// 4. 去除所有空格 (My Lora -> MyLora)
                noSpace.remove(" ");uniqueKeys.insert(noSpace);
                QString noUnder = coreName;// 5. 去除所有下划线 (My_Lora -> MyLora)
                noUnder.remove("_");uniqueKeys.insert(noUnder);
                QString pure = coreName;// 6. 极致纯净版 (同时去除空格和下划线)
                pure.remove(" ").remove("_");uniqueKeys.insert(pure);
            }
        }

        // 将 Set 转为 List 以便传入线程
        searchKeys = uniqueKeys.values();
        // 过滤掉太短的 Key，防止错误匹配 (例如 "v1" 这种太短的词会匹配到所有图片)
        for (auto it = searchKeys.begin(); it != searchKeys.end(); ) {
            if (it->length() < 2) {
                it = searchKeys.erase(it);
            } else {
                ++it;
            }
        }
        for (const QString &key : searchKeys) {
            const QString normalized = normalizeLoraNameForMatch(key);
            if (!normalized.isEmpty()) normalizedLoraNames.insert(normalized);
        }
        qDebug() << (selectedIsCheckpoint ? "生成的 Checkpoint 匹配名:" : "生成的 LoRA 匹配名:")
                 << normalizedLoraNames.values();
    }


    // =========================================================
    // 3. 异步扫描
    // =========================================================
    QMap<QString, UserImageInfo> currentCacheCopy = this->imageCache;
    bool recursive = optGalleryRecursive;
    const bool splitOnNewline = optSplitOnNewline;
    const QStringList filterTags = optFilterTags;
    auto scannedCount = QSharedPointer<std::atomic<int>>::create(0);
    auto matchedCount = QSharedPointer<std::atomic<int>>::create(0);
    // 开启异步任务
    QFuture<QPair<QList<UserImageInfo>, QMap<QString, UserImageInfo>>> future = QtConcurrent::run(
        backgroundThreadPool,
        [normalizedLoraNames, targetSummaryHashes, useSummaryHashMatch, isGlobalMode, selectedIsCheckpoint, recursive, splitOnNewline, filterTags, currentCacheCopy, validGalleryPaths, scannedCount, matchedCount]() {

            QList<UserImageInfo> results;
            QMap<QString, UserImageInfo> newCacheUpdates; // 用于收集需要更新到主缓存的数据

            QDirIterator::IteratorFlag iterFlag = recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
            QSet<QString> visited;

            for (const QString &root : validGalleryPaths) {
                QDirIterator it(root, QStringList() << "*.png" << "*.jpg" << "*.jpeg", QDir::Files, iterFlag);
                while (it.hasNext()) {
                    QString path = it.next();
                    if (visited.contains(path)) continue;
                    visited.insert(path);
                    scannedCount->fetch_add(1, std::memory_order_relaxed);

                    QFileInfo fi = it.fileInfo();
                    qint64 currentModified = fi.lastModified().toMSecsSinceEpoch();

                    UserImageInfo info;
                    bool needParse = true;

                    // === 核心优化：检查缓存 ===
                    if (currentCacheCopy.contains(path)) {
                        const UserImageInfo &cachedInfo = currentCacheCopy.value(path);
                        if (cachedInfo.lastModified == currentModified
                            && cachedInfo.parserVersion >= USER_GALLERY_PARSER_VERSION) {
                            // 命中缓存！直接使用，不需要 open 文件
                            info = cachedInfo;
                            needParse = false;
                        }
                    }

                    // 如果没命中缓存，或者文件被修改过，则解析
                    if (needParse) {
                        info.path = path;
                        info.lastModified = currentModified;
                        parsePngInfoWorker(path, info, splitOnNewline, filterTags); // 解析 I/O 操作

                        // 记录到更新列表
                        newCacheUpdates.insert(path, info);
                    }

                    // === 筛选逻辑 ===
                    if (info.prompt.isEmpty() && info.parameters.isEmpty()) continue;

                    bool matched = false;
                    if (isGlobalMode) {
                        matched = true;
                    } else if (useSummaryHashMatch) {
                        const QSet<QString> imageHashes = selectedIsCheckpoint
                                                              ? extractCheckpointHashValuesFromParametersWorker(info.parameters)
                                                              : extractLoraHashValuesFromParametersWorker(info.parameters);
                        matched = hashSetsMatchByPrefixWorker(imageHashes, targetSummaryHashes);
                    } else if (selectedIsCheckpoint) {
                        matched = parametersUseCheckpointWorker(info.parameters, normalizedLoraNames);
                    } else {
                        matched = promptUsesLoraWorker(info.prompt, info.parameters, normalizedLoraNames);
                    }

                    if (matched) {
                        matchedCount->fetch_add(1, std::memory_order_relaxed);
                        results.append(info);
                    }
                }
            }

            // 按时间倒序
            std::sort(results.begin(), results.end(), [](const UserImageInfo &a, const UserImageInfo &b){
                return a.lastModified > b.lastModified; // 使用 timestamp 比较更快
            });

            // 返回结果 和 需要更新的缓存
            return qMakePair(results, newCacheUpdates);
        });

    // 监听结果
    QFutureWatcher<QPair<QList<UserImageInfo>, QMap<QString, UserImageInfo>>> *watcher =
        new QFutureWatcher<QPair<QList<UserImageInfo>, QMap<QString, UserImageInfo>>>(this);

    QTimer *scanProgressTimer = new QTimer(this);
    connect(scanProgressTimer, &QTimer::timeout, this, [this, scanPrefix, scannedCount, matchedCount, lastShown = 0]() mutable {
        const int scanned = scannedCount->load(std::memory_order_relaxed);
        if (scanned <= 0) return;
        if (scanned < lastShown + 50) return;
        lastShown = (scanned / 50) * 50;
        const int matched = matchedCount->load(std::memory_order_relaxed);
        ui->statusbar->showMessage(QString("%1... 已扫描 %2 张，匹配 %3 张")
                                       .arg(scanPrefix)
                                       .arg(scanned)
                                       .arg(matched));
    });
    scanProgressTimer->start(200);

    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, scanProgressTimer](){
        if (scanProgressTimer) {
            scanProgressTimer->stop();
            scanProgressTimer->deleteLater();
        }
        auto resultPair = watcher->result();
        QList<UserImageInfo> results = resultPair.first;
        QMap<QString, UserImageInfo> newUpdates = resultPair.second;

        // 1. 更新主线程缓存
        if (!newUpdates.isEmpty()) {
            for(auto it = newUpdates.begin(); it != newUpdates.end(); ++it) {
                this->imageCache.insert(it.key(), it.value());
            }
            // 保存到磁盘，下次启动就快了
            saveUserGalleryCache();
            refreshModelUsageStatsAsync();
        }

        // 2. UI 更新逻辑 (与原代码一致)
        // 这里的 UI 渲染（new QListWidgetItem）在大量数据时也会卡顿
        // 建议使用 setUpdatesEnabled(false)
        ui->listUserImages->setUpdatesEnabled(false);
        for (const auto &info : results) {
            QListWidgetItem *item = new QListWidgetItem();
            item->setData(ROLE_USER_IMAGE_PATH, info.path);
            item->setData(ROLE_USER_IMAGE_PROMPT, info.prompt);
            item->setData(ROLE_USER_IMAGE_NEG, info.negativePrompt);
            item->setData(ROLE_USER_IMAGE_PARAMS, info.parameters);
            item->setData(ROLE_USER_IMAGE_TAGS, info.cleanTags);
            item->setData(ROLE_USER_IMAGE_NEG_TAGS, info.negativeCleanTags);
            item->setIcon(placeholderIcon);
            ui->listUserImages->addItem(item);
        }
        ui->listUserImages->setUpdatesEnabled(true);
        scheduleVisibleUserImageThumbLoad();

        refreshUserTagFlowStats();
        ui->statusbar->showMessage(QString("扫描完成，共 %1 张").arg(results.count()), 3000);
        watcher->deleteLater();
    });

    watcher->setFuture(future);
}

void MainWindow::parsePngInfo(const QString &path, UserImageInfo &info) {
    parsePngInfoWorker(path, info, optSplitOnNewline, optFilterTags);
}

void MainWindow::refreshUserTagFlowStats()
{
    QMap<QString, int> tagCounts;
    const bool currentOnly = ui->chkUserTagCurrentImageOnly && ui->chkUserTagCurrentImageOnly->isChecked();
    const bool includeNegative = ui->chkUserTagIncludeNegative && ui->chkUserTagIncludeNegative->isChecked();

    auto addTagsFromItem = [&](QListWidgetItem *item) {
        if (!item) return;
        QSet<QString> perImageTags;
        const QStringList positiveTags = item->data(ROLE_USER_IMAGE_TAGS).toStringList();
        for (const QString &tag : positiveTags) {
            if (tag.compare("BREAK", Qt::CaseInsensitive) == 0) continue;
            perImageTags.insert(tag);
        }
        if (includeNegative) {
            const QStringList negativeTags = item->data(ROLE_USER_IMAGE_NEG_TAGS).toStringList();
            for (const QString &tag : negativeTags) {
                if (tag.compare("BREAK", Qt::CaseInsensitive) == 0) continue;
                perImageTags.insert(tag);
            }
        }
        for (const QString &tag : perImageTags) {
            tagCounts[tag]++;
        }
    };

    if (currentOnly) {
        addTagsFromItem(ui->listUserImages->currentItem());
    } else {
        for (int i = 0; i < ui->listUserImages->count(); ++i) {
            addTagsFromItem(ui->listUserImages->item(i));
        }
    }

    tagFlowWidget->setData(tagCounts);
    onTagFilterChanged(tagFlowWidget->getSelectedTags());
}

void MainWindow::onUserImageClicked(QListWidgetItem *item) {
    if (!item) return;

    QString path = item->data(ROLE_USER_IMAGE_PATH).toString();
    QString prompt = item->data(ROLE_USER_IMAGE_PROMPT).toString();
    QString neg = item->data(ROLE_USER_IMAGE_NEG).toString();
    QString params = item->data(ROLE_USER_IMAGE_PARAMS).toString();

    QString safePrompt = prompt.toHtmlEscaped();
    QString safeNeg = neg.toHtmlEscaped();
    QString safeParams = params.toHtmlEscaped();

    // 格式化显示
    // 使用 <hr> 分割线，参数部分使用较小的字体和灰色
    QString html = QString(
                       "<style>"
                       "  .content { white-space: pre-wrap; }" // 定义一个样式类
                       "</style>"
                       "<p><b><span style='color:%4'>Positive:</span></b><br>"
                       "<span class='content'>%1</span></p>"
                       "<p><b><span style='color:%5'>Negative:</span></b><br>"
                       "<span class='content'>%2</span></p>"
                       "<hr style='background-color:#444; height:1px; border:none;'>"
                       "<p><b><span style='color:%6'>Parameters:</span></b><br>"
                       "<span class='content' style='color:%7; font-size:11px; font-family:Consolas, monospace;'>%3</span></p>"
                       ).arg(safePrompt, safeNeg, safeParams,
                             AppStyle::AccentBlue, AppStyle::HtmlNegative,
                             AppStyle::HtmlSubtle, AppStyle::HtmlDim);

    ui->textUserPrompt->setHtml(html);

    // 联动更新顶部 Hero 大图
    ui->heroFrame->setProperty("fullImagePath", path);
    transitionToImage(path);
    if (ui->chkUserTagCurrentImageOnly && ui->chkUserTagCurrentImageOnly->isChecked()) {
        refreshUserTagFlowStats();
    }
}

void MainWindow::onTagFilterChanged(const QSet<QString> &selectedTags) {
    int visibleCount = 0;
    const bool includeNegative = ui->chkUserTagIncludeNegative && ui->chkUserTagIncludeNegative->isChecked();

    for(int i = 0; i < ui->listUserImages->count(); ++i) {
        QListWidgetItem *item = ui->listUserImages->item(i);
        QString rawPrompt = item->data(ROLE_USER_IMAGE_PROMPT).toString();
        QStringList distinctTags = item->data(ROLE_USER_IMAGE_TAGS).toStringList();
        if (includeNegative) {
            distinctTags.append(item->data(ROLE_USER_IMAGE_NEG_TAGS).toStringList());
            distinctTags.removeDuplicates();
        }

        bool match = true;
        // AND 逻辑：必须包含所有选中的 Tag
        for (const QString &selTag : selectedTags) {
            bool tagFound = false;
            for (const QString &imgTag : distinctTags) {
                if (imgTag.compare(selTag, Qt::CaseInsensitive) == 0) {
                    tagFound = true;
                    break;
                }
            }

            if (!tagFound) {
                match = false;
                break;
            }
        }

        item->setHidden(!match);
        if (match) visibleCount++;
    }

    ui->statusbar->showMessage(QString("筛选: %1 张图片符合条件").arg(visibleCount));
}

void MainWindow::onGalleryButtonClicked()
{
    // 1. 清除侧边栏模型选中状态，表示现在不是看某个具体模型
    ui->modelList->clearSelection();

    // 2. 切换到详情页 (Page 1)
    ui->mainStack->setCurrentIndex(1);

    // 3. 强制切换到“本地返图”标签页 (Index 1)
    // 注意：这里我们手动模拟 onToggleDetailTab 的部分逻辑
    ui->detailContentStack->setCurrentIndex(1);
    fitDetailContentToCurrentPage();

    // 4. 设置 UI 状态 (伪装成一个 Model)
    clearDetailView(); // 清空之前的模型信息

    // 自定义标题
    ui->lblModelName->setText("Global User Gallery / 所有用户返图");
    ui->lblModelName->setStyleSheet(AppStyle::globalGalleryTitleStyle());

    // 隐藏/禁用一些不相关的按钮
    ui->btnForceUpdate->setVisible(false);
    ui->btnCheckModelUpdate->setVisible(false);
    ui->btnOpenUrl->setVisible(false);
    ui->btnEditMeta->setVisible(false);
    ui->btnFavorite->setVisible(false);
    ui->btnShowUserGallery->setVisible(false);
    ui->btnEditMeta->setVisible(false);
    ui->btnCopyLoraTag->setVisible(false);

    // 5. 清除背景图 (或者你可以放一张默认的图库壁纸)
    currentHeroPath = "";
    transitionToImage("");

    // 6. 执行全局扫描 (传入空字符串)
    scanForUserImages("");
}

// 辅助函数：清洗单个 Tag
// 辅助函数：将 Prompt 字符串解析为 Tag 列表
QStringList MainWindow::parsePromptsToTags(const QString &rawPrompt) {
    return parsePromptsToTagsWorker(rawPrompt, optSplitOnNewline, optFilterTags);
}

void MainWindow::initMenuBar() {
    // 1. 使用 this->menuBar() 这是一个保险措施
    // 它可以确保即便 XML 里的菜单栏丢失或层级错误，这里也能获取到窗口真正的菜单栏
    QMenuBar *bar = this->menuBar();
    bar->clear(); // 清空旧内容

    // 2. 直接添加“库”按钮 (Action)
    // 这种直接 addAction 到 bar 的方式，效果就像是点击按钮，而不是弹出下拉菜单
    QAction *actLib = new QAction("📚 库 / Library", this);
    actLib->setShortcut(QKeySequence("Ctrl+1"));
    connect(actLib, &QAction::triggered, this, &MainWindow::onMenuSwitchToLibrary);
    bar->addAction(actLib);

    // 4. 直接添加“设置”按钮 (Action)
    QAction *actSet = new QAction("⚙️ 设置 / Settings", this);
    actSet->setShortcut(QKeySequence("Ctrl+2"));
    connect(actSet, &QAction::triggered, this, &MainWindow::onMenuSwitchToSettings);
    bar->addAction(actSet);

    QAction *actDownloads = new QAction("⬇️ 下载 / Downloads", this);
    actDownloads->setShortcut(QKeySequence("Ctrl+3"));
    connect(actDownloads, &QAction::triggered, this, &MainWindow::onMenuSwitchToDownloads);
    bar->addAction(actDownloads);

    // 5. 工具页
    if (!toolsTabWidget) { // 确保只初始化一次
        toolsTabWidget = new QTabWidget(this);
        toolsTabWidget->setObjectName("toolsTabWidget");

        auto makeToolPlaceholder = [this](const QString &text) {
            QWidget *page = new QWidget(toolsTabWidget);
            page->setObjectName("toolPlaceholder");
            QVBoxLayout *layout = new QVBoxLayout(page);
            QLabel *label = new QLabel(text, page);
            label->setAlignment(Qt::AlignCenter);
            label->setStyleSheet(AppStyle::MutedLabelStyle);
            layout->addWidget(label);
            return page;
        };

        toolsTabWidget->addTab(makeToolPlaceholder("点击后加载图片同步工具..."), "🔄 图片同步 / Sync");
        toolsTabWidget->addTab(makeToolPlaceholder("点击后加载提示词解析工具..."), "📝 提示词解析 / Prompt");
        toolsTabWidget->addTab(makeToolPlaceholder("点击后加载 Tag 浏览工具..."), "🏷️ Tag 浏览 / Tag");
        toolsTabWidget->addTab(makeToolPlaceholder("点击后加载大模型提示词工具..."), "🤖 大模型提示词 / LLM");
        toolsTabWidget->addTab(makeToolPlaceholder("点击后加载使用分析工具..."), "📊 使用分析 / Analysis");
        toolsTabWidget->addTab(makeToolPlaceholder("点击后加载提示词模板库..."), "🧩 提示词模板 / Templates");
        toolsTabWidget->setTabPosition(QTabWidget::West);

        toolsTabWidget->setAutoFillBackground(true);
        QPalette pal = toolsTabWidget->palette();
        pal.setColor(QPalette::Window, QColor(AppStyle::SidebarDark));
        toolsTabWidget->setPalette(pal);

        // 核心动作：加入堆栈
        ui->rootStack->addWidget(toolsTabWidget);

        connect(toolsTabWidget, &QTabWidget::currentChanged, this, &MainWindow::ensureToolTabLoaded);
    }

    QAction *actTools = new QAction("🛠️ 工具箱 / Tools", this);
    actTools->setShortcut(QKeySequence("Ctrl+4"));
    connect(actTools, &QAction::triggered, this, [this](){
        ui->rootStack->setCurrentWidget(toolsTabWidget);
        if (toolsTabWidget) {
            ensureToolTabLoaded(toolsTabWidget->currentIndex());
        }
    });
    bar->addAction(actTools);

    // 6. 关于按钮
    QAction *btnAbout = new QAction("ℹ️ 关于 / About");
    btnAbout->setShortcut(QKeySequence("Ctrl+5"));
    connect(btnAbout, &QAction::triggered, this, &MainWindow::onMenuSwitchToAbout);
    bar->addAction(btnAbout);

    // 7. 强制显示 (防止被 hidden 属性隐藏)
    bar->setVisible(true);
}

void MainWindow::ensureToolTabLoaded(int index)
{
    if (!toolsTabWidget || index < 0 || index >= toolsTabWidget->count()) return;
    QWidget *currentPage = toolsTabWidget->widget(index);
    if (!currentPage || currentPage->objectName() != "toolPlaceholder") return;
    if (pendingToolTabLoads.contains(index)) return;

    pendingToolTabLoads.insert(index);
    QTimer::singleShot(0, this, [this, index]() {
        pendingToolTabLoads.remove(index);
        if (!toolsTabWidget || index < 0 || index >= toolsTabWidget->count()) return;
        QWidget *currentPage = toolsTabWidget->widget(index);
        if (!currentPage || currentPage->objectName() != "toolPlaceholder") return;

        QWidget *newPage = nullptr;
        switch (index) {
        case 0:
            newPage = new SyncWidget(toolsTabWidget);
            break;
        case 1:
            parserWidget = new PromptParserWidget(toolsTabWidget);
            parserWidget->setTranslationMap(&translationMap);
            newPage = parserWidget;
            break;
        case 2:
            tagBrowserWidget = new TagBrowserWidget(toolsTabWidget);
            tagBrowserWidget->setCsvPath(translationCsvPath);
            tagBrowserWidget->setMergedTranslationMap(&translationMap);
            connect(tagBrowserWidget, &TagBrowserWidget::csvSaved, this, [this](const QString &path){
                const QString normalized = QFileInfo(path).absoluteFilePath();
                if (!translationCsvPaths.contains(normalized)) translationCsvPaths.prepend(normalized);
                translationCsvPath = normalized;
                reloadTranslationMaps();
                applyPathListsToUi();
                saveGlobalConfig();
            });
            newPage = tagBrowserWidget;
            break;
        case 3:
            llmPromptWidget = new LlmPromptWidget(toolsTabWidget);
            llmPromptWidget->setLibraryPaths(
                collectEnabledPaths(loraPaths, disabledLoraPaths),
                collectEnabledPaths(galleryPaths, disabledGalleryPaths)
            );
            newPage = llmPromptWidget;
            break;
        case 4:
            usageAnalysisWidget = new UsageAnalysisWidget(toolsTabWidget);
            connect(usageAnalysisWidget, &UsageAnalysisWidget::requestRefresh,
                    this, &MainWindow::refreshUsageAnalysisWidget);
            connect(usageAnalysisWidget, &UsageAnalysisWidget::requestOpenModel,
                    this, &MainWindow::jumpToDownloadSource);
            refreshUsageAnalysisWidget();
            newPage = usageAnalysisWidget;
            break;
        case 5:
            promptTemplateLibraryWidget = new PromptTemplateLibraryWidget(toolsTabWidget);
            promptTemplateLibraryWidget->setTranslationMap(&translationMap);
            newPage = promptTemplateLibraryWidget;
            break;
        default:
            break;
        }

        if (!newPage) return;
        applyToolPageTheme(newPage);
        const QString label = toolsTabWidget->tabText(index);
        QSignalBlocker blocker(toolsTabWidget);
        toolsTabWidget->removeTab(index);
        currentPage->setParent(nullptr);
        currentPage->deleteLater();
        toolsTabWidget->insertTab(index, newPage, label);
        toolsTabWidget->setCurrentIndex(index);
    });
}

void MainWindow::onMenuSwitchToLibrary() {
    ui->rootStack->setCurrentIndex(0);
}

void MainWindow::onMenuSwitchToSettings() {
    ui->rootStack->setCurrentWidget(ui->pageSettings);
}

void MainWindow::onMenuSwitchToDownloads()
{
    ui->rootStack->setCurrentWidget(ui->pageDownloads);
    updateDownloadModelActionButtons();
    if (downloadManager && !downloadManager->cacheLoaded()) {
        downloadsPage->setStatusText("正在恢复上次下载列表...");
        QTimer::singleShot(0, this, [this]() {
            if (downloadManager) downloadManager->ensureCacheLoaded();
            updateDownloadSelectionSummary();
        });
        return;
    }
    QTimer::singleShot(0, this, &MainWindow::updateDownloadSelectionSummary);
}

void MainWindow::onTestCivitaiApiKeyClicked()
{
    if (settingsPage) optCivitaiApiKey = settingsPage->state().civitaiApiKey;
    saveGlobalConfig();
    if (optCivitaiApiKey.isEmpty()) {
        if (settingsPage) settingsPage->setCivitaiApiStatus("请先输入 Civitai API Key");
        return;
    }
    if (settingsPage) {
        settingsPage->setCivitaiApiStatus("正在测试 API Key...");
        settingsPage->setCivitaiApiTesting(true);
    }

    QNetworkRequest request = makeNetworkRequest(QUrl("https://civitai.com/api/v1/models?limit=1&favorites=true"));
    QNetworkReply *reply = netManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (settingsPage) settingsPage->setCivitaiApiTesting(false);
        if (reply->error() != QNetworkReply::NoError) {
            if (settingsPage) settingsPage->setCivitaiApiStatus("API Key 测试失败: " + civitaiNetworkErrorMessage(reply));
            return;
        }
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            if (settingsPage) settingsPage->setCivitaiApiStatus("API Key 测试失败: 返回不是有效 JSON");
            return;
        }
        if (settingsPage) settingsPage->setCivitaiApiStatus("API Key 可用");
    });
}

void MainWindow::initDownloadsPage()
{
    downloadsPage->initializeAppearance();
    downloadManager = new DownloadManager(downloadsPage, netManager, backgroundThreadPool, this);
    downloadManager->setPlaceholderIcon(placeholderIcon);
    downloadManager->setNetworkCallbacks(
        [this](const QUrl &url, bool allowCivitaiAuth) {
            return makeNetworkRequest(url, allowCivitaiAuth);
        },
        [this](QNetworkReply *reply) {
            return civitaiNetworkErrorMessage(reply);
        },
        [this](const QUrl &url) {
            return civitaiUrlWithToken(url);
        },
        [this]() {
            return civitaiApiKey();
        },
        [](const QString &filePath) {
            return FileUtils::calculateSha256Hex(filePath);
        });
    downloadManager->setTargetPathCallback([this](const ModelUpdateInfo &info, bool *overwrite) {
        return chooseModelDownloadTarget(info, overwrite);
    });
    downloadManager->setPreviewPathCallback([this](const ModelUpdateInfo &info) {
        return resolveDownloadPreviewPath(info);
    });
    connect(downloadManager, &DownloadManager::statusMessageChanged,
            downloadsPage, &DownloadsPage::setStatusText);
    connect(downloadManager, &DownloadManager::modelFileReady,
            this, &MainWindow::finishModelDownload);
    connect(ui->modelList, &QListWidget::itemSelectionChanged,
            this, &MainWindow::updateDownloadModelActionButtons);

    connect(downloadsPage->checkSelectedButton(), &QPushButton::clicked, this, [this]() {
        QList<QListWidgetItem*> items;
        const QStringList filePaths = downloadManager ? downloadManager->selectedFilePaths() : QStringList();
        for (const QString &filePath : filePaths) {
            if (QListWidgetItem *item = findModelItemByFilePath(filePath)) items << item;
        }
        checkUpdatesForItems(items);
    });
    connect(downloadsPage->checkAllButton(), &QPushButton::clicked, this, [this]() {
        QList<QListWidgetItem*> items;
        for (int i = 0; i < ui->modelList->count(); ++i) {
            QListWidgetItem *item = ui->modelList->item(i);
            if (isModelListItem(item)) items << item;
        }
        checkUpdatesForItems(items);
    });
    connect(downloadsPage->downloadSelectedButton(), &QPushButton::clicked, this, [this]() {
        if (downloadManager) downloadManager->startSelectedDownloads();
    });
    connect(downloadsPage->ignoreSelectedButton(), &QPushButton::clicked, this, [this]() {
        if (downloadManager) downloadManager->ignoreSelectedUpdates();
    });
    connect(downloadsPage->retryButton(), &QPushButton::clicked, this, [this]() {
        if (downloadManager) downloadManager->retryFailedDownloads();
    });
    connect(downloadsPage->openFolderButton(), &QPushButton::clicked, this, [this]() {
        const QStringList filePaths = downloadManager ? downloadManager->selectedFilePaths() : QStringList();
        if (filePaths.isEmpty()) return;
        const QString path = downloadsPage->cardTargetPath(filePaths.first());
        showFileInFolder(path);
    });
    connect(downloadsPage->clearCompletedButton(), &QPushButton::clicked, this, [this]() {
        if (downloadManager) downloadManager->clearCompleted();
    });
    connect(downloadsPage->filterCombo(), QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        if (downloadManager) downloadManager->filterCards();
    });
    connect(downloadsPage->statusTabs(), &QTabWidget::currentChanged, this, [this]() {
        updateDownloadSelectionSummary();
    });
    connect(downloadsPage->toggleCurrentTabButton(), &QPushButton::clicked, this, [this]() {
        downloadsPage->toggleCurrentTabSelection();
    });
    connect(downloadsPage->clearSelectionButton(), &QPushButton::clicked, this, [this]() {
        downloadsPage->clearAllCardSelection();
    });
    connect(downloadsPage, &DownloadsPage::cardSelectionChanged,
            this, &MainWindow::updateDownloadSelectionSummary);
    connect(downloadsPage, &DownloadsPage::sourceRequested,
            this, &MainWindow::jumpToDownloadSource);
    connect(downloadsPage, &DownloadsPage::civitaiRequested,
            this, &MainWindow::openDownloadCivitaiPage);
    connect(downloadsPage, &DownloadsPage::downloadRequested, this, [this](const QString &filePath) {
        if (!downloadManager || !downloadManager->containsInfo(filePath)) {
            downloadsPage->setStatusText("该模型还没有可下载的更新信息。");
            return;
        }
        const ModelUpdateInfo info = downloadManager->info(filePath);
        const QString status = downloadsPage->cardStatusText(filePath);
        if (!info.hasUpdate || status.contains("已忽略") || status.contains("已是最新") ||
            status.contains("失败") || status.contains("无法") || status.contains("错误") ||
            status.contains("出错") || status.contains("本地") || status.contains("跳过")) {
            if (QListWidgetItem *item = findModelItemByFilePath(filePath)) {
                QList<QListWidgetItem*> items;
                items << item;
                checkUpdatesForItems(items);
            } else {
                downloadsPage->setStatusText("模型已不在当前列表中，无法重新检测。");
            }
            return;
        }
        if (downloadManager) downloadManager->enqueueModelDownload(info);
    });
    connect(downloadsPage, &DownloadsPage::ignoreToggled, this, [this](const QString &filePath) {
        if (downloadManager) downloadManager->toggleIgnore(filePath);
    });
    connect(downloadsPage, &DownloadsPage::metadataScanRequested,
            this, &MainWindow::startMetadataScan);
    connect(downloadsPage, &DownloadsPage::metadataUpdateRequested,
            this, [this](const QStringList &paths) { startMetadataSyncForPaths(paths, true); });
    connect(downloadsPage, &DownloadsPage::metadataCivArchiveRequested,
            this, [this](const QStringList &paths) {
                if (paths.isEmpty()) {
                    downloadsPage->setStatusText("请先勾选要从 CivArchive 补充的模型。");
                    return;
                }
                if (metadataSyncRunning) {
                    downloadsPage->setStatusText("已有元信息同步任务正在运行。");
                    return;
                }

                bool hasLocalEdited = false;
                for (const QString &path : paths) {
                    if (QListWidgetItem *item = findModelItemByFilePath(path)) {
                        if (item->data(ROLE_LOCAL_EDITED).toBool()) {
                            hasLocalEdited = true;
                            break;
                        }
                    }
                }
                if (hasLocalEdited) {
                    const auto ret = QMessageBox::warning(this,
                                                          "覆盖本地元信息",
                                                          "选中的模型包含本地/已编辑 metadata。\n继续从 CivArchive 补充会覆盖元信息，但会保留本地保护字段和用户私有数据。\n是否继续？",
                                                          QMessageBox::Yes | QMessageBox::Cancel,
                                                          QMessageBox::Cancel);
                    if (ret != QMessageBox::Yes) return;
                }

                QMessageBox previewMsg(this);
                previewMsg.setWindowTitle("同步预览图");
                previewMsg.setText("从 CivArchive 补充元信息时是否同时同步预览图？\n\n"
                                   "选择“是”会在归档信息包含图片时同步预览图。\n"
                                   "选择“仅元数据”只更新 JSON，不下载或覆盖图片。");
                QPushButton *btnYes = previewMsg.addButton("是", QMessageBox::AcceptRole);
                QPushButton *btnMetaOnly = previewMsg.addButton("仅元数据", QMessageBox::ActionRole);
                QPushButton *btnCancel = previewMsg.addButton("取消同步", QMessageBox::RejectRole);
                previewMsg.setDefaultButton(btnMetaOnly);
                previewMsg.exec();
                if (previewMsg.clickedButton() == btnCancel) {
                    downloadsPage->setStatusText("已取消 CivArchive 元信息补充。");
                    return;
                }
                metadataSyncPreviewImages = (previewMsg.clickedButton() == btnYes);
                pendingMetadataSyncJobs.clear();
                for (const QString &path : paths) {
                    if (QListWidgetItem *item = findModelItemByFilePath(path)) {
                        MetadataSyncJob job;
                        job.snapshot = snapshotForModelItem(item);
                        job.updateExisting = true;
                        job.civArchiveOnly = true;
                        if (!job.snapshot.filePath.isEmpty()) pendingMetadataSyncJobs.enqueue(job);
                    }
                }
                metadataSyncTotal = pendingMetadataSyncJobs.size();
                metadataSyncDone = 0;
                metadataPreviewTasksPending = 0;
                metadataSyncWaitingForPreviews = false;
                metadataSyncRunning = metadataSyncTotal > 0;
                if (!metadataSyncRunning) {
                    metadataSyncPreviewImages = false;
                    downloadsPage->setStatusText("没有可从 CivArchive 补充的模型。");
                    return;
                }
                downloadsPage->setStatusText(QString("正在从 CivArchive 补充元信息... 0/%1").arg(metadataSyncTotal));
                processNextMetadataSyncJob();
            });
    connect(downloadsPage, &DownloadsPage::metadataOpenModelRequested,
            this, &MainWindow::jumpToDownloadSource);
    connect(downloadsPage, &DownloadsPage::metadataOpenFolderRequested, this, [this](const QString &filePath) {
        showFileInFolder(filePath);
    });
    connect(downloadsPage, &DownloadsPage::healthCheckRequested,
            this, &MainWindow::runMetadataHealthCheck);
    connect(downloadsPage, &DownloadsPage::healthOpenModelRequested,
            this, &MainWindow::jumpToDownloadSource);
    connect(downloadsPage, &DownloadsPage::healthOpenFolderRequested, this, [this](const QString &filePath) {
        showFileInFolder(filePath);
    });
    updateDownloadSelectionSummary();
    updateDownloadModelActionButtons();
}

void MainWindow::updateDownloadSelectionSummary()
{
    if (downloadManager) downloadManager->updateSelectionSummary();
    updateDownloadModelActionButtons();
}

void MainWindow::updateDownloadModelActionButtons()
{
    if (!downloadsPage || !ui || !ui->modelList) return;

    const bool hasCurrentModel = isModelListItem(ui->modelList->currentItem());
    bool hasSelectedModels = false;
    for (QListWidgetItem *item : ui->modelList->selectedItems()) {
        if (isModelListItem(item)) {
            hasSelectedModels = true;
            break;
        }
    }
    downloadsPage->setModelSelectionAvailability(hasCurrentModel, hasSelectedModels);
}

void MainWindow::checkUpdatesForItems(const QList<QListWidgetItem*> &items, bool switchToDownloads, bool detailPrompt)
{
    if (downloadManager && !downloadManager->cacheLoaded()) {
        downloadManager->ensureCacheLoaded();
    }
    if (items.isEmpty()) {
        if (downloadsPage) downloadsPage->setStatusText("没有可检查的模型。请先选择模型，或使用检查全部。");
        if (switchToDownloads) onMenuSwitchToDownloads();
        return;
    }

    if (switchToDownloads) onMenuSwitchToDownloads();
    ++updateCheckToken;
    detailUpdateCheckPending = detailPrompt && items.size() == 1;
    detailUpdateCheckFilePath = detailUpdateCheckPending
        ? QFileInfo(items.first()->data(ROLE_FILE_PATH).toString()).absoluteFilePath()
        : QString();
    pendingUpdateChecksQueue.clear();
    pendingUpdateHashChecks.clear();
    activeUpdateNetworkChecks = 0;
    activeUpdateHashChecks = 0;
    completedUpdateChecks = 0;
    downloadsPage->setUpdateCheckButtonsEnabled(false);
    for (QListWidgetItem *item : items) {
        if (!isModelListItem(item)) continue;
        UpdateCheckSnapshot snapshot;
        snapshot.filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
        snapshot.baseName = item->data(ROLE_MODEL_NAME).toString();
        snapshot.modelDir = QFileInfo(snapshot.filePath).absolutePath();
        snapshot.modelId = item->data(ROLE_CIVITAI_MODEL_ID).toInt();
        snapshot.displayName = item->text();
        snapshot.currentVersionId = item->data(ROLE_CIVITAI_VERSION_ID).toInt();
        snapshot.currentSha256 = item->data(ROLE_CIVITAI_SHA256).toString();
        snapshot.localEdited = item->data(ROLE_LOCAL_EDITED).toBool();
        pendingUpdateChecksQueue.enqueue(snapshot);
    }
    pendingUpdateChecks = pendingUpdateChecksQueue.size();
    if (pendingUpdateChecks <= 0) {
        downloadsPage->setStatusText("没有可检查的模型。");
        downloadsPage->setUpdateCheckButtonsEnabled(true);
        detailUpdateCheckPending = false;
        detailUpdateCheckFilePath.clear();
        return;
    }
    downloadsPage->setStatusText(QString("正在检查 %1 个模型的更新...").arg(pendingUpdateChecks));
    QTimer::singleShot(0, this, &MainWindow::dispatchQueuedUpdateChecks);
}

void MainWindow::checkUpdateForSnapshot(const UpdateCheckSnapshot &snapshot)
{
    if (snapshot.filePath.isEmpty()) {
        markUpdateCheckFinished();
        return;
    }

    ModelUpdateInfo pending;
    pending.filePath = snapshot.filePath;
    pending.modelDir = snapshot.modelDir;
    pending.baseName = snapshot.baseName;
    pending.displayName = snapshot.displayName;
    pending.modelId = snapshot.modelId;
    pending.currentVersionId = snapshot.currentVersionId;
    if (snapshot.localEdited) {
        addOrUpdateDownloadCard(pending, "本地/已编辑模型，已跳过");
        markUpdateCheckFinished();
        return;
    }
    addOrUpdateDownloadCard(pending, "检查中...");

    if (snapshot.modelId > 0) {
        QNetworkReply *reply = netManager->get(makeNetworkRequest(QUrl(QString("https://civitai.com/api/v1/models/%1").arg(snapshot.modelId))));
        reply->setProperty("filePath", snapshot.filePath);
        reply->setProperty("baseName", snapshot.baseName);
        reply->setProperty("modelDir", snapshot.modelDir);
        reply->setProperty("currentVersionId", pending.currentVersionId);
        reply->setProperty("currentSha256", snapshot.currentSha256);
        reply->setProperty("token", updateCheckToken);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleModelUpdateReply(reply); });
        return;
    }

    enqueueUpdateHashCheck(snapshot);
}

void MainWindow::dispatchQueuedUpdateChecks()
{
    constexpr int kMaxConcurrentUpdateChecks = 6;
    while (activeUpdateNetworkChecks < kMaxConcurrentUpdateChecks && !pendingUpdateChecksQueue.isEmpty()) {
        const UpdateCheckSnapshot snapshot = pendingUpdateChecksQueue.dequeue();
        ++activeUpdateNetworkChecks;
        checkUpdateForSnapshot(snapshot);
    }
}

void MainWindow::enqueueUpdateHashCheck(const UpdateCheckSnapshot &snapshot)
{
    pendingUpdateHashChecks.enqueue(snapshot);
    ModelUpdateInfo pending;
    pending.filePath = snapshot.filePath;
    pending.modelDir = snapshot.modelDir;
    pending.baseName = snapshot.baseName;
    pending.displayName = snapshot.displayName;
    addOrUpdateDownloadCard(pending, "计算 Hash 中...");
    dispatchUpdateHashChecks();
}

void MainWindow::dispatchUpdateHashChecks()
{
    constexpr int kMaxConcurrentHashChecks = 2;
    while (activeUpdateHashChecks < kMaxConcurrentHashChecks && !pendingUpdateHashChecks.isEmpty()) {
        const UpdateCheckSnapshot snapshot = pendingUpdateHashChecks.dequeue();
        ++activeUpdateHashChecks;
        auto *watcher = new QFutureWatcher<QString>(this);
        watcher->setProperty("token", updateCheckToken);
        watcher->setProperty("filePath", snapshot.filePath);
        watcher->setProperty("baseName", snapshot.baseName);
        watcher->setProperty("modelDir", snapshot.modelDir);
        watcher->setProperty("displayName", snapshot.displayName);
        connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher]() {
            const int token = watcher->property("token").toInt();
            UpdateCheckSnapshot snapshot;
            snapshot.filePath = watcher->property("filePath").toString();
            snapshot.baseName = watcher->property("baseName").toString();
            snapshot.modelDir = watcher->property("modelDir").toString();
            snapshot.displayName = watcher->property("displayName").toString();
            const QString hash = watcher->result();
            watcher->deleteLater();
            activeUpdateHashChecks = qMax(0, activeUpdateHashChecks - 1);

            if (token != updateCheckToken) {
                dispatchUpdateHashChecks();
                return;
            }

            ModelUpdateInfo pending;
            pending.filePath = snapshot.filePath;
            pending.modelDir = snapshot.modelDir;
            pending.baseName = snapshot.baseName;
            pending.displayName = snapshot.displayName;
            if (hash.isEmpty()) {
                addOrUpdateDownloadCard(pending, "无法计算 Hash，无法判断");
                markUpdateCheckFinished();
                dispatchUpdateHashChecks();
                return;
            }

            QNetworkReply *reply = netManager->get(makeNetworkRequest(QUrl(QString("https://civitai.com/api/v1/model-versions/by-hash/%1").arg(hash))));
            reply->setProperty("filePath", snapshot.filePath);
            reply->setProperty("baseName", snapshot.baseName);
            reply->setProperty("modelDir", snapshot.modelDir);
            reply->setProperty("currentSha256", hash);
            reply->setProperty("byHash", true);
            reply->setProperty("token", updateCheckToken);
            connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleModelUpdateReply(reply); });
            dispatchUpdateHashChecks();
        });
        watcher->setFuture(QtConcurrent::run(backgroundThreadPool, [filePath = snapshot.filePath]() {
            return FileUtils::calculateSha256Hex(filePath);
        }));
    }
}

void MainWindow::markUpdateCheckFinished()
{
    if (pendingUpdateChecks <= 0) return;
    activeUpdateNetworkChecks = qMax(0, activeUpdateNetworkChecks - 1);
    ++completedUpdateChecks;
    downloadsPage->setStatusText(QString("正在检查更新... %1/%2").arg(completedUpdateChecks).arg(pendingUpdateChecks));
    if (completedUpdateChecks >= pendingUpdateChecks && activeUpdateHashChecks == 0 && pendingUpdateHashChecks.isEmpty()) {
        downloadsPage->setStatusText(QString("更新检查完成，共 %1 个模型。").arg(pendingUpdateChecks));
        downloadsPage->setUpdateCheckButtonsEnabled(true);
        if (downloadManager) downloadManager->saveCache();
        if (detailUpdateCheckPending) {
            const QString filePath = detailUpdateCheckFilePath;
            detailUpdateCheckPending = false;
            detailUpdateCheckFilePath.clear();
            const QString status = downloadsPage ? downloadsPage->cardStatusText(filePath) : QString();
            const bool hasUpdate = downloadManager && downloadManager->containsInfo(filePath)
                && downloadManager->info(filePath).hasUpdate;
            const bool coexisting = status.contains("旧版共存");
            if (hasUpdate || coexisting) {
                const QMessageBox::StandardButton choice = QMessageBox::question(
                    this,
                    "检测到模型更新",
                    QString("检测到该模型有可用更新。\n\n状态: %1\n\n是否跳转到下载任务列表？")
                        .arg(status.isEmpty() ? "发现新版本" : status),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::Yes);
                if (choice == QMessageBox::Yes) {
                    onMenuSwitchToDownloads();
                    if (downloadsPage) downloadsPage->setCardSelected(filePath, true);
                    updateDownloadSelectionSummary();
                }
            } else {
                QMessageBox::information(
                    this,
                    "检查更新",
                    status.isEmpty() ? "当前模型没有检测到可用更新。" : QString("当前模型状态: %1").arg(status));
            }
        }
        return;
    }
    QTimer::singleShot(0, this, &MainWindow::dispatchQueuedUpdateChecks);
}

void MainWindow::handleModelUpdateReply(QNetworkReply *reply)
{
    const int token = reply->property("token").toInt();
    const QString filePath = reply->property("filePath").toString();
    const QString baseName = reply->property("baseName").toString();
    const QString modelDir = reply->property("modelDir").toString();
    const int currentVersionId = reply->property("currentVersionId").toInt();
    const QString currentSha256 = reply->property("currentSha256").toString();
    const bool byHash = reply->property("byHash").toBool();
    reply->deleteLater();
    if (token > 0 && token != updateCheckToken) return;

    ModelUpdateInfo fallback;
    fallback.filePath = filePath;
    fallback.baseName = baseName;
    fallback.modelDir = modelDir;
    fallback.displayName = QFileInfo(filePath).completeBaseName();

    if (reply->error() != QNetworkReply::NoError) {
        addOrUpdateDownloadCard(fallback, "检查失败: " + civitaiNetworkErrorMessage(reply));
        markUpdateCheckFinished();
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
    if (byHash) {
        const int modelId = root["modelId"].toInt();
        if (modelId <= 0) {
            addOrUpdateDownloadCard(fallback, "无法从 Hash 匹配 Civitai 模型");
            markUpdateCheckFinished();
            return;
        }
        QNetworkReply *detailReply = netManager->get(makeNetworkRequest(QUrl(QString("https://civitai.com/api/v1/models/%1").arg(modelId))));
        detailReply->setProperty("filePath", filePath);
        detailReply->setProperty("baseName", baseName);
        detailReply->setProperty("modelDir", modelDir);
        detailReply->setProperty("currentVersionId", root["id"].toInt());
        detailReply->setProperty("currentSha256", currentSha256);
        detailReply->setProperty("token", updateCheckToken);
        connect(detailReply, &QNetworkReply::finished, this, [this, detailReply]() { handleModelUpdateReply(detailReply); });
        return;
    }

    ModelUpdateInfo info;
    QListWidgetItem *sourceItem = nullptr;
    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (item && QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath() == filePath) {
            sourceItem = item;
            break;
        }
    }
    if (sourceItem) {
        sourceItem->setData(ROLE_CIVITAI_MODEL_ID, root["id"].toInt());
        if (sourceItem->data(ROLE_CIVITAI_VERSION_ID).toInt() <= 0 && currentVersionId > 0) {
            sourceItem->setData(ROLE_CIVITAI_VERSION_ID, currentVersionId);
        }
        sourceItem->setData(ROLE_MODEL_TYPE, root["type"].toString());
        applyCivitaiAttributionToItem(sourceItem,
                                      readModelCreatorFromJson(root),
                                      readModelTagsFromJson(root));
        info = parseModelUpdateInfo(sourceItem, root);
        if (downloadManager) downloadManager->setInfo(info);
    } else {
        fallback.modelId = root["id"].toInt();
        addOrUpdateDownloadCard(fallback, "模型列表项不存在，无法判断");
        markUpdateCheckFinished();
        return;
    }
    const QString status = info.latestFileExistsLocally
        ? "旧版共存：本地已存在新版本"
        : (info.hasUpdate ? "发现新版本" : "已是最新");
    addOrUpdateDownloadCard(info, status);
    markUpdateCheckFinished();
}

ModelUpdateInfo MainWindow::parseModelUpdateInfo(QListWidgetItem *item, const QJsonObject &modelRoot) const
{
    ModelUpdateInfo info;
    info.filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
    info.modelDir = QFileInfo(info.filePath).absolutePath();
    info.baseName = item->data(ROLE_MODEL_NAME).toString();
    info.displayName = item->text();
    info.modelId = modelRoot["id"].toInt(item->data(ROLE_CIVITAI_MODEL_ID).toInt());
    info.currentVersionId = item->data(ROLE_CIVITAI_VERSION_ID).toInt();
    const QString currentSha = item->data(ROLE_CIVITAI_SHA256).toString();
    info.metadataSource = modelRoot.value("metadataSource").toString();
    info.sourceUrl = modelRoot.value("sourceUrl").toString();

    QJsonArray versions = modelRoot["modelVersions"].toArray();
    QJsonObject currentVersionObj;
    QJsonObject latestVersionObj;
    QDateTime latestTime;
    for (const QJsonValue &val : versions) {
        const QJsonObject version = val.toObject();
        const int versionId = version["id"].toInt();
        if (versionId == info.currentVersionId) currentVersionObj = version;
        if (info.currentVersionId <= 0 && !currentSha.isEmpty()) {
            for (const QJsonValue &fileVal : version["files"].toArray()) {
                const QString sha = fileVal.toObject()["hashes"].toObject()["SHA256"].toString();
                if (!sha.isEmpty() && sha.compare(currentSha, Qt::CaseInsensitive) == 0) {
                    currentVersionObj = version;
                    info.currentVersionId = versionId;
                }
            }
        }
        QDateTime t = QDateTime::fromString(version["publishedAt"].toString(), Qt::ISODate);
        if (!t.isValid()) t = QDateTime::fromString(version["createdAt"].toString(), Qt::ISODate);
        if (!latestVersionObj.isEmpty() && (!t.isValid() || (latestTime.isValid() && t <= latestTime))) continue;
        latestVersionObj = version;
        latestTime = t;
    }

    if (latestVersionObj.isEmpty() && !versions.isEmpty()) latestVersionObj = versions.first().toObject();
    latestVersionObj = mergeCivitaiModelIntoVersion(latestVersionObj, modelRoot);
    if (!latestVersionObj.contains("modelId")) latestVersionObj["modelId"] = info.modelId;
    info.latestVersionJson = latestVersionObj;
    info.latestVersionId = latestVersionObj["id"].toInt();
    info.latestVersion = latestVersionObj["name"].toString();
    info.currentVersion = currentVersionObj["name"].toString();
    if (info.currentVersion.isEmpty()) info.currentVersion = QString("版本ID %1").arg(info.currentVersionId);
    if (info.latestVersion.isEmpty()) info.latestVersion = QString("版本ID %1").arg(info.latestVersionId);

    QJsonObject selectedFile;
    for (const QJsonValue &fileVal : latestVersionObj["files"].toArray()) {
        const QJsonObject f = fileVal.toObject();
        if (selectedFile.isEmpty() || f["primary"].toBool()) selectedFile = f;
        if (f["primary"].toBool()) break;
    }
    info.downloadUrl = selectedFile["downloadUrl"].toString();
    info.downloadFileName = selectedFile["name"].toString();
    info.sha256 = selectedFile["hashes"].toObject()["SHA256"].toString();
    if (info.sha256.isEmpty()) info.sha256 = currentSha;
    info.sizeMB = selectedFile["sizeKB"].toDouble() / 1024.0;
    info.hasUpdate = info.latestVersionId > 0 && info.latestVersionId != info.currentVersionId && !info.downloadUrl.isEmpty();
    if (info.hasUpdate && !info.downloadFileName.isEmpty()) {
        const QString latestLocalPath = QFileInfo(QDir(info.modelDir).filePath(info.downloadFileName)).absoluteFilePath();
        info.latestFileExistsLocally = QFile::exists(latestLocalPath)
                                      && latestLocalPath.compare(info.filePath, Qt::CaseInsensitive) != 0;
    }
    return info;
}

void MainWindow::addOrUpdateDownloadCard(const ModelUpdateInfo &info, const QString &status)
{
    if (info.filePath.isEmpty()) return;
    if (downloadManager) {
        downloadManager->addOrUpdateCard(info,
                                         status,
                                         findModelItemByFilePath(info.filePath) != nullptr || QFile::exists(info.filePath));
    }
}

QListWidgetItem *MainWindow::findModelItemByFilePath(const QString &filePath) const
{
    const QString normalized = QFileInfo(filePath).absoluteFilePath();
    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!isModelListItem(item)) continue;
        if (QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath() == normalized) {
            return item;
        }
    }
    return nullptr;
}

void MainWindow::jumpToDownloadSource(const QString &filePath)
{
    QListWidgetItem *item = findModelItemByFilePath(filePath);
    if (!item) {
        downloadsPage->setStatusText("模型已不在当前列表中，无法跳转。");
        return;
    }
    onMenuSwitchToLibrary();
    ui->modelList->setCurrentItem(item);
    item->setSelected(true);
    ui->modelList->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    onModelListClicked(item);
}

void MainWindow::openDownloadCivitaiPage(const QString &filePath)
{
    ModelUpdateInfo info = downloadManager ? downloadManager->info(filePath) : ModelUpdateInfo{};
    int modelId = info.modelId;
    QString sourceUrl = info.sourceUrl.trimmed();
    QString sha256 = info.sha256.trimmed();
    QString modelDir = info.modelDir;
    QString baseName = info.baseName;
    QJsonObject metadataRoot;

    if (QListWidgetItem *item = findModelItemByFilePath(filePath)) {
        if (modelId <= 0) modelId = item->data(ROLE_CIVITAI_MODEL_ID).toInt();
        if (sha256.isEmpty()) sha256 = item->data(ROLE_CIVITAI_SHA256).toString().trimmed();
        if (modelDir.isEmpty()) modelDir = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absolutePath();
        if (baseName.isEmpty()) baseName = item->data(ROLE_MODEL_NAME).toString();
    }

    if (!modelDir.isEmpty() && !baseName.isEmpty()) {
        QFile file(QDir(modelDir).filePath(baseName + ".json"));
        if (file.open(QIODevice::ReadOnly)) {
            metadataRoot = QJsonDocument::fromJson(file.readAll()).object();
            if (sourceUrl.isEmpty()) sourceUrl = metadataBrowserUrlFromRoot(metadataRoot);
            if (sha256.isEmpty()) sha256 = metadataShaFromRoot(metadataRoot);
            if (modelId <= 0) modelId = metadataRoot.value("model").toObject().value("id").toInt(metadataRoot.value("modelId").toInt());
        }
    }

    if (sourceUrl.contains("civarchive.com", Qt::CaseInsensitive)) {
        QDesktopServices::openUrl(QUrl(sourceUrl));
        return;
    }
    if (sourceUrl.isEmpty()
        && info.metadataSource.compare("civarchive", Qt::CaseInsensitive) == 0
        && !sha256.isEmpty()) {
        QString normalized = sha256;
        normalized.remove(QRegularExpression("[^A-Fa-f0-9]"));
        if (!normalized.isEmpty()) {
            QDesktopServices::openUrl(QUrl(QString("https://civarchive.com/sha256/%1").arg(normalized.toLower())));
            return;
        }
    }
    if (!sourceUrl.isEmpty() && modelId <= 0) {
        QDesktopServices::openUrl(QUrl(sourceUrl));
        return;
    }

    if (modelId <= 0) {
        downloadsPage->setStatusText("该任务没有可打开的 Civitai 模型 ID。");
        return;
    }
    QDesktopServices::openUrl(QUrl(QString("https://civitai.com/models/%1").arg(modelId)));
}

void MainWindow::showFileInFolder(const QString &filePath)
{
    FileUtils::showFileInFolder(filePath, this);
}

QString MainWindow::resolveDownloadPreviewPath(const ModelUpdateInfo &info) const
{
    if (QListWidgetItem *item = findModelItemByFilePath(info.filePath)) {
        const QString itemPreview = item->data(ROLE_PREVIEW_PATH).toString();
        if (!itemPreview.isEmpty() && QFile::exists(itemPreview)) return itemPreview;
    }
    const QString fallback = findLocalPreviewPath(info.modelDir, info.baseName, QString(), 0);
    if (!fallback.isEmpty() && QFile::exists(fallback)) return fallback;
    return QString();
}

UpdateCheckSnapshot MainWindow::snapshotForModelItem(QListWidgetItem *item) const
{
    UpdateCheckSnapshot snapshot;
    if (!isModelListItem(item)) return snapshot;
    snapshot.filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
    snapshot.baseName = item->data(ROLE_MODEL_NAME).toString();
    snapshot.modelDir = QFileInfo(snapshot.filePath).absolutePath();
    snapshot.modelId = item->data(ROLE_CIVITAI_MODEL_ID).toInt();
    snapshot.displayName = item->text();
    snapshot.currentVersionId = item->data(ROLE_CIVITAI_VERSION_ID).toInt();
    snapshot.currentSha256 = item->data(ROLE_CIVITAI_SHA256).toString();
    snapshot.localEdited = item->data(ROLE_LOCAL_EDITED).toBool();
    return snapshot;
}

QVector<MetadataScanItem> MainWindow::collectMetadataScanSeeds() const
{
    QVector<MetadataScanItem> items;
    items.reserve(ui->modelList->count());
    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *modelItem = ui->modelList->item(i);
        if (!isModelListItem(modelItem)) continue;
        const UpdateCheckSnapshot snapshot = snapshotForModelItem(modelItem);
        if (snapshot.filePath.isEmpty()) continue;

        MetadataScanItem item;
        item.filePath = snapshot.filePath;
        item.displayName = snapshot.displayName;
        item.jsonPath = QDir(snapshot.modelDir).filePath(snapshot.baseName + ".json");
        item.previewPath = modelItem->data(ROLE_PREVIEW_PATH).toString();
        item.modelIdText = snapshot.modelId > 0 ? QString::number(snapshot.modelId) : QString();
        item.versionIdText = snapshot.currentVersionId > 0 ? QString::number(snapshot.currentVersionId) : QString();
        item.sha256 = snapshot.currentSha256;
        item.localEdited = snapshot.localEdited;
        item.syncFailure = modelSyncFailureMessage(snapshot.filePath);
        items.append(item);
    }
    return items;
}

void MainWindow::startMetadataScan()
{
    if (!downloadsPage) return;
    if (!metadataScanWatcher) metadataScanWatcher = new QFutureWatcher<QVector<MetadataScanItem>>(this);
    if (metadataScanWatcher->isRunning()) return;

    const QVector<MetadataScanItem> seeds = collectMetadataScanSeeds();
    downloadsPage->setMetadataScanRunning(true);
    disconnect(metadataScanWatcher, nullptr, this, nullptr);
    connect(metadataScanWatcher, &QFutureWatcherBase::finished, this, [this]() {
        const QVector<MetadataScanItem> items = metadataScanWatcher->result();
        downloadsPage->setMetadataScanItems(items);
        downloadsPage->setMetadataScanRunning(false);
        downloadsPage->setStatusText(QString("元信息扫描完成，共 %1 个模型。").arg(items.size()));
    });
    metadataScanWatcher->setFuture(QtConcurrent::run(backgroundThreadPool, [seeds]() {
        return scanMetadataItemsWorker(seeds);
    }));
}

void MainWindow::runMetadataHealthCheck()
{
    if (!downloadsPage) return;
    if (!metadataHealthWatcher) metadataHealthWatcher = new QFutureWatcher<QVector<MetadataHealthIssue>>(this);
    if (metadataHealthWatcher->isRunning()) return;

    const QVector<MetadataScanItem> seeds = collectMetadataScanSeeds();
    downloadsPage->setHealthCheckRunning(true);
    disconnect(metadataHealthWatcher, nullptr, this, nullptr);
    connect(metadataHealthWatcher, &QFutureWatcherBase::finished, this, [this]() {
        const QVector<MetadataHealthIssue> issues = metadataHealthWatcher->result();
        downloadsPage->setHealthIssues(issues);
        downloadsPage->setHealthCheckRunning(false);
        downloadsPage->setStatusText(QString("元数据健康检查完成，共 %1 条问题/提示。").arg(issues.size()));
    });
    metadataHealthWatcher->setFuture(QtConcurrent::run(backgroundThreadPool, [seeds]() {
        return metadataHealthCheckWorker(seeds);
    }));
}

void MainWindow::startMetadataSyncForPaths(const QStringList &filePaths, bool updateExisting)
{
    if (!downloadsPage || filePaths.isEmpty()) {
        if (downloadsPage) downloadsPage->setStatusText("请先勾选要处理的模型。");
        return;
    }
    if (metadataSyncRunning) {
        downloadsPage->setStatusText("已有元信息同步任务正在运行。");
        return;
    }

    bool hasLocalEdited = false;
    for (const QString &path : filePaths) {
        if (QListWidgetItem *item = findModelItemByFilePath(path)) {
            if (item->data(ROLE_LOCAL_EDITED).toBool()) {
                hasLocalEdited = true;
                break;
            }
        }
    }
    if (hasLocalEdited) {
        const auto ret = QMessageBox::warning(this,
                                              "覆盖本地元信息",
                                              "选中的模型包含本地/已编辑 metadata。\n继续同步会覆盖 Civitai 元信息，但会保留本地保护字段和用户私有数据。\n是否继续？",
                                              QMessageBox::Yes | QMessageBox::Cancel,
                                              QMessageBox::Cancel);
        if (ret != QMessageBox::Yes) return;
    }

    QMessageBox previewMsg(this);
    previewMsg.setWindowTitle("同步预览图");
    previewMsg.setText("本次元信息同步是否同时同步 Civitai 预览图？\n\n"
                       "选择“是”会补齐缺失预览图，并尽量把提示词/参数写入本地预览图。\n"
                       "选择“仅元数据”只更新 JSON，不下载或覆盖图片。");
    QPushButton *btnYes = previewMsg.addButton("是", QMessageBox::AcceptRole);
    QPushButton *btnMetaOnly = previewMsg.addButton("仅元数据", QMessageBox::ActionRole);
    QPushButton *btnCancel = previewMsg.addButton("取消同步", QMessageBox::RejectRole);
    previewMsg.setDefaultButton(btnMetaOnly);
    previewMsg.exec();
    if (previewMsg.clickedButton() == btnCancel) {
        downloadsPage->setStatusText("已取消元信息同步。");
        return;
    }
    metadataSyncPreviewImages = (previewMsg.clickedButton() == btnYes);

    pendingMetadataSyncJobs.clear();
    for (const QString &path : filePaths) {
        QListWidgetItem *item = findModelItemByFilePath(path);
        if (!item) continue;
        MetadataSyncJob job;
        job.snapshot = snapshotForModelItem(item);
        job.updateExisting = updateExisting;
        if (!job.snapshot.filePath.isEmpty()) pendingMetadataSyncJobs.enqueue(job);
    }
    metadataSyncTotal = pendingMetadataSyncJobs.size();
    metadataSyncDone = 0;
    metadataPreviewTasksPending = 0;
    metadataSyncWaitingForPreviews = false;
    metadataSyncRunning = metadataSyncTotal > 0;
    if (!metadataSyncRunning) {
        metadataSyncPreviewImages = false;
        downloadsPage->setStatusText("没有可同步的模型。");
        return;
    }
    downloadsPage->setStatusText(QString("正在同步元信息... 0/%1").arg(metadataSyncTotal));
    processNextMetadataSyncJob();
}

void MainWindow::processNextMetadataSyncJob()
{
    if (!metadataSyncRunning) return;
    if (pendingMetadataSyncJobs.isEmpty()) {
        metadataSyncRunning = false;
        if (metadataSyncPreviewImages && metadataPreviewTasksPending > 0) {
            metadataSyncWaitingForPreviews = true;
            downloadsPage->setStatusText(QString("正在同步预览图元信息... 剩余 %1 个任务").arg(metadataPreviewTasksPending));
            return;
        }
        finishMetadataSyncBatch();
        return;
    }
    const MetadataSyncJob job = pendingMetadataSyncJobs.dequeue();
    downloadsPage->updateMetadataScanItemStatus(job.snapshot.filePath, "同步中...", QString());
    fetchMetadataForSyncJob(job);
}

void MainWindow::fetchMetadataForSyncJob(const MetadataSyncJob &job)
{
    if (job.civArchiveOnly) {
        fetchMetadataFromCivArchive(job, "手动从 CivArchive 补充", true);
        return;
    }

    if (job.snapshot.modelId > 0) {
        if (job.snapshot.currentVersionId > 0) {
            downloadsPage->updateMetadataScanItemStatus(job.snapshot.filePath, "正在获取完整版本元信息...", QString());
            QNetworkReply *reply = netManager->get(makeNetworkRequest(QUrl(QString("https://civitai.com/api/v1/model-versions/%1").arg(job.snapshot.currentVersionId))));
            reply->setProperty("filePath", job.snapshot.filePath);
            reply->setProperty("baseName", job.snapshot.baseName);
            reply->setProperty("modelDir", job.snapshot.modelDir);
            reply->setProperty("displayName", job.snapshot.displayName);
            reply->setProperty("modelId", job.snapshot.modelId);
            reply->setProperty("currentVersionId", job.snapshot.currentVersionId);
            reply->setProperty("currentSha256", job.snapshot.currentSha256);
            reply->setProperty("updateExisting", job.updateExisting);
            reply->setProperty("civArchiveOnly", job.civArchiveOnly);
            reply->setProperty("detailFallback", job.detailFallback);
            connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleMetadataSyncVersionReply(reply); });
            return;
        }

        QNetworkReply *reply = netManager->get(makeNetworkRequest(QUrl(QString("https://civitai.com/api/v1/models/%1").arg(job.snapshot.modelId))));
        reply->setProperty("filePath", job.snapshot.filePath);
        reply->setProperty("baseName", job.snapshot.baseName);
        reply->setProperty("modelDir", job.snapshot.modelDir);
        reply->setProperty("displayName", job.snapshot.displayName);
        reply->setProperty("modelId", job.snapshot.modelId);
        reply->setProperty("currentVersionId", job.snapshot.currentVersionId);
        reply->setProperty("currentSha256", job.snapshot.currentSha256);
        reply->setProperty("updateExisting", job.updateExisting);
        reply->setProperty("civArchiveOnly", job.civArchiveOnly);
        reply->setProperty("detailFallback", job.detailFallback);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleMetadataSyncModelReply(reply); });
        return;
    }

    const QString cachedHash = job.snapshot.currentSha256.trimmed();
    if (!optRecalculateKnownMetadataHash && !cachedHash.isEmpty()) {
        downloadsPage->updateMetadataScanItemStatus(job.snapshot.filePath, "使用缓存 Hash 同步中...", QString());
        QNetworkReply *reply = netManager->get(makeNetworkRequest(QUrl(QString("https://civitai.com/api/v1/model-versions/by-hash/%1").arg(cachedHash))));
        reply->setProperty("filePath", job.snapshot.filePath);
        reply->setProperty("baseName", job.snapshot.baseName);
        reply->setProperty("modelDir", job.snapshot.modelDir);
        reply->setProperty("displayName", job.snapshot.displayName);
        reply->setProperty("currentSha256", cachedHash);
        reply->setProperty("updateExisting", job.updateExisting);
        reply->setProperty("civArchiveOnly", job.civArchiveOnly);
        reply->setProperty("detailFallback", job.detailFallback);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleMetadataSyncHashReply(reply); });
        return;
    }

    downloadsPage->updateMetadataScanItemStatus(job.snapshot.filePath, "正在计算 Hash...", QString());
    auto *watcher = new QFutureWatcher<QString>(this);
    watcher->setProperty("filePath", job.snapshot.filePath);
    watcher->setProperty("baseName", job.snapshot.baseName);
    watcher->setProperty("modelDir", job.snapshot.modelDir);
    watcher->setProperty("displayName", job.snapshot.displayName);
    watcher->setProperty("updateExisting", job.updateExisting);
    watcher->setProperty("civArchiveOnly", job.civArchiveOnly);
    watcher->setProperty("detailFallback", job.detailFallback);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher]() {
        MetadataSyncJob job;
        job.snapshot.filePath = watcher->property("filePath").toString();
        job.snapshot.baseName = watcher->property("baseName").toString();
        job.snapshot.modelDir = watcher->property("modelDir").toString();
        job.snapshot.displayName = watcher->property("displayName").toString();
        job.updateExisting = watcher->property("updateExisting").toBool();
        job.civArchiveOnly = watcher->property("civArchiveOnly").toBool();
        job.detailFallback = watcher->property("detailFallback").toBool();
        const QString hash = watcher->result();
        watcher->deleteLater();
        if (hash.isEmpty()) {
            fetchMetadataFromCivArchive(job, "无法计算 Hash，尝试使用 ID 查询 CivArchive");
            return;
        }
        job.snapshot.currentSha256 = hash;
        QNetworkReply *reply = netManager->get(makeNetworkRequest(QUrl(QString("https://civitai.com/api/v1/model-versions/by-hash/%1").arg(hash))));
        reply->setProperty("filePath", job.snapshot.filePath);
        reply->setProperty("baseName", job.snapshot.baseName);
        reply->setProperty("modelDir", job.snapshot.modelDir);
        reply->setProperty("displayName", job.snapshot.displayName);
        reply->setProperty("currentSha256", hash);
        reply->setProperty("updateExisting", job.updateExisting);
        reply->setProperty("civArchiveOnly", job.civArchiveOnly);
        reply->setProperty("detailFallback", job.detailFallback);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleMetadataSyncHashReply(reply); });
    });
    watcher->setFuture(QtConcurrent::run(backgroundThreadPool, [filePath = job.snapshot.filePath]() {
        return FileUtils::calculateSha256Hex(filePath);
    }));
}

void MainWindow::handleMetadataSyncVersionReply(QNetworkReply *reply)
{
    MetadataSyncJob job;
    job.snapshot.filePath = reply->property("filePath").toString();
    job.snapshot.baseName = reply->property("baseName").toString();
    job.snapshot.modelDir = reply->property("modelDir").toString();
    job.snapshot.displayName = reply->property("displayName").toString();
    job.snapshot.modelId = reply->property("modelId").toInt();
    job.snapshot.currentVersionId = reply->property("currentVersionId").toInt();
    job.snapshot.currentSha256 = reply->property("currentSha256").toString();
    job.updateExisting = reply->property("updateExisting").toBool();
    job.civArchiveOnly = reply->property("civArchiveOnly").toBool();
    job.detailFallback = reply->property("detailFallback").toBool();
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        fetchMetadataFromCivArchive(job, "版本元信息获取失败: " + civitaiNetworkErrorMessage(reply));
        return;
    }

    const QJsonObject versionRoot = QJsonDocument::fromJson(reply->readAll()).object();
    const int modelId = versionRoot.value("modelId").toInt(job.snapshot.modelId);
    job.snapshot.currentVersionId = versionRoot.value("id").toInt(job.snapshot.currentVersionId);
    if (modelId <= 0) {
        fetchMetadataFromCivArchive(job, "版本元信息缺少模型 ID");
        return;
    }

    QNetworkReply *detailReply = netManager->get(makeNetworkRequest(QUrl(QString("https://civitai.com/api/v1/models/%1").arg(modelId))));
    detailReply->setProperty("filePath", job.snapshot.filePath);
    detailReply->setProperty("baseName", job.snapshot.baseName);
    detailReply->setProperty("modelDir", job.snapshot.modelDir);
    detailReply->setProperty("displayName", job.snapshot.displayName);
    detailReply->setProperty("currentVersionId", job.snapshot.currentVersionId);
    detailReply->setProperty("currentSha256", job.snapshot.currentSha256);
    detailReply->setProperty("updateExisting", job.updateExisting);
    detailReply->setProperty("civArchiveOnly", job.civArchiveOnly);
    detailReply->setProperty("detailFallback", job.detailFallback);
    detailReply->setProperty("versionHint", QJsonDocument(versionRoot).toJson(QJsonDocument::Compact));
    connect(detailReply, &QNetworkReply::finished, this, [this, detailReply]() { handleMetadataSyncModelReply(detailReply); });
}

void MainWindow::handleMetadataSyncHashReply(QNetworkReply *reply)
{
    MetadataSyncJob job;
    job.snapshot.filePath = reply->property("filePath").toString();
    job.snapshot.baseName = reply->property("baseName").toString();
    job.snapshot.modelDir = reply->property("modelDir").toString();
    job.snapshot.displayName = reply->property("displayName").toString();
    job.snapshot.currentSha256 = reply->property("currentSha256").toString();
    job.updateExisting = reply->property("updateExisting").toBool();
    job.civArchiveOnly = reply->property("civArchiveOnly").toBool();
    job.detailFallback = reply->property("detailFallback").toBool();
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        fetchMetadataFromCivArchive(job, "Hash 匹配失败: " + civitaiNetworkErrorMessage(reply));
        return;
    }

    const QJsonObject versionRoot = QJsonDocument::fromJson(reply->readAll()).object();
    const int modelId = versionRoot.value("modelId").toInt();
    job.snapshot.currentVersionId = versionRoot.value("id").toInt();
    if (modelId <= 0) {
        fetchMetadataFromCivArchive(job, "无法从 Hash 匹配 Civitai 模型");
        return;
    }

    QNetworkReply *detailReply = netManager->get(makeNetworkRequest(QUrl(QString("https://civitai.com/api/v1/models/%1").arg(modelId))));
    detailReply->setProperty("filePath", job.snapshot.filePath);
    detailReply->setProperty("baseName", job.snapshot.baseName);
    detailReply->setProperty("modelDir", job.snapshot.modelDir);
    detailReply->setProperty("displayName", job.snapshot.displayName);
    detailReply->setProperty("currentVersionId", job.snapshot.currentVersionId);
    detailReply->setProperty("currentSha256", job.snapshot.currentSha256);
    detailReply->setProperty("updateExisting", job.updateExisting);
    detailReply->setProperty("civArchiveOnly", job.civArchiveOnly);
    detailReply->setProperty("detailFallback", job.detailFallback);
    detailReply->setProperty("versionHint", QJsonDocument(versionRoot).toJson(QJsonDocument::Compact));
    connect(detailReply, &QNetworkReply::finished, this, [this, detailReply]() { handleMetadataSyncModelReply(detailReply); });
}

void MainWindow::handleMetadataSyncModelReply(QNetworkReply *reply)
{
    MetadataSyncJob job;
    job.snapshot.filePath = reply->property("filePath").toString();
    job.snapshot.baseName = reply->property("baseName").toString();
    job.snapshot.modelDir = reply->property("modelDir").toString();
    job.snapshot.displayName = reply->property("displayName").toString();
    job.snapshot.currentVersionId = reply->property("currentVersionId").toInt();
    job.snapshot.currentSha256 = reply->property("currentSha256").toString();
    job.updateExisting = reply->property("updateExisting").toBool();
    job.civArchiveOnly = reply->property("civArchiveOnly").toBool();
    job.detailFallback = reply->property("detailFallback").toBool();
    const QJsonObject versionHint = QJsonDocument::fromJson(reply->property("versionHint").toByteArray()).object();
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        fetchMetadataFromCivArchive(job, "同步失败: " + civitaiNetworkErrorMessage(reply));
        return;
    }

    const QJsonObject modelRoot = QJsonDocument::fromJson(reply->readAll()).object();
    const bool ok = saveMetadataFromModelRoot(job, modelRoot, versionHint);
    if (!ok && !job.civArchiveOnly) {
        fetchMetadataFromCivArchive(job, "Civitai 返回成功但无法匹配当前版本");
        return;
    }
    if (!ok) {
        finishMetadataSyncJobWithFailure(job, "同步失败: 未找到可保存的版本");
        return;
    }
    const QString nowText = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm");
    downloadsPage->updateMetadataScanItemStatus(job.snapshot.filePath,
                                                "metadata 已同步",
                                                "existing",
                                                nowText,
                                                "同步时间");
    ++metadataSyncDone;
    downloadsPage->setStatusText(QString("正在同步元信息... %1/%2").arg(metadataSyncDone).arg(metadataSyncTotal));
    processNextMetadataSyncJob();
}

bool MainWindow::tryStartCivArchiveHashCalculation(const MetadataSyncJob &job, const QString &reason)
{
    if (job.snapshot.filePath.isEmpty() || !QFileInfo::exists(job.snapshot.filePath)) return false;
    if (!job.snapshot.currentSha256.trimmed().isEmpty()) return false;
    if (!optRecalculateKnownMetadataHash && !job.civArchiveOnly) return false;

    downloadsPage->updateMetadataScanItemStatus(job.snapshot.filePath, "正在计算 Hash 以查询 CivArchive...", QString());
    auto *watcher = new QFutureWatcher<QString>(this);
    watcher->setProperty("filePath", job.snapshot.filePath);
    watcher->setProperty("baseName", job.snapshot.baseName);
    watcher->setProperty("modelDir", job.snapshot.modelDir);
    watcher->setProperty("displayName", job.snapshot.displayName);
    watcher->setProperty("modelId", job.snapshot.modelId);
    watcher->setProperty("currentVersionId", job.snapshot.currentVersionId);
    watcher->setProperty("updateExisting", job.updateExisting);
    watcher->setProperty("civArchiveOnly", job.civArchiveOnly);
    watcher->setProperty("detailFallback", job.detailFallback);
    watcher->setProperty("reason", reason);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher]() {
        MetadataSyncJob retryJob;
        retryJob.snapshot.filePath = watcher->property("filePath").toString();
        retryJob.snapshot.baseName = watcher->property("baseName").toString();
        retryJob.snapshot.modelDir = watcher->property("modelDir").toString();
        retryJob.snapshot.displayName = watcher->property("displayName").toString();
        retryJob.snapshot.modelId = watcher->property("modelId").toInt();
        retryJob.snapshot.currentVersionId = watcher->property("currentVersionId").toInt();
        retryJob.snapshot.currentSha256 = watcher->result();
        retryJob.updateExisting = watcher->property("updateExisting").toBool();
        retryJob.civArchiveOnly = watcher->property("civArchiveOnly").toBool();
        retryJob.detailFallback = watcher->property("detailFallback").toBool();
        const QString reason = watcher->property("reason").toString();
        watcher->deleteLater();
        fetchMetadataFromCivArchive(retryJob, reason);
    });
    watcher->setFuture(QtConcurrent::run(backgroundThreadPool, [filePath = job.snapshot.filePath]() {
        return FileUtils::calculateSha256Hex(filePath);
    }));
    return true;
}

void MainWindow::fetchMetadataFromCivArchive(const MetadataSyncJob &job, const QString &reason, bool directOnly)
{
    if (!downloadsPage) return;
    if (!directOnly && !job.civArchiveOnly && !optTryCivArchiveOnMetadataFail) {
        finishMetadataSyncJobWithFailure(job, reason);
        return;
    }

    MetadataSyncJob lookupJob = job;
    QUrl url = civArchiveLookupUrl(lookupJob);
    if (!url.isValid()) {
        if (tryStartCivArchiveHashCalculation(job, reason)) return;
        finishMetadataSyncJobWithFailure(job, reason + "；CivArchive 查询缺少 Hash 或模型 ID", "no_ids");
        return;
    }

    downloadsPage->updateMetadataScanItemStatus(job.snapshot.filePath, "正在查询 CivArchive...", QString());
    QNetworkReply *reply = netManager->get(makeNetworkRequest(url, false));
    reply->setProperty("filePath", lookupJob.snapshot.filePath);
    reply->setProperty("baseName", lookupJob.snapshot.baseName);
    reply->setProperty("modelDir", lookupJob.snapshot.modelDir);
    reply->setProperty("displayName", lookupJob.snapshot.displayName);
    reply->setProperty("modelId", lookupJob.snapshot.modelId);
    reply->setProperty("currentVersionId", lookupJob.snapshot.currentVersionId);
    reply->setProperty("currentSha256", lookupJob.snapshot.currentSha256);
    reply->setProperty("updateExisting", lookupJob.updateExisting);
    reply->setProperty("civArchiveOnly", lookupJob.civArchiveOnly);
    reply->setProperty("detailFallback", lookupJob.detailFallback);
    reply->setProperty("civArchiveReason", reason);
    reply->setProperty("civArchiveUrl", url.toString());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        handleMetadataSyncCivArchiveReply(reply);
    });
}

bool MainWindow::startDetailCivArchiveFallback(const QString &filePath, const QString &baseName, const QString &modelDir, const QString &reason, const QString &currentSha256)
{
    if (!optTryCivArchiveOnMetadataFail || filePath.isEmpty()) return false;
    MetadataSyncJob job;
    if (QListWidgetItem *item = findModelItemByFilePath(filePath)) {
        job.snapshot = snapshotForModelItem(item);
    }
    job.snapshot.filePath = filePath;
    job.snapshot.baseName = baseName;
    job.snapshot.modelDir = modelDir.isEmpty() ? QFileInfo(filePath).absolutePath() : modelDir;
    if (!currentSha256.trimmed().isEmpty()) job.snapshot.currentSha256 = currentSha256.trimmed();
    if (job.snapshot.displayName.isEmpty()) job.snapshot.displayName = QFileInfo(filePath).completeBaseName();
    job.updateExisting = true;
    job.detailFallback = true;
    metadataSyncTotal = qMax(metadataSyncTotal, 1);
    metadataSyncDone = 0;
    metadataSyncRunning = false;
    metadataSyncPreviewImages = !m_skipPreviewSync;
    fetchMetadataFromCivArchive(job, reason);
    return true;
}

void MainWindow::handleMetadataSyncCivArchiveReply(QNetworkReply *reply)
{
    MetadataSyncJob job;
    job.snapshot.filePath = reply->property("filePath").toString();
    job.snapshot.baseName = reply->property("baseName").toString();
    job.snapshot.modelDir = reply->property("modelDir").toString();
    job.snapshot.displayName = reply->property("displayName").toString();
    job.snapshot.modelId = reply->property("modelId").toInt();
    job.snapshot.currentVersionId = reply->property("currentVersionId").toInt();
    job.snapshot.currentSha256 = reply->property("currentSha256").toString();
    job.updateExisting = reply->property("updateExisting").toBool();
    job.civArchiveOnly = reply->property("civArchiveOnly").toBool();
    job.detailFallback = reply->property("detailFallback").toBool();
    const QString reason = reply->property("civArchiveReason").toString();
    const QString sourceUrl = reply->property("civArchiveUrl").toString();
    const QByteArray body = reply->readAll();
    const QString networkError = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    if (!networkError.isEmpty()) {
        QString message = reason + QString("；CivArchive 查询失败: %1").arg(networkError);
        if (status > 0) message += QString(" (HTTP %1)").arg(status);
        finishMetadataSyncJobWithFailure(job, message);
        return;
    }

    QJsonObject modelRoot;
    QJsonObject versionHint;
    if (!parseCivArchivePayload(body, sourceUrl, modelRoot, versionHint)) {
        finishMetadataSyncJobWithFailure(job, reason + "；CivArchive 返回内容中未找到可识别的模型元信息");
        return;
    }

    const bool ok = saveMetadataFromModelRoot(job, modelRoot, versionHint);
    if (!ok) {
        finishMetadataSyncJobWithFailure(job, reason + "；CivArchive 元信息无法匹配当前模型版本");
        return;
    }

    const QString nowText = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm");
    downloadsPage->updateMetadataScanItemStatus(job.snapshot.filePath,
                                                "metadata 已从 CivArchive 补充",
                                                "existing",
                                                nowText,
                                                "CivArchive");
    ++metadataSyncDone;
    downloadsPage->setStatusText(QString("正在同步元信息... %1/%2").arg(metadataSyncDone).arg(metadataSyncTotal));
    processNextMetadataSyncJob();
}

void MainWindow::finishMetadataSyncJobWithFailure(const MetadataSyncJob &job, const QString &message, const QString &category)
{
    if (downloadsPage) {
        downloadsPage->updateMetadataScanItemStatus(job.snapshot.filePath, message, category);
        downloadsPage->setStatusText(QString("正在同步元信息... %1/%2").arg(metadataSyncDone + 1).arg(metadataSyncTotal));
    }
    recordModelSyncFailure(job.snapshot.filePath, job.snapshot.baseName, message);
    if (job.detailFallback) {
        setModelTitleError(message);
        ui->textDescription->setPlainText(QString("上次同步失败，请点击刷新模型详情重新同步。\n%1").arg(message));
        transitionToImage("");
        ui->statusbar->showMessage("元数据获取失败: " + message, 4000);
        m_forceResyncPreview = false;
        m_skipPreviewSync = false;
    }
    ++metadataSyncDone;
    processNextMetadataSyncJob();
}

bool MainWindow::saveMetadataFromModelRoot(const MetadataSyncJob &job, const QJsonObject &modelRoot, const QJsonObject &versionHint)
{
    if (modelRoot.isEmpty()) return false;
    QJsonObject selectedVersion = versionHint;
    const QJsonArray versions = modelRoot.value("modelVersions").toArray();
    if (selectedVersion.isEmpty()) {
        for (const QJsonValue &val : versions) {
            const QJsonObject version = val.toObject();
            bool match = false;
            if (job.snapshot.currentVersionId > 0 && version.value("id").toInt() == job.snapshot.currentVersionId) {
                match = true;
            }
            if (!match && !job.snapshot.currentSha256.isEmpty()) {
                for (const QJsonValue &fileVal : version.value("files").toArray()) {
                    const QString sha = fileVal.toObject().value("hashes").toObject().value("SHA256").toString();
                    if (!sha.isEmpty() && sha.compare(job.snapshot.currentSha256, Qt::CaseInsensitive) == 0) {
                        match = true;
                        break;
                    }
                }
            }
            if (match) {
                selectedVersion = version;
                break;
            }
        }
    }
    if (selectedVersion.isEmpty() && !job.updateExisting && !versions.isEmpty()) selectedVersion = versions.first().toObject();
    if (selectedVersion.isEmpty()) return false;

    QJsonObject root = mergeCivitaiModelIntoVersion(selectedVersion, modelRoot);
    root["syncedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    if (modelRoot.value("metadataSource").toString() == "civarchive") {
        root["metadataSource"] = QStringLiteral("civarchive");
        if (modelRoot.contains("sourceUrl")) root["sourceUrl"] = modelRoot.value("sourceUrl");
    } else if (!root.contains("metadataSource")) {
        root["metadataSource"] = QStringLiteral("civitai");
    }
    QFile existing(QDir(job.snapshot.modelDir).filePath(job.snapshot.baseName + ".json"));
    if (existing.exists() && existing.open(QIODevice::ReadOnly)) {
        const QJsonObject oldRoot = QJsonDocument::fromJson(existing.readAll()).object();
        const QStringList localKeys = {"localEdited", "localEditedAt", "localOnly", "modelUrl"};
        for (const QString &key : localKeys) {
            if (oldRoot.contains(key)) root[key] = oldRoot.value(key);
        }
    } else {
        root["localOnly"] = false;
    }
    saveLocalMetadata(job.snapshot.modelDir, job.snapshot.baseName, root);
    if (metadataSyncPreviewImages) {
        syncPreviewImagesFromMetadata(job.snapshot.modelDir,
                                      job.snapshot.baseName,
                                      imageInfosFromVersionJson(root),
                                      true,
                                      true);
    }

    if (QListWidgetItem *item = findModelItemByFilePath(job.snapshot.filePath)) {
        preloadItemMetadata(item, QDir(job.snapshot.modelDir).filePath(job.snapshot.baseName + ".json"));
        if (optUseCivitaiName) {
            const QString civitaiName = item->data(ROLE_CIVITAI_NAME).toString();
            if (!civitaiName.isEmpty()) item->setText(civitaiName);
        }
        applyModelUserNoteData(item);
        applyModelHighlightColor(item);
    }
    clearModelSyncFailure(job.snapshot.filePath);
    if (!metadataSyncRunning) {
        if (QListWidgetItem *current = ui->modelList->currentItem()) {
            if (current && QFileInfo(current->data(ROLE_FILE_PATH).toString()).absoluteFilePath() == job.snapshot.filePath) {
                ModelMeta meta;
                meta.filePath = job.snapshot.filePath;
                if (readLocalJson(job.snapshot.modelDir, job.snapshot.baseName, meta)) updateDetailView(meta);
            }
        }
        refreshHomeGallery();
        refreshCollectionTreeView();
    }
    return true;
}

QString MainWindow::chooseModelDownloadTarget(const ModelUpdateInfo &info, bool *overwrite)
{
    if (overwrite) *overwrite = false;
    int policy = optModelUpdateDownloadPolicy;
    if (policy == 0) {
        QMessageBox msg(this);
        msg.setWindowTitle("下载新版本");
        msg.setText(QString("如何保存 %1 的新版本？").arg(info.displayName));
        QPushButton *keepBtn = msg.addButton("保留旧版", QMessageBox::AcceptRole);
        QPushButton *replaceBtn = msg.addButton("覆盖当前文件", QMessageBox::DestructiveRole);
        msg.addButton("取消", QMessageBox::RejectRole);
        msg.exec();
        if (msg.clickedButton() == keepBtn) policy = 1;
        else if (msg.clickedButton() == replaceBtn) policy = 2;
        else return QString();
    }
    QString fileName = info.downloadFileName;
    if (fileName.isEmpty()) fileName = QFileInfo(info.filePath).fileName();
    if (policy == 2) {
        if (overwrite) *overwrite = true;
        return QFileInfo(info.filePath).absoluteFilePath();
    }
    return uniqueFilePath(info.modelDir, fileName);
}

QString MainWindow::uniqueFilePath(const QString &dirPath, const QString &fileName) const
{
    QDir dir(dirPath);
    QString base = QFileInfo(fileName).completeBaseName();
    QString suffix = QFileInfo(fileName).suffix();
    QString candidate = dir.filePath(fileName);
    int index = 1;
    while (QFile::exists(candidate)) {
        const QString nextName = suffix.isEmpty()
            ? QString("%1_%2").arg(base).arg(index)
            : QString("%1_%2.%3").arg(base).arg(index).arg(suffix);
        candidate = dir.filePath(nextName);
        ++index;
    }
    return QFileInfo(candidate).absoluteFilePath();
}

void MainWindow::finishModelDownload(const ModelFileDownloadTask &task)
{
    QString baseName = QFileInfo(task.targetPath).completeBaseName();
    QJsonObject versionJson = task.info.latestVersionJson;
    if (!versionJson.isEmpty()) {
        versionJson["syncedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        saveLocalMetadata(QFileInfo(task.targetPath).absolutePath(), baseName, versionJson);
        const QJsonArray images = versionJson["images"].toArray();
        for (const QJsonValue &val : images) {
            const QJsonObject img = val.toObject();
            const QString type = img["type"].toString();
            const QString url = img["url"].toString();
            if (url.isEmpty() || type == "video" || url.endsWith(".mp4", Qt::CaseInsensitive) || url.endsWith(".webm", Qt::CaseInsensitive)) {
                continue;
            }
            const QString previewPath = QDir(QFileInfo(task.targetPath).absolutePath()).filePath(baseName + ".preview.png");
            if (!QFile::exists(previewPath)) downloadThumbnail(url, previewPath, nullptr);
            break;
        }
    }
    if (downloadManager) downloadManager->saveCache();
    downloadsPage->setStatusText("模型下载完成: " + QFileInfo(task.targetPath).fileName());

    const QStringList activeLoraPaths = collectEnabledPaths(loraPaths, disabledLoraPaths);
    if (!activeLoraPaths.isEmpty()) scanModels(activeLoraPaths);
}

// === 全局配置加载与保存 (JSON) ===
void MainWindow::loadGlobalConfig() {
    QString configPath = qApp->applicationDirPath() + "/config/settings.json";
    QFile file(configPath);
    SettingsState settings;
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        QJsonObject root = doc.object();

        settings = SettingsState::fromJson(root, DEFAULT_FILTER_TAGS);
        optUiScale = settings.uiScale;
        optFilterNSFW = settings.filterNSFW;
        optNSFWMode = settings.nsfwMode;
        optNSFWLevel = settings.nsfwLevel;
        optLoraRecursive = settings.loraRecursive;
        optGalleryRecursive = settings.galleryRecursive;
        optBlurRadius = settings.blurRadius;
        optDownscaleBlur = settings.downscaleBlur;
        optBlurProcessWidth = settings.blurProcessWidth;
        optRenderThreadCount = settings.renderThreadCount;
        optRestoreTreeState = settings.restoreTreeState;
        optSplitOnNewline = settings.splitOnNewline;
        optFilterTags = settings.filterTags();
        optShowEmptyCollections = settings.showEmptyCollections;
        optCollectionFolderTopLevel = settings.collectionFolderTopLevel;
        optCollectionFolderSecondLevel = settings.collectionFolderSecondLevel;
        optModelListFolderGrouping = settings.modelListFolderGrouping;
        optUseArrangedUA = settings.useCustomUserAgent;
        optSavedUAString = settings.customUserAgent;
        optCivitaiApiKey = settings.civitaiApiKey;
        optUseCivitaiName = settings.useCivitaiName;
        optSuppressLocalWarnings = settings.suppressLocalWarnings;
        optUserGalleryMatchMode = settings.userGalleryMatchMode;
        optRecalculateKnownMetadataHash = settings.recalculateKnownMetadataHash;
        optTryCivArchiveOnMetadataFail = settings.tryCivArchiveOnMetadataFail;
        optModelUpdateDownloadPolicy = settings.modelUpdateDownloadPolicy;
        optAutoCheckUpdatesOnStartup = settings.autoCheckUpdatesOnStartup;
        optThemeId = settings.themeId;
        optCustomThemePath = settings.customThemePath;

        qDebug() << "Loaded User-Agent:" << currentUserAgent;

        auto readPathList = [](const QJsonObject &obj, const QString &arrayKey, const QStringList &fallbackKeys) {
            QStringList out;
            if (obj.contains(arrayKey) && obj[arrayKey].isArray()) {
                QJsonArray arr = obj[arrayKey].toArray();
                for (const auto &val : arr) {
                    QString path = val.toString().trimmed();
                    if (!path.isEmpty()) out.append(path);
                }
                return out;
            }
            for (const QString &key : fallbackKeys) {
                QString val = obj.value(key).toString().trimmed();
                if (!val.isEmpty()) {
                    out.append(val);
                    break;
                }
            }
            return out;
        };
        auto readPathSet = [](const QJsonObject &obj, const QString &arrayKey) {
            QSet<QString> out;
            if (!obj.contains(arrayKey) || !obj[arrayKey].isArray()) return out;
            const QJsonArray arr = obj[arrayKey].toArray();
            for (const auto &val : arr) {
                const QString path = val.toString().trimmed();
                if (!path.isEmpty()) out.insert(path);
            }
            return out;
        };

        loraPaths = normalizePathList(readPathList(root, "lora_paths", {"lora_path"}));
        galleryPaths = normalizePathList(readPathList(root, "gallery_paths", {"gallery_path", "sd_folder"}));
        translationCsvPaths = normalizePathList(readPathList(root, "translation_paths", {"translation_path"}));
        disabledLoraPaths = normalizePathSet(readPathSet(root, "lora_paths_disabled"));
        disabledGalleryPaths = normalizePathSet(readPathSet(root, "gallery_paths_disabled"));
        disabledTranslationCsvPaths = normalizePathSet(readPathSet(root, "translation_paths_disabled"));
        collapsedModelFolders = normalizePathSet(readPathSet(root, "model_list_collapsed_folders"));
        {
            QSet<QString> filtered;
            for (const QString &path : loraPaths) {
                if (disabledLoraPaths.contains(path)) filtered.insert(path);
            }
            disabledLoraPaths = filtered;
        }
        {
            QSet<QString> enabledLoraPathSet;
            for (const QString &path : loraPaths) {
                if (!disabledLoraPaths.contains(path)) enabledLoraPathSet.insert(path);
            }
            QSet<QString> filtered;
            for (const QString &path : collapsedModelFolders) {
                if (enabledLoraPathSet.contains(path)) filtered.insert(path);
            }
            collapsedModelFolders = filtered;
        }
        {
            QSet<QString> filtered;
            for (const QString &path : galleryPaths) {
                if (disabledGalleryPaths.contains(path)) filtered.insert(path);
            }
            disabledGalleryPaths = filtered;
        }
        {
            QSet<QString> filtered;
            for (const QString &path : translationCsvPaths) {
                if (disabledTranslationCsvPaths.contains(path)) filtered.insert(path);
            }
            disabledTranslationCsvPaths = filtered;
        }
        translationCsvPath = collectEnabledPaths(translationCsvPaths, disabledTranslationCsvPaths).value(0);

        // 读取树状菜单状态
        if (optRestoreTreeState && root.contains("tree_state")) {
            QJsonObject treeState = root["tree_state"].toObject();
            startupTreeScrollPos = treeState["scroll_pos"].toInt(0);
            QJsonArray arr = treeState["expanded_items"].toArray();
            for (const auto &val : arr) {
                startupExpandedCollections.insert(val.toString());
            }
        }

        // 设置UA
        if (optUseArrangedUA && !optSavedUAString.isEmpty())currentUserAgent = optSavedUAString;
        else currentUserAgent = getRandomUserAgent();
    }

    applyPathListsToUi();

    settingsPage->setState(settings);
    settingsPage->setCivitaiApiStatus(optCivitaiApiKey.isEmpty() ? "API Key 未配置" : "API Key 未测试");
    applyApplicationTheme(optThemeId, optCustomThemePath, true);
    initSettingsPage();
}

void MainWindow::initSettingsPage()
{
    if (!settingsPage || settingsPageConnectionsInitialized) return;
    settingsPageConnectionsInitialized = true;
    connect(settingsPage, &SettingsPage::loraPathsEditRequested, this, &MainWindow::onBrowseLoraPath);
    connect(settingsPage, &SettingsPage::galleryPathsEditRequested, this, &MainWindow::onBrowseGalleryPath);
    connect(settingsPage, &SettingsPage::translationPathsEditRequested, this, &MainWindow::onBrowseTranslationPath);
    connect(settingsPage, &SettingsPage::clearGalleryCacheRequested, this, &MainWindow::onClearUserGalleryCacheClicked);
    connect(settingsPage, &SettingsPage::testCivitaiApiKeyRequested, this, [this](const QString &key) {
        optCivitaiApiKey = key.trimmed();
        saveGlobalConfig();
        onTestCivitaiApiKeyClicked();
    });
    connect(settingsPage, &SettingsPage::blurChanged, this, [this](int value, bool finalSave) {
        optBlurRadius = value;
        if (settingsPage) settingsPage->setBlurValue(value);
        updateBackgroundImage();
        if (finalSave) saveGlobalConfig();
    });
    connect(settingsPage, &SettingsPage::resetFilterTagsRequested, this, &MainWindow::resetFilterTagsToDefault);
    connect(settingsPage, &SettingsPage::randomUserAgentRequested, this, &MainWindow::applyRandomUserAgent);
    connect(settingsPage, &SettingsPage::stateChanged, this, &MainWindow::applySettingsState);
}

void MainWindow::applySettingsState(SettingsState state)
{
    state.normalize();

    const bool recursiveChanged = optLoraRecursive != state.loraRecursive || optGalleryRecursive != state.galleryRecursive;
    const bool renderThreadsChanged = optRenderThreadCount != state.renderThreadCount;
    const bool collectionTreeChanged = optShowEmptyCollections != state.showEmptyCollections
        || optCollectionFolderTopLevel != state.collectionFolderTopLevel
        || optCollectionFolderSecondLevel != state.collectionFolderSecondLevel;
    const bool modelGroupingChanged = optModelListFolderGrouping != state.modelListFolderGrouping;
    const bool civitaiNameChanged = optUseCivitaiName != state.useCivitaiName;
    const bool galleryMatchChanged = optUserGalleryMatchMode != state.userGalleryMatchMode;
    const bool uiScaleChanged = !qFuzzyCompare(optUiScale, state.uiScale);
    const bool customUaModeChanged = optUseArrangedUA != state.useCustomUserAgent;
    const bool themeChanged = optThemeId != state.themeId || optCustomThemePath != state.customThemePath;

    optLoraRecursive = state.loraRecursive;
    optGalleryRecursive = state.galleryRecursive;
    optBlurRadius = state.blurRadius;
    optDownscaleBlur = state.downscaleBlur;
    optBlurProcessWidth = state.blurProcessWidth;
    optFilterNSFW = state.filterNSFW;
    optNSFWMode = state.nsfwMode;
    optNSFWLevel = state.nsfwLevel;
    optRenderThreadCount = qMax(1, state.renderThreadCount);
    optRestoreTreeState = state.restoreTreeState;
    optSplitOnNewline = state.splitOnNewline;
    optFilterTags = state.filterTags();
    optShowEmptyCollections = state.showEmptyCollections;
    optCollectionFolderTopLevel = state.collectionFolderTopLevel;
    optCollectionFolderSecondLevel = state.collectionFolderSecondLevel;
    optModelListFolderGrouping = state.modelListFolderGrouping;
    optUseArrangedUA = state.useCustomUserAgent;
    optSavedUAString = state.customUserAgent;
    optCivitaiApiKey = state.civitaiApiKey;
    optModelUpdateDownloadPolicy = state.modelUpdateDownloadPolicy;
    optAutoCheckUpdatesOnStartup = state.autoCheckUpdatesOnStartup;
    optUseCivitaiName = state.useCivitaiName;
    optSuppressLocalWarnings = state.suppressLocalWarnings;
    optUserGalleryMatchMode = state.userGalleryMatchMode;
    optRecalculateKnownMetadataHash = state.recalculateKnownMetadataHash;
    optTryCivArchiveOnMetadataFail = state.tryCivArchiveOnMetadataFail;
    optUiScale = state.uiScale;
    optThemeId = state.themeId;
    optCustomThemePath = state.customThemePath;

    if (renderThreadsChanged) {
        threadPool->setMaxThreadCount(optRenderThreadCount);
        backgroundThreadPool->setMaxThreadCount(optRenderThreadCount);
    }
    if (customUaModeChanged) {
        currentUserAgent = optUseArrangedUA && !optSavedUAString.isEmpty() ? optSavedUAString : getRandomUserAgent();
        qDebug() << "UA Changed to:" << currentUserAgent;
    } else if (optUseArrangedUA) {
        currentUserAgent = optSavedUAString;
    }

    saveGlobalConfig();

    if (recursiveChanged) {
        // 递归扫描设置在下一次扫描时生效。
    }
    if (collectionTreeChanged) refreshCollectionTreeView();
    if (modelGroupingChanged) {
        executeSort();
        refreshHomeGallery();
        refreshCollectionTreeView();
    }
    if (civitaiNameChanged) {
        updateModelListNames();
        executeSort();
    }
    if (galleryMatchChanged) refreshModelUsageStatsAsync();
    if (uiScaleChanged) {
        ui->statusbar->showMessage(QString("缩放比例已设置为 %1x，重启后生效").arg(optUiScale), 3000);
    }
    if (themeChanged) {
        applyApplicationTheme(optThemeId, optCustomThemePath, true);
    }
}

void MainWindow::resetFilterTagsToDefault()
{
    const QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        "确认重置 / Confirm Reset",
        "确定要将过滤提示词重置为默认值吗？\n此操作将覆盖当前的自定义设置。\n\n"
        "Are you sure you want to reset filter tags to default?",
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    settingsPage->setFilterTagsText(DEFAULT_FILTER_TAGS);
    optFilterTags = DEFAULT_FILTER_TAGS.split(',', Qt::SkipEmptyParts);
    for (QString &s : optFilterTags) s = s.trimmed();
    saveGlobalConfig();
    ui->statusbar->showMessage("过滤词已重置", 2000);
}

void MainWindow::applyRandomUserAgent()
{
    const QString newUA = getRandomUserAgent();
    settingsPage->setUserAgentText(newUA);
    optUseArrangedUA = settingsPage->state().useCustomUserAgent;
    optSavedUAString = newUA;
    if (optUseArrangedUA) {
        currentUserAgent = newUA;
    }
    saveGlobalConfig();
}

void MainWindow::applyApplicationTheme(const QString &themeId, const QString &customPath, bool updateStatus)
{
    const ThemeBundle bundle = loadThemeBundle(themeId, customPath);
    qApp->setStyleSheet(bundle.dialogQss);
    setStyleSheet(bundle.mainQss);
    currentToolPageQss = bundle.toolQss;
    refreshLoadedToolPageThemes();
    if (settingsPage && updateStatus) {
        settingsPage->setThemeStatus(bundle.status);
    }
    if (updateStatus && ui && ui->statusbar) {
        ui->statusbar->showMessage(bundle.ok ? QString("已应用主题：%1").arg(themeDisplayName(themeId)) : bundle.status, 2500);
    }
}

void MainWindow::applyToolPageTheme(QWidget *page)
{
    if (!page) return;
    page->setStyleSheet(currentToolPageQss.isEmpty() ? loadQssResource(":/styles/toolpage.qss") : currentToolPageQss);
}

void MainWindow::refreshLoadedToolPageThemes()
{
    applyToolPageTheme(tagBrowserWidget);
    applyToolPageTheme(llmPromptWidget);
    applyToolPageTheme(parserWidget);
    applyToolPageTheme(usageAnalysisWidget);
    applyToolPageTheme(promptTemplateLibraryWidget);
    if (toolsTabWidget) {
        for (int i = 0; i < toolsTabWidget->count(); ++i) {
            QWidget *page = toolsTabWidget->widget(i);
            if (page && page->objectName() != "toolPlaceholder") applyToolPageTheme(page);
        }
    }
}

void MainWindow::saveGlobalConfig() {
    QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);

    QString configPath = configDir + "/settings.json";
    QJsonObject root;
    QFile readFile(configPath);
    if (readFile.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(readFile.readAll()).object();
        readFile.close();
    }
    SettingsState settings = settingsPage ? settingsPage->state() : SettingsState{};
    if (!settingsPage) {
        settings.loraRecursive = optLoraRecursive;
        settings.galleryRecursive = optGalleryRecursive;
        settings.blurRadius = optBlurRadius;
        settings.downscaleBlur = optDownscaleBlur;
        settings.blurProcessWidth = optBlurProcessWidth;
        settings.filterNSFW = optFilterNSFW;
        settings.nsfwMode = optNSFWMode;
        settings.nsfwLevel = optNSFWLevel;
        settings.renderThreadCount = optRenderThreadCount;
        settings.restoreTreeState = optRestoreTreeState;
        settings.splitOnNewline = optSplitOnNewline;
        settings.filterTagsText = optFilterTags.join(", ");
        settings.showEmptyCollections = optShowEmptyCollections;
        settings.collectionFolderTopLevel = optCollectionFolderTopLevel;
        settings.collectionFolderSecondLevel = optCollectionFolderSecondLevel;
        settings.modelListFolderGrouping = optModelListFolderGrouping;
        settings.useCustomUserAgent = optUseArrangedUA;
        settings.customUserAgent = optSavedUAString;
        settings.civitaiApiKey = optCivitaiApiKey;
        settings.modelUpdateDownloadPolicy = optModelUpdateDownloadPolicy;
        settings.autoCheckUpdatesOnStartup = optAutoCheckUpdatesOnStartup;
        settings.useCivitaiName = optUseCivitaiName;
        settings.suppressLocalWarnings = optSuppressLocalWarnings;
        settings.userGalleryMatchMode = optUserGalleryMatchMode;
        settings.recalculateKnownMetadataHash = optRecalculateKnownMetadataHash;
        settings.tryCivArchiveOnMetadataFail = optTryCivArchiveOnMetadataFail;
        settings.uiScale = optUiScale;
        settings.themeId = optThemeId;
        settings.customThemePath = optCustomThemePath;
    }

    QJsonArray loraArr;
    for (const QString &path : loraPaths) loraArr.append(path);
    QJsonArray galleryArr;
    for (const QString &path : galleryPaths) galleryArr.append(path);
    QJsonArray translationArr;
    for (const QString &path : translationCsvPaths) translationArr.append(path);
    QJsonArray loraDisabledArr;
    for (const QString &path : disabledLoraPaths) loraDisabledArr.append(path);
    QJsonArray galleryDisabledArr;
    for (const QString &path : disabledGalleryPaths) galleryDisabledArr.append(path);
    QJsonArray translationDisabledArr;
    for (const QString &path : disabledTranslationCsvPaths) translationDisabledArr.append(path);
    QJsonArray collapsedModelFoldersArr;
    for (const QString &path : collapsedModelFolders) collapsedModelFoldersArr.append(path);
    root["lora_paths"]                 = loraArr;
    root["gallery_paths"]              = galleryArr;
    root["translation_paths"]          = translationArr;
    root["lora_paths_disabled"]        = loraDisabledArr;
    root["gallery_paths_disabled"]     = galleryDisabledArr;
    root["translation_paths_disabled"] = translationDisabledArr;
    root["model_list_collapsed_folders"] = collapsedModelFoldersArr;
    root["translation_path"]           = collectEnabledPaths(translationCsvPaths, disabledTranslationCsvPaths).value(0);
    settings.writeToJson(root);
    root.remove("model_switch_delay_ms");

    // 记录当前窗口的实际大小（这样用户拉伸窗口后，下次启动会记住大小）
    // 只有当窗口没有最大化/全屏时才记录
    if (!this->isMaximized() && !this->isFullScreen()) {
        root["window_width"] = this->width();
        root["window_height"] = this->height();
    } else {
        // 如果处于最大化状态，可以保存正常的几何尺寸
        root["window_width"] = this->normalGeometry().width();
        root["window_height"] = this->normalGeometry().height();
    }

    if (optRestoreTreeState) {
        QJsonObject treeState;

        // 1. 获取当前展开的项
        QJsonArray expandedArr;
        // 如果 UI 还没初始化完(比如刚启动就关闭)，尽量保留读取到的旧状态，防止清空
        // 但这里我们主要处理运行时保存：
        if (ui->collectionTree->topLevelItemCount() > 0) {
            std::function<void(QTreeWidgetItem*)> collectExpanded = [&](QTreeWidgetItem *item) {
                if (!item) return;
                if (item->isExpanded()) {
                    const QString key = item->data(0, ROLE_COLLECTION_EXPAND_KEY).toString();
                    expandedArr.append(key.isEmpty() ? item->data(0, ROLE_COLLECTION_NAME).toString() : key);
                }
                for (int i = 0; i < item->childCount(); ++i) collectExpanded(item->child(i));
            };
            for (int i = 0; i < ui->collectionTree->topLevelItemCount(); ++i) {
                collectExpanded(ui->collectionTree->topLevelItem(i));
            }
            treeState["expanded_items"] = expandedArr;
            treeState["scroll_pos"] = ui->collectionTree->verticalScrollBar()->value();
        } else {
            // 如果树是空的（极少情况），可能还没加载，保留启动时读到的缓存
            QJsonArray cachedArr;
            for (const QString &s : startupExpandedCollections) cachedArr.append(s);
            treeState["expanded_items"] = cachedArr;
            treeState["scroll_pos"] = startupTreeScrollPos;
        }

        root["tree_state"] = treeState;
    }

    QFile file(configPath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson());
    }
}

QStringList MainWindow::normalizePathList(const QStringList &paths) const
{
    QStringList result;
    QSet<QString> seen;
    for (QString path : paths) {
        path = path.trimmed();
        if (path.isEmpty()) continue;
        QString normalized = QFileInfo(path).absoluteFilePath();
        if (seen.contains(normalized)) continue;
        seen.insert(normalized);
        result.append(normalized);
    }
    return result;
}

QSet<QString> MainWindow::normalizePathSet(const QSet<QString> &paths) const
{
    QSet<QString> result;
    for (QString path : paths) {
        path = path.trimmed();
        if (path.isEmpty()) continue;
        result.insert(QFileInfo(path).absoluteFilePath());
    }
    return result;
}

QString MainWindow::formatPathListForEdit(const QStringList &paths) const
{
    return paths.join("; ");
}

QStringList MainWindow::collectValidPaths(const QStringList &paths) const
{
    QStringList valid;
    for (const QString &path : paths) {
        if (!path.isEmpty() && QDir(path).exists()) {
            valid.append(path);
        }
    }
    return valid;
}

QStringList MainWindow::collectEnabledPaths(const QStringList &paths, const QSet<QString> &disabledPaths) const
{
    QStringList enabled;
    enabled.reserve(paths.size());
    for (const QString &path : paths) {
        if (path.isEmpty()) continue;
        const QString normalized = QFileInfo(path).absoluteFilePath();
        if (disabledPaths.contains(normalized)) continue;
        enabled.append(normalized);
    }
    return normalizePathList(enabled);
}

QList<ManagedPathEntry> MainWindow::buildPathEntries(const QStringList &paths, const QSet<QString> &disabledPaths) const
{
    QList<ManagedPathEntry> entries;
    entries.reserve(paths.size());
    for (const QString &path : paths) {
        if (path.isEmpty()) continue;
        const QString normalized = QFileInfo(path).absoluteFilePath();
        entries.append({normalized, !disabledPaths.contains(normalized)});
    }
    return entries;
}

void MainWindow::applyPathEntries(const QList<ManagedPathEntry> &entries, QStringList &paths, QSet<QString> &disabledPaths)
{
    paths.clear();
    disabledPaths.clear();
    QSet<QString> seen;
    for (const ManagedPathEntry &entry : entries) {
        const QString normalized = QFileInfo(entry.path.trimmed()).absoluteFilePath();
        if (normalized.isEmpty() || seen.contains(normalized)) continue;
        seen.insert(normalized);
        paths.append(normalized);
        if (!entry.enabled) disabledPaths.insert(normalized);
    }
}

void MainWindow::applyPathListsToUi()
{
    const QStringList activeLoraPaths = collectEnabledPaths(loraPaths, disabledLoraPaths);
    const QStringList activeGalleryPaths = collectEnabledPaths(galleryPaths, disabledGalleryPaths);
    const QStringList activeTranslationPaths = collectEnabledPaths(translationCsvPaths, disabledTranslationCsvPaths);
    currentLoraPath = activeLoraPaths.value(0);
    sdOutputFolder = activeGalleryPaths.value(0);
    translationCsvPath = activeTranslationPaths.value(0);

    if (settingsPage) {
        settingsPage->setPathSummaries(
            formatPathListForEdit(activeLoraPaths),
            formatPathListForEdit(activeGalleryPaths),
            formatPathListForEdit(activeTranslationPaths));
    }
    if (tagBrowserWidget) {
        tagBrowserWidget->setCsvPath(translationCsvPath);
        tagBrowserWidget->setMergedTranslationMap(&translationMap);
    }
    if (llmPromptWidget) llmPromptWidget->setLibraryPaths(activeLoraPaths, activeGalleryPaths);
}

bool MainWindow::editLoraPaths(bool rescanAfter)
{
    PathListDialog dlg(this);
    dlg.setDialogTitle("LoRA 路径管理");
    dlg.setHintText("可添加多个 LoRA 模型目录。通过右侧复选框控制是否启用扫描。");
    dlg.setPathEntries(buildPathEntries(loraPaths, disabledLoraPaths));

    if (dlg.exec() != QDialog::Accepted) return false;

    applyPathEntries(dlg.pathEntries(), loraPaths, disabledLoraPaths);
    loraPaths = normalizePathList(loraPaths);
    disabledLoraPaths = normalizePathSet(disabledLoraPaths);
    collapsedModelFolders = normalizePathSet(collapsedModelFolders);
    {
        QSet<QString> filtered;
        for (const QString &path : loraPaths) {
            if (disabledLoraPaths.contains(path)) filtered.insert(path);
        }
        disabledLoraPaths = filtered;
    }
    {
        QSet<QString> activeRoots;
        for (const QString &path : loraPaths) {
            if (!disabledLoraPaths.contains(path)) activeRoots.insert(path);
        }
        QSet<QString> filtered;
        for (const QString &path : collapsedModelFolders) {
            if (activeRoots.contains(path)) filtered.insert(path);
        }
        collapsedModelFolders = filtered;
    }
    applyPathListsToUi();
    saveGlobalConfig();

    const QStringList activeLoraPaths = collectEnabledPaths(loraPaths, disabledLoraPaths);
    if (rescanAfter && !activeLoraPaths.isEmpty()) {
        scanModels(activeLoraPaths);
    }
    return true;
}

bool MainWindow::editGalleryPaths(bool rescanAfter)
{
    PathListDialog dlg(this);
    dlg.setDialogTitle("图库路径管理");
    dlg.setHintText("可添加多个图库目录。通过右侧复选框控制是否启用扫描。");
    dlg.setPathEntries(buildPathEntries(galleryPaths, disabledGalleryPaths));

    if (dlg.exec() != QDialog::Accepted) return false;

    applyPathEntries(dlg.pathEntries(), galleryPaths, disabledGalleryPaths);
    galleryPaths = normalizePathList(galleryPaths);
    disabledGalleryPaths = normalizePathSet(disabledGalleryPaths);
    {
        QSet<QString> filtered;
        for (const QString &path : galleryPaths) {
            if (disabledGalleryPaths.contains(path)) filtered.insert(path);
        }
        disabledGalleryPaths = filtered;
    }
    applyPathListsToUi();
    saveGlobalConfig();

    if (rescanAfter) {
        onRescanUserClicked();
    }
    return true;
}

// === 设置页交互 ===

void MainWindow::onBrowseLoraPath() {
    if (editLoraPaths(false)) {
        QMessageBox::information(this, "提示", "LoRA 路径已更新，请返回库界面点击刷新按钮。");
    }
}

void MainWindow::onBrowseGalleryPath() {
    editGalleryPaths(false);
}

QIcon MainWindow::generatePlaceholderIcon()
{
    // === 修改点 1：将基准尺寸设为 180 (适配主页大图) ===
    int fullSize = 180;

    // === 修改点 2：按比例调整内边距 ===
    // 之前 64px 用 8px 边距 (12.5%)
    // 现在 180px 对应约 20px 边距，这样缩小后在侧边栏看着比例才对
    int padding = 20;

    int contentSize = fullSize - (padding * 2);

    // 创建透明底图
    QPixmap finalPix(fullSize, fullSize);
    finalPix.fill(Qt::transparent);

    QPainter painter(&finalPix);
    // 开启高质量抗锯齿
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // 计算中间内容的区域
    QRect contentRect(padding, padding, contentSize, contentSize);

    // 绘制深色背景框 (圆角加大一点)
    painter.setBrush(QColor(AppStyle::PanelDark));
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(contentRect, 12, 12);

    // 绘制“×”符号
    QPen pen{QColor(AppStyle::PanelBorder)}; // 线条颜色稍微调深一点，更有质感
    pen.setWidth(5); // 线条加粗，适应大尺寸
    pen.setCapStyle(Qt::RoundCap); // 线条端点圆润
    painter.setPen(pen);

    // 画两条交叉线 (Margin 也要按比例放大)
    int margin = 40;
    painter.drawLine(contentRect.left() + margin, contentRect.top() + margin,
                     contentRect.right() - margin, contentRect.bottom() - margin);
    painter.drawLine(contentRect.right() - margin, contentRect.top() + margin,
                     contentRect.left() + margin, contentRect.bottom() - margin);

    return QIcon(finalPix);
}

QString MainWindow::getSafetensorsInternalName(const QString &path)
{
    return getSafetensorsInternalNameWorker(path);
}

QPixmap MainWindow::applyNSFWBlur(const QPixmap &pix) {
    if (pix.isNull()) return pix;

    QGraphicsBlurEffect *blur = new QGraphicsBlurEffect;
    blur->setBlurRadius(40); // 强度大一点，确保看不清内容

    QGraphicsScene scene;
    QGraphicsPixmapItem *item = new QGraphicsPixmapItem(pix);
    item->setGraphicsEffect(blur);
    scene.addItem(item);

    QPixmap result(pix.size());
    result.fill(Qt::transparent);
    QPainter painter(&result);
    scene.render(&painter);
    return result;
}

QPixmap MainWindow::applyRoundedMask(const QPixmap &src, int radius)
{
    if (src.isNull()) return QPixmap();
    if (radius <= 0) return src;

    QPixmap result(src.size());
    result.fill(Qt::transparent);

    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // 创建圆角路径
    QPainterPath path;
    path.addRoundedRect(src.rect(), radius, radius);

    // 裁剪并绘制
    painter.setClipPath(path);
    painter.drawPixmap(0, 0, src);

    return result;
}

// 浏览翻译文件
void MainWindow::onBrowseTranslationPath() {
    if (editTranslationCsvPaths()) {
        QMessageBox::information(this, "设置", "翻译词表已加载。");
    }
}

bool MainWindow::editTranslationCsvPaths()
{
    PathListDialog dlg(this);
    dlg.setDialogTitle("Tag 翻译表管理");
    dlg.setHintText("可添加多个 Tag 翻译 CSV。列表越靠上优先级越高，可通过右侧复选框临时停用。");
    dlg.setSelectionMode(PathListDialog::FileMode, "CSV Files (*.csv);;All Files (*.*)");
    dlg.setPathEntries(buildPathEntries(translationCsvPaths, disabledTranslationCsvPaths));

    if (dlg.exec() != QDialog::Accepted) return false;

    applyPathEntries(dlg.pathEntries(), translationCsvPaths, disabledTranslationCsvPaths);
    translationCsvPaths = normalizePathList(translationCsvPaths);
    disabledTranslationCsvPaths = normalizePathSet(disabledTranslationCsvPaths);
    {
        QSet<QString> filtered;
        for (const QString &path : translationCsvPaths) {
            if (disabledTranslationCsvPaths.contains(path)) filtered.insert(path);
        }
        disabledTranslationCsvPaths = filtered;
    }

    applyPathListsToUi();
    reloadTranslationMaps();
    saveGlobalConfig();
    return true;
}

void MainWindow::reloadTranslationMaps(bool notifyWidgets)
{
    translationMap.clear();
    const QStringList activePaths = collectEnabledPaths(translationCsvPaths, disabledTranslationCsvPaths);
    for (int i = activePaths.size() - 1; i >= 0; --i) {
        const QString path = activePaths.at(i);
        if (path.isEmpty() || !QFile::exists(path)) continue;

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        QTextStream in(&file);
        while (!in.atEnd()) {
            QString line = in.readLine();
            if (line.trimmed().isEmpty()) continue;

            int firstComma = line.indexOf(',');
            if (firstComma == -1) continue;
            QString en = line.left(firstComma).trimmed();
            QString cn = line.mid(firstComma + 1).trimmed();
            if (en.startsWith('"') && en.endsWith('"')) en = en.mid(1, en.length() - 2);
            if (cn.startsWith('"') && cn.endsWith('"')) cn = cn.mid(1, cn.length() - 2);
            if (!en.isEmpty() && !cn.isEmpty()) {
                translationMap.insert(en, cn);
            }
        }
    }

    translationCsvPath = activePaths.value(0);
    if (!notifyWidgets) {
        qDebug() << "Loaded translation entries:" << translationMap.size() << "from" << activePaths.size() << "CSV file(s)";
        return;
    }
    if (parserWidget) parserWidget->setTranslationMap(&translationMap);
    if (promptTemplateLibraryWidget) promptTemplateLibraryWidget->setTranslationMap(&translationMap);
    if (tagBrowserWidget) {
        tagBrowserWidget->setCsvPath(translationCsvPath);
        tagBrowserWidget->setMergedTranslationMap(&translationMap);
    }
    if (tagFlowWidget) tagFlowWidget->setTranslationMap(&translationMap);
    qDebug() << "Loaded translation entries:" << translationMap.size() << "from" << activePaths.size() << "CSV file(s)";
}

void MainWindow::onUserGalleryContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = ui->listUserImages->itemAt(pos);
    if (!item) return;

    QString filePath = item->data(ROLE_USER_IMAGE_PATH).toString();
    if (filePath.isEmpty()) return;
    QString prompt = item->data(ROLE_USER_IMAGE_PROMPT).toString();
    QString neg = item->data(ROLE_USER_IMAGE_NEG).toString();
    QString params = item->data(ROLE_USER_IMAGE_PARAMS).toString();

    QMenu menu(this);

    QAction *actCopyGenParams = menu.addAction("复制生成参数 / Copy Gen Params");
    actCopyGenParams->setToolTip("复制符合SD WebUI格式的完整参数，\n粘贴进提示词框后可直接点击↙️按钮读取。");
    menu.addSeparator(); // 分隔线
    QAction *actOpenImg = menu.addAction("打开图片 / Open Image");
    QAction *actOpenDir = menu.addAction("打开文件位置 / Show in Folder");
    QAction *actShowComfyWorkflow = menu.addAction("查看 ComfyUI Workflow / View Workflow");
    QAction *actShowRawMetadata = menu.addAction("显示原始 Metadata / Raw Metadata");
    menu.addSeparator();
    QAction *actCopyPath = menu.addAction("复制路径 / Copy Path");

    QAction *selected = menu.exec(ui->listUserImages->mapToGlobal(pos));

    if (selected == actCopyGenParams) {
        QStringList parts;

        // 1. 正向提示词
        if (!prompt.isEmpty()) {
            parts.append(prompt);
        }

        // 2. 反向提示词 (需要补上标准前缀 "Negative prompt: ")
        // 注意：在 parsePngInfo 里如果是 "(empty)" 则跳过
        if (!neg.isEmpty() && neg != "(empty)") {
            parts.append("Negative prompt: " + neg);
        }

        // 3. 参数行 (Steps: 20, Sampler: ...)
        if (!params.isEmpty()) {
            parts.append(params);
        }

        // 用换行符连接，这是 SD WebUI 识别的标准格式
        QString fullParams = parts.join("\n");

        QClipboard *clip = QGuiApplication::clipboard();
        clip->setText(fullParams);

        ui->statusbar->showMessage("已复制 SD 生成参数到剪贴板", 2000);
    }
    else if (selected == actOpenImg) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
    }
    else if (selected == actOpenDir) {
        showFileInFolder(filePath);
    }
    else if (selected == actShowRawMetadata) {
        showRawImageMetadataDialog(filePath);
    }
    else if (selected == actShowComfyWorkflow) {
        showComfyWorkflowViewer(filePath);
    }
    else if (selected == actCopyPath) {
        QGuiApplication::clipboard()->setText(QDir::toNativeSeparators(filePath));
        ui->statusbar->showMessage("路径已复制", 2000);
    }
}

void MainWindow::showComfyWorkflowViewer(const QString &filePath)
{
    ComfyWorkflowViewerDialog dialog(filePath, this);
    dialog.exec();
}

void MainWindow::showModelDescriptionDialog()
{
    QString descriptionHtml = currentMeta.description.trimmed();
    if (descriptionHtml.isEmpty()) {
        descriptionHtml = ui->textDescription->toHtml().trimmed();
    }

    QTextDocument probe;
    probe.setHtml(descriptionHtml);
    const QString plainText = probe.toPlainText().trimmed();
    if (plainText.isEmpty() || plainText == "No description.") {
        QMessageBox::information(this, "模型简介", "当前模型没有可显示的简介。");
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("模型简介 / Description");
    dialog.resize(820, 620);

    auto *layout = new QVBoxLayout(&dialog);
    auto *title = new QLabel(currentMeta.name.isEmpty() ? currentMeta.fileNameServer : currentMeta.name, &dialog);
    title->setObjectName("lblTitle");
    title->setWordWrap(true);
    title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(title);

    auto *textEdit = new QTextEdit(&dialog);
    textEdit->setReadOnly(true);
    textEdit->setAcceptRichText(true);
    textEdit->setHtml(descriptionHtml);
    textEdit->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    layout->addWidget(textEdit, 1);

    auto *buttons = new QHBoxLayout();
    auto *btnCopySelected = new QPushButton("复制选中 / Copy Selected", &dialog);
    auto *btnCopyAll = new QPushButton("复制全部 / Copy All", &dialog);
    auto *btnClose = new QPushButton("关闭 / Close", &dialog);
    buttons->addWidget(btnCopySelected);
    buttons->addWidget(btnCopyAll);
    buttons->addStretch(1);
    buttons->addWidget(btnClose);
    layout->addLayout(buttons);

    auto normalizeCopiedText = [](QString text) {
        text.replace(QChar(0x2029), '\n');
        text.replace(QChar(0x2028), '\n');
        return text.trimmed();
    };

    connect(btnCopySelected, &QPushButton::clicked, &dialog, [this, textEdit, normalizeCopiedText]() {
        const QString selected = normalizeCopiedText(textEdit->textCursor().selectedText());
        if (selected.isEmpty()) {
            ui->statusbar->showMessage("请先在简介中选中要复制的文本。", 2000);
            return;
        }
        QGuiApplication::clipboard()->setText(selected);
        ui->statusbar->showMessage("已复制选中的简介文本。", 2000);
    });

    connect(btnCopyAll, &QPushButton::clicked, &dialog, [this, textEdit, normalizeCopiedText]() {
        QGuiApplication::clipboard()->setText(normalizeCopiedText(textEdit->toPlainText()));
        ui->statusbar->showMessage("已复制完整简介。", 2000);
    });

    connect(btnClose, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
}

void MainWindow::showRawImageMetadataDialog(const QString &filePath)
{
    const QMap<QString, QString> chunks = extractImageMetadataTextChunks(filePath);

    QStringList sections;
    for (auto it = chunks.begin(); it != chunks.end(); ++it) {
        sections << QString("===== %1 =====\n%2").arg(it.key(), it.value());
    }
    QString rawText = sections.isEmpty()
                          ? QString("未找到图片 metadata。")
                          : sections.join("\n\n");

    QDialog dialog(this);
    dialog.setWindowTitle("原始 Metadata / Raw Metadata");
    dialog.resize(900, 650);

    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    QLabel *title = new QLabel(QFileInfo(filePath).fileName(), &dialog);
    title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(title);

    QTextEdit *textEdit = new QTextEdit(&dialog);
    textEdit->setPlainText(rawText);
    textEdit->setLineWrapMode(QTextEdit::NoWrap);
    textEdit->setReadOnly(false);
    layout->addWidget(textEdit, 1);

    QHBoxLayout *buttons = new QHBoxLayout();
    QPushButton *btnCopy = new QPushButton("复制 / Copy", &dialog);
    QPushButton *btnFormatJson = new QPushButton("格式化 JSON / Format JSON", &dialog);
    QPushButton *btnClose = new QPushButton("关闭 / Close", &dialog);
    buttons->addWidget(btnCopy);
    buttons->addWidget(btnFormatJson);
    buttons->addStretch(1);
    buttons->addWidget(btnClose);
    layout->addLayout(buttons);

    connect(btnCopy, &QPushButton::clicked, &dialog, [textEdit]() {
        QGuiApplication::clipboard()->setText(textEdit->toPlainText());
    });

    connect(btnFormatJson, &QPushButton::clicked, &dialog, [textEdit]() {
        QString text = textEdit->toPlainText();
        static QRegularExpression sectionRegex("===== ([^=]+) =====\\n(.*?)(?=\\n\\n===== |\\z)",
                                               QRegularExpression::DotMatchesEverythingOption);
        QRegularExpressionMatchIterator it = sectionRegex.globalMatch(text);
        QStringList formattedSections;
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            const QString key = match.captured(1).trimmed();
            const QString value = match.captured(2);
            QJsonParseError error;
            const QJsonDocument doc = QJsonDocument::fromJson(value.toUtf8(), &error);
            if (error.error == QJsonParseError::NoError && !doc.isNull()) {
                formattedSections << QString("===== %1 =====\n%2")
                                         .arg(key, QString::fromUtf8(doc.toJson(QJsonDocument::Indented)));
            } else {
                formattedSections << QString("===== %1 =====\n%2").arg(key, value);
            }
        }

        if (formattedSections.isEmpty()) {
            QJsonParseError error;
            const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &error);
            if (error.error == QJsonParseError::NoError && !doc.isNull()) {
                textEdit->setPlainText(QString::fromUtf8(doc.toJson(QJsonDocument::Indented)));
            } else {
                QMessageBox::information(textEdit, "格式化 JSON", "当前内容不是有效 JSON。");
            }
            return;
        }

        textEdit->setPlainText(formattedSections.join("\n\n"));
    });

    connect(btnClose, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
}

void MainWindow::onModelsTabButtonClicked()
{
    ui->sidebarStack->setCurrentIndex(0);
    ui->btnModelsTab->setChecked(true);
    ui->btnCollectionsTab->setChecked(false);
}

void MainWindow::onCollectionsTabButtonClicked()
{
    ui->sidebarStack->setCurrentIndex(1);
    ui->btnCollectionsTab->setChecked(true);
    ui->btnModelsTab->setChecked(false);
}

void MainWindow::onCollectionTreeItemClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);

    // 判断是收藏夹节点还是模型节点
    if (item->data(0, ROLE_IS_COLLECTION_NODE).toBool()) {
        // === 收藏夹节点 ===
        QString collectionName = item->data(0, ROLE_COLLECTION_NAME).toString();
        int count = item->data(0, ROLE_ITEM_COUNT).toInt();

        // 2. 展开/折叠节点
        bool wasExpanded = item->isExpanded();
        item->setExpanded(!wasExpanded);

        // 3. 切换文本前缀
        QString displayName;
        if (item->data(0, ROLE_IS_FOLDER_HEADER).toBool()) {
            displayName = item->data(0, ROLE_MODEL_ROOT_NAME).toString();
            if (displayName.isEmpty()) displayName = collectionName;
        } else {
            displayName = (collectionName == FILTER_UNCATEGORIZED) ? "未分类 / Uncategorized" : collectionName;
        }
        QString prefix = (!wasExpanded) ? " - " : " + "; // 此时已切换状态
        QString newText = QString("%1%2 (%3)").arg(prefix).arg(displayName).arg(count);
        item->setText(0, newText);
    } else {
        // 模型节点
        QString filePath = item->data(0, ROLE_FILE_PATH).toString();
        if (filePath.isEmpty()) return;

        QListWidgetItem* matchItem = nullptr;
        for(int i = 0; i < ui->modelList->count(); ++i) {
            QListWidgetItem* sideItem = ui->modelList->item(i);
            if (sideItem->data(ROLE_FILE_PATH).toString() == filePath) {
                matchItem = sideItem;
                break;
            }
        }

        if (matchItem) {
            ui->modelList->setCurrentItem(matchItem);
            onModelListClicked(matchItem);
        }
    }
}

void MainWindow::onCollectionTreeContextMenu(const QPoint &pos)
{
    // 获取点击位置的 Item (作为上下文判断依据)
    QTreeWidgetItem *clickedItem = ui->collectionTree->itemAt(pos);
    if (!clickedItem) return;

    // 获取所有选中的 Items
    QList<QTreeWidgetItem*> selectedTreeItems = ui->collectionTree->selectedItems();

    // 如果右键点击的项不在选区内，Qt 的默认行为会清除选区并单选点击项。
    // 这里做一个保险，如果选区为空，就把点击项加进去
    if (selectedTreeItems.isEmpty()) {
        selectedTreeItems.append(clickedItem);
    }

    // 判断是收藏夹节点还是模型节点
    if (clickedItem->data(0, ROLE_IS_COLLECTION_NODE).toBool()) {
        if (clickedItem->data(0, ROLE_IS_FOLDER_HEADER).toBool()) {
            QList<QListWidgetItem*> targetListItems;
            std::function<void(QTreeWidgetItem*)> collectModelItems = [&](QTreeWidgetItem *treeItem) {
                if (!treeItem) return;
                const QString baseName = treeItem->data(0, ROLE_MODEL_NAME).toString();
                if (!baseName.isEmpty()) {
                    for (int i = 0; i < ui->modelList->count(); ++i) {
                        QListWidgetItem *listItem = ui->modelList->item(i);
                        if (listItem->data(ROLE_MODEL_NAME).toString() == baseName) {
                            targetListItems.append(listItem);
                            break;
                        }
                    }
                }
                for (int i = 0; i < treeItem->childCount(); ++i) collectModelItems(treeItem->child(i));
            };
            collectModelItems(clickedItem);
            if (!targetListItems.isEmpty()) {
                showCollectionMenu(targetListItems, ui->collectionTree->mapToGlobal(pos));
            }
            return;
        }
        // --- 收藏夹节点右键菜单 ---
        QString collectionName = clickedItem->data(0, ROLE_COLLECTION_NAME).toString();

        QMenu menu(this);
        QAction *title = menu.addAction(QString("管理收藏夹: %1").arg(collectionName));
        title->setEnabled(false);
        menu.addSeparator();

        if (collectionName == FILTER_UNCATEGORIZED) {
            // "未分类"特殊处理，不能重命名和删除
            QAction *dummy = menu.addAction("无法操作此项");
            dummy->setEnabled(false);
        } else {
            QAction *actRename = menu.addAction("重命名 / Rename Collection");
            QAction *actDelete = menu.addAction("删除 / Delete Collection");

            QAction *selected = menu.exec(ui->collectionTree->mapToGlobal(pos));

            if (selected == actRename) {
                bool ok;
                QString newName = QInputDialog::getText(this, "重命名收藏夹", "新名称:", QLineEdit::Normal, collectionName, &ok);
                if (ok && !newName.trimmed().isEmpty() && newName != collectionName) {
                    if (collections.contains(newName)) {
                        QMessageBox::warning(this, "错误", "该名称已存在！");
                        return;
                    }
                    QStringList files = collections.value(collectionName);
                    collections.insert(newName, files);
                    collections.remove(collectionName);

                    if (currentCollectionFilter == collectionName) currentCollectionFilter = newName;

                    saveCollections();
                    refreshHomeCollectionsUI(); // 刷新主页的收藏夹按钮
                    refreshCollectionTreeView(); // 刷新收藏夹树状视图
                }
            } else if (selected == actDelete) {
                auto reply = QMessageBox::question(this, "确认删除",
                                                   QString("确定要删除收藏夹 \"%1\" 吗？\n(里面的模型不会被删除，仅删除分类)").arg(collectionName),
                                                   QMessageBox::Yes | QMessageBox::No);
                if (reply == QMessageBox::Yes) {
                    collections.remove(collectionName);
                    if (currentCollectionFilter == collectionName) currentCollectionFilter = "";
                    saveCollections();
                    refreshHomeCollectionsUI();
                    refreshCollectionTreeView();
                    // 如果删除了当前正在过滤的收藏夹，需要重置主页过滤
                    if (currentCollectionFilter.isEmpty()) refreshHomeGallery();
                }
            }
        }
    } else {
        // --- 模型节点逻辑 (核心修改：支持多选) ---

        QList<QListWidgetItem*> targetListItems;

        // 遍历所有选中的树节点
        for (QTreeWidgetItem *tItem : selectedTreeItems) {
            // 跳过选中的“文件夹”节点，只处理“模型”节点
            if (tItem->data(0, ROLE_IS_COLLECTION_NODE).toBool()) continue;

            QString baseName = tItem->data(0, ROLE_MODEL_NAME).toString();
            if (baseName.isEmpty()) baseName = tItem->text(0); // 兜底

            // 在 modelList 中查找对应的 Item (因为 showCollectionMenu 需要 QListWidgetItem)
            // 优化：这里不需要每次都遍历 modelList，可以构建一个查找 Map，或者简单遍历
            for (int i = 0; i < ui->modelList->count(); ++i) {
                QListWidgetItem *listItem = ui->modelList->item(i);
                if (listItem->data(ROLE_MODEL_NAME).toString() == baseName) {
                    targetListItems.append(listItem);
                    break;
                }
            }
        }

        // 如果找到了对应的模型列表项，弹出批量操作菜单
        if (!targetListItems.isEmpty()) {
            showCollectionMenu(targetListItems, ui->collectionTree->mapToGlobal(pos));
        }
    }
}

void MainWindow::refreshCollectionTreeView()
{
    // 保存菜单
    QSet<QString> expandedCollections;
    int scrollPos = 0;

    if (isFirstTreeRefresh && optRestoreTreeState) {
        // A. 首次启动：使用从 JSON 读取的缓存
        expandedCollections = startupExpandedCollections;
        scrollPos = startupTreeScrollPos;
        isFirstTreeRefresh = false; // 标记已使用，下次刷新就走逻辑 B
    } else {
        // B. 运行时刷新：使用 UI 当前的状态
        std::function<void(QTreeWidgetItem*)> collectExpanded = [&](QTreeWidgetItem *item) {
            if (!item) return;
            if (item->isExpanded()) {
                const QString key = item->data(0, ROLE_COLLECTION_EXPAND_KEY).toString();
                expandedCollections.insert(key.isEmpty() ? item->data(0, ROLE_COLLECTION_NAME).toString() : key);
            }
            for (int i = 0; i < item->childCount(); ++i) collectExpanded(item->child(i));
        };
        for (int i = 0; i < ui->collectionTree->topLevelItemCount(); ++i) {
            collectExpanded(ui->collectionTree->topLevelItem(i));
        }
        scrollPos = ui->collectionTree->verticalScrollBar()->value();
    }

    // 清理和生成
    ui->collectionTree->clear();
    ui->collectionTree->setAnimated(true);
    ui->collectionTree->setIconSize(QSize(32, 32));
    ui->collectionTree->setRootIsDecorated(false);
    ui->collectionTree->setIndentation(10); // 子节点的缩进保持正常
    ui->collectionTree->setExpandsOnDoubleClick(false);

    QFont categoryFont = ui->collectionTree->font();
    categoryFont.setBold(true);
    categoryFont.setPointSize(10);

    const QString PRE_OPEN   = " - "; // 展开时：减号 + 空格
    const QString PRE_CLOSED = " + "; // 折叠时：加号 + 空格

    QMap<QString, QListWidgetItem*> visibleItemMap; // BaseName -> Item
    QMap<QString, int> visibleItemRank;             // BaseName -> SortIndex (排名)

    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!isModelListItem(item)) continue;

        // 收藏夹树只关心搜索/底模过滤结果，不受 Models 文件夹折叠影响。
        if (!item->data(ROLE_MODEL_FILTER_VISIBLE).toBool()) continue;

        QString baseName = item->data(ROLE_MODEL_NAME).toString();
        visibleItemMap.insert(baseName, item);
        visibleItemRank.insert(baseName, i); // 记录它在列表中的顺序
    }

    // 定义排序 Lambda：让树节点按照 modelList 的顺序排列
    auto rankSort = [&](const QString &s1, const QString &s2) {
        return visibleItemRank.value(s1) < visibleItemRank.value(s2);
    };

    // 辅助 Lambda：添加子节点
    auto addModelChildren = [&](QTreeWidgetItem *parent, QStringList models) {
        // 1. 过滤：移除不可见的模型
        QMutableStringListIterator it(models);
        while (it.hasNext()) {
            if (!visibleItemMap.contains(it.next())) {
                it.remove();
            }
        }

        // 2. 排序：按照 modelList 的顺序重排
        std::sort(models.begin(), models.end(), rankSort);

        // 3. 生成节点
        for (const QString &baseName : models) {
            if (visibleItemMap.contains(baseName)) {
                QListWidgetItem *sourceItem = visibleItemMap.value(baseName);

                QTreeWidgetItem *child = new QTreeWidgetItem(parent);
                child->setText(0, sourceItem->text());
                child->setData(0, ROLE_FILE_PATH, sourceItem->data(ROLE_FILE_PATH));
                child->setData(0, ROLE_PREVIEW_PATH, sourceItem->data(ROLE_PREVIEW_PATH));
                child->setData(0, ROLE_NSFW_LEVEL, sourceItem->data(ROLE_NSFW_LEVEL));
                child->setData(0, ROLE_MODEL_NAME, sourceItem->data(ROLE_MODEL_NAME));
                child->setData(0, ROLE_MODEL_CREATOR, sourceItem->data(ROLE_MODEL_CREATOR));
                child->setData(0, ROLE_MODEL_TAGS, sourceItem->data(ROLE_MODEL_TAGS));
                child->setData(0, ROLE_USER_RATING, sourceItem->data(ROLE_USER_RATING));
                child->setData(0, ROLE_USER_NOTE, sourceItem->data(ROLE_USER_NOTE));
                child->setData(0, ROLE_USER_TAGS, sourceItem->data(ROLE_USER_TAGS));
                child->setData(0, ROLE_USER_CUSTOM_TRIGGERS, sourceItem->data(ROLE_USER_CUSTOM_TRIGGERS));
                child->setIcon(0, sourceItem->icon());
                applyModelHighlightColor(child);
                applyModelUserNoteData(child);
            }
        }
    };

    // =========================================================
    // 3. 生成节点 (使用上面的可见数据源)
    // =========================================================

    QSet<QString> categorizedSet;
    for (auto it = collections.begin(); it != collections.end(); ++it) {
        for (const QString &m : it.value()) categorizedSet.insert(m);
    }

    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setIgnorePunctuation(false);

    QList<QString> collectionNames = collections.keys();
    collectionNames.removeAll(FILTER_UNCATEGORIZED);
    std::sort(collectionNames.begin(), collectionNames.end(), [&](const QString &s1, const QString &s2){
        return collator.compare(s1, s2) < 0;
    });

    auto makeTreeParent = [&](QTreeWidgetItem *parent, const QString &expandKey, const QString &collectionName, const QString &displayName, int count) {
        QTreeWidgetItem *node = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(ui->collectionTree);
        node->setData(0, ROLE_IS_COLLECTION_NODE, true);
        node->setData(0, ROLE_COLLECTION_NAME, collectionName);
        node->setData(0, ROLE_COLLECTION_EXPAND_KEY, expandKey);
        node->setData(0, ROLE_ITEM_COUNT, count);
        node->setFont(0, categoryFont);

        const bool expanded = expandedCollections.contains(expandKey);
        node->setExpanded(expanded);
        node->setText(0, (expanded ? PRE_OPEN : PRE_CLOSED) + displayName + " (" + QString::number(count) + ")");
        return node;
    };

    auto modelBelongsToFolder = [&](const QString &baseName, const QString &folderPath) {
        QListWidgetItem *item = visibleItemMap.value(baseName, nullptr);
        if (!item) return false;
        return item->data(ROLE_MODEL_ROOT_PATH).toString() == folderPath;
    };
    auto folderNameForPath = [&](const QString &folderPath) {
        for (auto it = visibleItemMap.begin(); it != visibleItemMap.end(); ++it) {
            QListWidgetItem *item = it.value();
            if (item->data(ROLE_MODEL_ROOT_PATH).toString() == folderPath) {
                QString folderName = item->data(ROLE_MODEL_ROOT_NAME).toString();
                return folderName.isEmpty() ? folderPath : folderName;
            }
        }
        return folderPath.isEmpty() ? QString("未指定文件夹") : folderPath;
    };
    auto addFolderChildrenForCollection = [&](QTreeWidgetItem *collectionNode, const QString &collectionKeyPrefix, QStringList models) {
        QMap<QString, QStringList> modelsByFolder;
        QMap<QString, QString> folderNames;
        for (const QString &baseName : models) {
            if (!visibleItemMap.contains(baseName)) continue;
            QListWidgetItem *item = visibleItemMap.value(baseName);
            QString folderPath = item->data(ROLE_MODEL_ROOT_PATH).toString();
            if (folderPath.isEmpty()) folderPath = "__unknown__";
            modelsByFolder[folderPath].append(baseName);
            folderNames.insert(folderPath, folderNameForPath(folderPath));
        }

        QStringList folderPaths = modelsByFolder.keys();
        std::sort(folderPaths.begin(), folderPaths.end(), [&](const QString &a, const QString &b) {
            return collator.compare(folderNames.value(a), folderNames.value(b)) < 0;
        });

        for (const QString &folderPath : folderPaths) {
            QStringList folderModels = modelsByFolder.value(folderPath);
            const int visibleCount = folderModels.count();
            if (visibleCount <= 0 && !optShowEmptyCollections) continue;
            const QString key = QString("%1:folder:%2").arg(collectionKeyPrefix, folderPath);
            QTreeWidgetItem *folderNode = makeTreeParent(collectionNode, key, key, folderNames.value(folderPath), visibleCount);
            folderNode->setData(0, ROLE_IS_FOLDER_HEADER, true);
            folderNode->setData(0, ROLE_MODEL_ROOT_NAME, folderNames.value(folderPath));
            addModelChildren(folderNode, folderModels);
        }
    };

    auto addCollectionGroup = [&](QTreeWidgetItem *parent, const QString &folderPath) {
        QStringList uncatModels;
        for (auto it = visibleItemMap.begin(); it != visibleItemMap.end(); ++it) {
            if (optCollectionFolderTopLevel && !modelBelongsToFolder(it.key(), folderPath)) continue;
            if (!categorizedSet.contains(it.key())) uncatModels.append(it.key());
        }

        const int uncatCount = uncatModels.count();
        if (uncatCount > 0 || optShowEmptyCollections) {
            const QString key = optCollectionFolderTopLevel ? QString("folder:%1:%2").arg(folderPath, FILTER_UNCATEGORIZED)
                                                            : FILTER_UNCATEGORIZED;
            QTreeWidgetItem *uncategorizedNode = makeTreeParent(parent, key, FILTER_UNCATEGORIZED, "未分类 / Uncategorized", uncatCount);
            std::sort(uncatModels.begin(), uncatModels.end(), rankSort);
            if (optCollectionFolderSecondLevel && !optCollectionFolderTopLevel) {
                addFolderChildrenForCollection(uncategorizedNode, key, uncatModels);
            } else {
                addModelChildren(uncategorizedNode, uncatModels);
            }
        }

        for (const QString &colName : collectionNames) {
            QStringList models = collections.value(colName);
            if (optCollectionFolderTopLevel) {
                QMutableStringListIterator it(models);
                while (it.hasNext()) {
                    if (!modelBelongsToFolder(it.next(), folderPath)) it.remove();
                }
            }

            int visibleCount = 0;
            for(const QString &m : models) {
                if(visibleItemMap.contains(m)) visibleCount++;
            }

            if (visibleCount > 0 || optShowEmptyCollections) {
                const QString key = optCollectionFolderTopLevel ? QString("folder:%1:%2").arg(folderPath, colName)
                                                                : colName;
                QTreeWidgetItem *collectionNode = makeTreeParent(parent, key, colName, colName, visibleCount);
                if (optCollectionFolderSecondLevel && !optCollectionFolderTopLevel) {
                    addFolderChildrenForCollection(collectionNode, key, models);
                } else {
                    addModelChildren(collectionNode, models);
                }
            }
        }
    };

    if (optCollectionFolderTopLevel) {
        QMap<QString, QString> folderNames;
        for (auto it = visibleItemMap.begin(); it != visibleItemMap.end(); ++it) {
            QListWidgetItem *item = it.value();
            const QString folderPath = item->data(ROLE_MODEL_ROOT_PATH).toString();
            if (folderPath.isEmpty()) continue;
            QString folderName = item->data(ROLE_MODEL_ROOT_NAME).toString();
            if (folderName.isEmpty()) folderName = folderPath;
            folderNames.insert(folderPath, folderName);
        }

        QStringList folderPaths = folderNames.keys();
        std::sort(folderPaths.begin(), folderPaths.end(), [&](const QString &a, const QString &b) {
            return collator.compare(folderNames.value(a), folderNames.value(b)) < 0;
        });

        for (const QString &folderPath : folderPaths) {
            int folderVisibleCount = 0;
            for (auto it = visibleItemMap.begin(); it != visibleItemMap.end(); ++it) {
                if (modelBelongsToFolder(it.key(), folderPath)) folderVisibleCount++;
            }
            if (folderVisibleCount <= 0 && !optShowEmptyCollections) continue;

            const QString folderKey = "folder:" + folderPath;
            QTreeWidgetItem *folderNode = makeTreeParent(nullptr, folderKey, folderKey, folderNames.value(folderPath), folderVisibleCount);
            folderNode->setData(0, ROLE_IS_FOLDER_HEADER, true);
            folderNode->setData(0, ROLE_MODEL_ROOT_NAME, folderNames.value(folderPath));
            addCollectionGroup(folderNode, folderPath);
        }
    } else {
        addCollectionGroup(nullptr, QString());
    }

    // 恢复滚动条
    // 使用 QTimer 0 延时，确保 UI 布局完成后再滚动，否则可能滚不到位
    if (scrollPos > 0) {
        QTimer::singleShot(0, this, [this, scrollPos](){
            ui->collectionTree->verticalScrollBar()->setValue(scrollPos);
        });
    }
}

void MainWindow::cancelPendingTasks()
{
    // QThreadPool::clear() 会移除所有尚未开始的任务
    // 正在运行的任务无法强制停止，但它们很快就会结束。
    // 注意：backgroundThreadPool 用于侧边栏/收藏夹等静默缩略图加载，
    // 普通页面刷新不应清掉它，否则模型列表会留下占位叉号。
    if (threadPool) threadPool->clear();

    // 可选：如果之前的逻辑有正在下载的队列，也可以在这里清空
    // downloadQueue.clear();
    // isDownloading = false;
}

void MainWindow::syncTreeSelection(const QString &filePath)
{
    if (filePath.isEmpty()) return;

    bool found = false;

    std::function<bool(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem *node) {
        if (!node) return false;
        if (node->data(0, ROLE_FILE_PATH).toString() == filePath) {
            QTreeWidgetItem *parent = node->parent();
            while (parent) {
                parent->setExpanded(true);
                parent = parent->parent();
            }
            ui->collectionTree->setCurrentItem(node);
            node->setSelected(true);
            ui->collectionTree->scrollToItem(node, QAbstractItemView::PositionAtCenter);
            return true;
        }
        for (int i = 0; i < node->childCount(); ++i) {
            if (visit(node->child(i))) return true;
        }
        return false;
    };

    for (int i = 0; i < ui->collectionTree->topLevelItemCount(); ++i) {
        if (visit(ui->collectionTree->topLevelItem(i))) {
            found = true;
            break;
        }
    }
    Q_UNUSED(found);
}

void MainWindow::onMenuSwitchToAbout()
{
    ui->rootStack->setCurrentWidget(ui->pageAbout);
}

void MainWindow::onCheckUpdateClicked()
{
    startAppUpdateCheck(false);
}

void MainWindow::startAppUpdateCheck(bool silentIfLatest)
{
    ui->statusbar->showMessage("正在连接 GitHub 检查更新...", 3000);
    if (aboutPage) aboutPage->setCheckingForUpdates(true);

    QNetworkRequest request((QUrl(GITHUB_REPO_API)));
    request.setHeader(QNetworkRequest::UserAgentHeader, currentUserAgent);

    QNetworkReply *reply = netManager->get(request);
    reply->setProperty("silentIfLatest", silentIfLatest);
    connect(reply, &QNetworkReply::finished, this, [this, reply](){
        onUpdateApiReceived(reply);
    });
}

void MainWindow::onUpdateApiReceived(QNetworkReply *reply)
{
    const bool silentIfLatest = reply->property("silentIfLatest").toBool();
    if (aboutPage) aboutPage->setCheckingForUpdates(false);
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        if (silentIfLatest) {
            ui->statusbar->showMessage("自动检查更新失败: " + reply->errorString(), 4000);
        } else {
            QMessageBox::warning(this, "检查失败", "无法连接到 GitHub API:\n" + reply->errorString());
        }
        return;
    }

    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    QJsonObject root = doc.object();

    // GitHub API 返回的 tag_name (例如 "1.1.1" 或 "1.1.5")
    QString remoteTag = root["tag_name"].toString();
    QString htmlUrl = root["html_url"].toString();
    QString body = root["body"].toString();

    if (remoteTag.isEmpty()) {
        if (silentIfLatest) {
            ui->statusbar->showMessage("自动检查更新失败: 无法解析版本信息", 4000);
        } else {
            QMessageBox::warning(this, "错误", "无法解析版本信息 (Rate Limit Exceeded?)。");
        }
        return;
    }

    // === 版本比较逻辑 (适配不带 v 的情况) ===
    // 如果 remoteTag 带有 'v' 前缀而 CURRENT_VERSION 没有，可以手动去除
    if (remoteTag.startsWith("v", Qt::CaseInsensitive)) {
        remoteTag = remoteTag.mid(1);
    }

    // 简单比较：只要字符串不相等，且远程版本号通常比本地长或大
    // 更严谨的方法是用 semver 库，但这里我们可以写个简单的辅助判断

    bool hasNewVersion = false;

    if (remoteTag != CURRENT_VERSION) {
        // 分割版本号进行数字比较 (1.1.1 vs 1.0.0)
        QStringList remoteParts = remoteTag.split('.');
        QStringList localParts = CURRENT_VERSION.split('.');

        int len = qMax(remoteParts.size(), localParts.size());
        for (int i = 0; i < len; ++i) {
            int r = (i < remoteParts.size()) ? remoteParts[i].toInt() : 0;
            int l = (i < localParts.size()) ? localParts[i].toInt() : 0;

            if (r > l) {
                hasNewVersion = true;
                break;
            } else if (r < l) {
                // 本地版本比远程还新（开发版），不算更新
                hasNewVersion = false;
                break;
            }
        }
    }

    if (hasNewVersion) {
        // 发现新版本
        QMessageBox msgBox(this);
        msgBox.setWindowTitle("发现新版本");
        msgBox.setTextFormat(Qt::RichText);
        msgBox.setText(QString("<h3>🚀 发现新版本: %1</h3>"
                               "<p>当前版本: %2</p>"
                               "<hr>"
                               "<p><b>更新日志:</b></p><pre style='font-size:11px'>%3</pre>")
                           .arg(remoteTag)
                           .arg(CURRENT_VERSION)
                           .arg(body));

        QPushButton *btnGo = msgBox.addButton("前往下载 / Download", QMessageBox::AcceptRole);
        msgBox.addButton("稍后 / Later", QMessageBox::RejectRole);
        msgBox.exec();

        if (msgBox.clickedButton() == btnGo) {
            QDesktopServices::openUrl(QUrl(htmlUrl));
        }
    } else {
        if (silentIfLatest) {
            ui->statusbar->showMessage(QString("当前已是最新版本 (%1)").arg(CURRENT_VERSION), 3000);
        } else {
            QMessageBox::information(this, "检查更新", QString("当前已是最新版本 (%1)。").arg(CURRENT_VERSION));
        }
    }
}

QString MainWindow::getRandomUserAgent() {
    QStringList uas;
    // Chrome Win10
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/132.0.6834.83 Safari/537.36";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/133.0.6943.50 Safari/537.36";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36";
    uas << "Mozilla/5.0 (Windows NT 11.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
    // Edge Win10
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36 Edg/120.0.0.0";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36 Edg/121.0.0.0";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36 Edg/131.0.0.0";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/132.0.0.0 Safari/537.36 Edg/132.0.0.0";
    uas << "Mozilla/5.0 (Windows NT 11.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36 Edg/131.0.2903.99";
    // Windows Firefox
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:132.0) Gecko/20100101 Firefox/132.0";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:133.0) Gecko/20100101 Firefox/133.0";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:134.0) Gecko/20100101 Firefox/134.0";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:123.0) Gecko/20100101 Firefox/123.0";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:122.0) Gecko/20100101 Firefox/122.0";
    // macOS Chrome
    // Intel Mac
    uas << "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
    uas << "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/132.0.0.0 Safari/537.36";
    // macOS Safari
    uas << "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_7_1) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.6 Safari/605.1.15";
    uas << "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_1) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.1 Safari/605.1.15";
    // macOS Firefox
    uas << "Mozilla/5.0 (Macintosh; Intel Mac OS X 14.7; rv:132.0) Gecko/20100101 Firefox/132.0";
    // Linux
    uas << "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
    uas << "Mozilla/5.0 (X11; Linux x86_64; rv:132.0) Gecko/20100101 Firefox/132.0";
    uas << "Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:133.0) Gecko/20100101 Firefox/133.0";

    // 随机取一个
    int index = QRandomGenerator::global()->bounded(uas.size());
    return uas.at(index);
}

// 加载缓存
void MainWindow::loadUserGalleryCache() {
    imageCache.clear();
    QString configDir = qApp->applicationDirPath() + "/config";
    QFile file(configDir + "/user_gallery_cache.json");
    if (!file.open(QIODevice::ReadOnly)) return;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    QJsonObject root = doc.object();

    for (auto it = root.begin(); it != root.end(); ++it) {
        QJsonObject obj = it.value().toObject();
        UserImageInfo info;
        info.path = it.key();
        info.prompt = obj["p"].toString();
        info.negativePrompt = obj["np"].toString();
        info.parameters = obj["param"].toString();
        info.lastModified = obj["t"].toVariant().toLongLong();
        info.parserVersion = obj.value("pv").toInt(0);

        // 恢复 Tags (为了节省空间，JSON里可以不存tags，读取时解析，或者也存进去)
        // 这里建议直接解析，因为 parsePromptsToTags 是纯内存操作，很快
        info.cleanTags = parsePromptsToTags(info.prompt);
        info.negativeCleanTags = parsePromptsToTags(info.negativePrompt);

        imageCache.insert(info.path, info);
    }
}

// 保存缓存
void MainWindow::saveUserGalleryCache() {
    QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);

    QJsonObject root;
    // 遍历当前的 imageCache
    for (auto it = imageCache.begin(); it != imageCache.end(); ++it) {
        const UserImageInfo &info = it.value();
        QJsonObject obj;
        obj["p"] = info.prompt;
        obj["np"] = info.negativePrompt;
        obj["param"] = info.parameters;
        obj["t"] = QString::number(info.lastModified); // 存为字符串避免精度问题
        obj["pv"] = info.parserVersion;

        root.insert(info.path, obj);
    }

    QFile file(configDir + "/user_gallery_cache.json");
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)); // Compact 模式减小体积
    }
}

void MainWindow::updateModelListNames()
{
    // 暂时关闭排序，防止修改文本时列表乱跳
    ui->modelList->setSortingEnabled(false);

    for(int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!isModelListItem(item)) continue;

        // 获取文件名 (BaseName)
        QString baseName = item->data(ROLE_MODEL_NAME).toString();
        // 获取 Civitai 名
        QString civitName = item->data(ROLE_CIVITAI_NAME).toString();

        if (optUseCivitaiName && !civitName.isEmpty()) {
            item->setText(civitName);
        } else {
            item->setText(baseName);
        }
        applyModelUserNoteData(item);
    }

    // 恢复排序 (executeSort 会处理)
}
