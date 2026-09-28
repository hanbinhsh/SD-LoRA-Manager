#include "utils/civarchiveparser.h"
#include "utils/downloadcache.h"
#include "utils/downloadstatus.h"
#include "utils/fileutils.h"
#include "utils/gallerymetadata.h"
#include "utils/imagemetadataparser.h"
#include "utils/metadatainspection.h"
#include "utils/modelfilter.h"
#include "utils/modelimagematcher.h"
#include "utils/modellistcache.h"
#include "utils/modelmetadatacodec.h"
#include "utils/modelscanner.h"
#include "utils/pathutils.h"
#include "utils/previewimagestore.h"
#include "utils/tagutils.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtEndian>

namespace {
int failures = 0;

void check(bool ok, const char *message)
{
    if (!ok) {
        ++failures;
        qCritical() << "FAIL:" << message;
    }
}

void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write fixture");
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "read fixture");
    return file.readAll();
}

QJsonObject versionFixture()
{
    return QJsonDocument::fromJson(R"({
        "id": 42, "modelId": 7, "name": "v1", "baseModel": "SDXL",
        "model": {"id": 7, "name": "Example", "type": "LORA",
                  "creator": {"username": "Artist", "image": "avatar"},
                  "tags": ["style", "STYLE", "Portrait"]},
        "trainedWords": ["trigger,", "another"],
        "stats": {"downloadCount": 123, "thumbsUpCount": 5},
        "images": [
            {"url": "movie.mp4", "type": "video", "unknown": [1, 2]},
            {"url": "photo.png", "width": 512, "height": 768, "unknown": {"keep": true},
             "meta": {"prompt": "portrait, blue_hair", "negativePrompt": "blur",
                      "steps": "30", "cfgScale": "6.5", "seed": "1234567890123",
                      "sampler": "Euler", "unknownParameter": 123}},
            {"url": "second.png", "meta": {"steps": 20, "cfgScale": 4.5, "seed": 999}}
        ],
        "files": [
            {"name": "other.safetensors", "primary": true, "hashes": {"SHA256": "BBBBBBBB"}},
            {"name": "example.safetensors", "sizeKB": 2048, "hashes": {"SHA256": "AAAAAAAA"}}
        ],
        "unknownVersionField": {"keep": [1, 2, 3]}
    })").object();
}

void testMetadata(const QTemporaryDir &dir)
{
    const QJsonObject version = versionFixture();
    const QByteArray original = QJsonDocument(version).toJson();
    const ModelMeta parsed = ModelMetadata::parseVersion(version, dir.filePath("example.safetensors"), "example");
    check(parsed.name == "Example [v1]" && parsed.modelId == 7 && parsed.versionId == 42, "model identity");
    check(parsed.sha256 == "AAAAAAAA" && parsed.fileSizeMB == 2.0, "matching file chosen before primary");
    check(parsed.images.size() == 2 && parsed.images.first().prompt == "portrait, blue_hair", "display filters video only");
    check(parsed.images.first().seed == "1234567890123" && parsed.images.first().steps == "30"
          && parsed.images.first().cfgScale == "6.5", "string image parameters");
    check(parsed.images.last().steps == "20" && parsed.images.last().seed == "999", "numeric image parameters");
    check(parsed.trainedWordsGroups == QStringList({"trigger", "another"}), "trigger decoding");
    check(parsed.creatorName == "Artist" && parsed.modelTags == QStringList({"style", "Portrait"}), "attribution decoding");
    check(QJsonDocument(version).toJson() == original, "display decoding never mutates raw metadata");

    QJsonObject model{{"id", 7}, {"name", "Example"}, {"unknownModelField", QJsonArray{1, 2}},
                      {"modelVersions", QJsonArray{version}}};
    const auto merged = ModelMetadata::mergeCivitaiModelIntoVersion(version, model);
    for (auto it = version.begin(); it != version.end(); ++it) {
        if (it.key() != "model") check(merged.value(it.key()) == it.value(), "version fields preserved on merge");
    }
    for (auto it = model.begin(); it != model.end(); ++it)
        check(merged.value("model").toObject().value(it.key()) == it.value(), "complete model fields preserved");

    const QString path = dir.filePath("example.json");
    writeFile(path, original);
    ModelMeta fromDisk;
    fromDisk.filePath = dir.filePath("example.safetensors");
    check(ModelMetadata::readLocalJson(dir.path(), "example", fromDisk), "read local JSON");
    check(ModelMetadata::readLocalJson(dir.path(), "example", fromDisk) && fromDisk.images.size() == 2
          && fromDisk.trainedWordsGroups.size() == 2, "repeated reads replace instead of append");
    check(readFile(path) == original, "reading local JSON leaves bytes unchanged");
    const ModelMeta beforeFailure = fromDisk;
    writeFile(path, "{broken");
    check(!ModelMetadata::readLocalJson(dir.path(), "example", fromDisk)
          && fromDisk.name == beforeFailure.name, "invalid read leaves output unchanged");
    const ModelMeta empty = ModelMetadata::parseVersion({}, "", "local");
    check(empty.fileSizeMB == 0 && empty.downloadCount == 0 && !empty.nsfw, "empty metadata fields initialized");
}

void testMatcher(const QTemporaryDir &dir)
{
    using namespace ModelImageMatcher;
    const QString modelPath = dir.filePath("artist_style.v1.safetensors");
    const QByteArray header = R"({"__metadata__":{"ss_output_name":"InternalArtist"}})";
    QByteArray bytes(8, '\0');
    qToLittleEndian<quint64>(header.size(), bytes.data());
    bytes += header;
    writeFile(modelPath, bytes);
    const ModelUsageInput input{modelPath, "artist_style.v1", "LoRA", "", "abcdef1234567890"};
    const auto candidate = buildCandidate(input);
    check(safetensorsInternalName(modelPath) == "InternalArtist", "safetensors internal name");

    UserImageInfo comfy;
    comfy.parameters = "Source: ComfyUI\nComfyUI LoRAs: artist_style.v1: 0.8";
    comfy.lastModified = 200;
    const auto comfyUsage = imageUsage(comfy);
    check(matches(candidate, comfyUsage, {2, true}), "ComfyUI local filename matches despite different internal name");
    check(!matches(candidate, comfyUsage, {2, false}), "strict hash mode without ComfyUI fallback");
    comfy.parameters += "\nComfyUI Lora hashes: artist_style.v1: 9999999999";
    check(!matches(candidate, imageUsage(comfy), {2, true}), "mismatching hash must not fall back to name");
    comfy.parameters = "Source: ComfyUI\nComfyUI LoRAs: artist style.v1: 0.8";
    check(matches(candidate, imageUsage(comfy), {2, true}), "spaces and underscores equivalent");
    comfy.parameters = "Source: ComfyUI\nComfyUI LoRAs: artist_style.v2: 0.8";
    check(!matches(candidate, imageUsage(comfy), {0, true}), "dotted versions are not collapsed");

    UserImageInfo a1111;
    a1111.prompt = "<lora:InternalArtist:1>, portrait";
    a1111.lastModified = 100;
    check(matches(candidate, imageUsage(a1111), {0, false}), "A1111 internal name");
    check(!matches(candidate, imageUsage(a1111), {2, true}), "A1111 does not use ComfyUI fallback");
    a1111.parameters = "Lora hashes: \"InternalArtist: abcdef1234\"";
    check(matches(candidate, imageUsage(a1111), {2, false}), "short LoRA hash matches SHA prefix");

    const ModelUsageInput checkpoint{dir.filePath("base.ckpt"), "base", "Checkpoint", "Base model [v1]", "123456abcdef"};
    const auto base = buildCandidate(checkpoint);
    UserImageInfo baseImage;
    baseImage.parameters = "Steps: 20, Model: base, Model hash: 123456abcd";
    check(matches(base, imageUsage(baseImage), {2, false}), "A1111 checkpoint hash");
    baseImage.parameters = "Source: ComfyUI\nCheckpoint: base.ckpt";
    check(matches(base, imageUsage(baseImage), {2, true}), "ComfyUI checkpoint name fallback");
    check(!matches(base, imageUsage(baseImage), {2, false}), "checkpoint fallback setting applies");
    check(!matches(candidate, imageUsage(baseImage), {0, true}), "checkpoint names do not match LoRA");

    const auto noHash = buildCandidate({dir.filePath("bare.ckpt"), "bare", "Checkpoint", "", ""});
    baseImage.parameters = "Model: bare";
    check(matches(noHash, imageUsage(baseImage), {1, false}), "hash-preferred mode falls back when target has no hash");
    check(!matches(noHash, imageUsage(baseImage), {2, false}), "strict hash does not fall back for missing target hash");

    QMap<QString, UserImageInfo> cache{{"a", a1111}, {"b", comfy}, {"c", baseImage}};
    for (int mode = 0; mode <= 2; ++mode) {
        for (const bool fallback : {false, true}) {
            const auto stats = calculateUsage({input, checkpoint}, cache, mode, fallback);
            for (int i = 0; i < 2; ++i) {
                const auto target = i == 0 ? candidate : base;
                int count = 0;
                qint64 lastUsed = 0;
                for (const auto &image : cache) {
                    if (matches(target, imageUsage(image), {mode, fallback})) {
                        ++count;
                        lastUsed = qMax(lastUsed, image.lastModified);
                    }
                }
                check(stats[i].usageCount == count && stats[i].lastUsed == lastUsed,
                      "usage counts and gallery predicate agree in every mode");
            }
        }
    }
}

void testScanner(const QTemporaryDir &dir)
{
    const QString file = dir.filePath("one.ckpt");
    const QString json = dir.filePath("one.json");
    const QString cache = dir.filePath("cache/index.json");
    writeFile(file, "model");
    writeFile(json, R"({"id":1,"modelId":2,"name":"v1","model":{"name":"Model","type":"Checkpoint"},"images":[]})");
    const auto cold = ModelScanner::scanModelsWorker({dir.path(), dir.path()}, true, cache);
    const auto warm = ModelScanner::scanModelsWorker({dir.path()}, true, cache);
    check(cold.size() == 1 && warm.size() == 1, "scanner deduplicates roots");
    check(cold.first().meta.civitaiName == warm.first().meta.civitaiName
          && warm.first().meta.previewState == ModelPreviewState::KnownNoPreview, "warm and cold summaries equivalent");
    ModelListCache badCache(cache);
    badCache.insert(file, ModelListCache::fingerprint(QFileInfo(file), QFileInfo(json)), {{"name", "incomplete"}});
    check(badCache.save(), "save invalid summary fixture");
    check(ModelScanner::scanModelsWorker({dir.path()}, true, cache).first().meta.civitaiName == "Model [v1]",
          "malformed cached summaries are reparsed");
    writeFile(json, R"({"localOnly":true,"name":"edited","trainedWords":["new"],"images":[]})");
    check(ModelScanner::scanModelsWorker({dir.path()}, true, cache).first().meta.trainedWords == QStringList{"new"},
          "changed metadata invalidates warm summary");
    writeFile(json, "{broken");
    check(ModelScanner::scanModelsWorker({dir.path()}, true, cache).first().meta.previewState == ModelPreviewState::MissingOrUnknown,
          "invalid JSON is not classified as known no preview");
}

void testPreview(const QTemporaryDir &dir)
{
    QImage image(16, 24, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    check(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"), "create PNG fixture");
    PreviewMetadataPayload payload;
    payload.prompt = "portrait, blue_hair";
    payload.negativePrompt = "blur";
    payload.steps = "20";
    payload.cfgScale = "7.5";
    payload.seed = "1234567890123";
    payload.sampler = "Euler";
    payload.width = 16;
    payload.height = 24;
    const QString path = dir.filePath("preview.png");
    check(PreviewImageStore::savePreviewImageWithMetadata(bytes, path, payload), "write preview metadata");
    check(PreviewImageStore::previewFileAlreadyHasPromptMetadata(path), "detect existing preview metadata");
    const auto parsed = parseImageMetadataFromFile(path);
    check(parsed.positivePrompt == payload.prompt && parsed.negativePrompt == payload.negativePrompt,
          "preview parameters remain readable by shared image parser");
    UserImageInfo info;
    GalleryMetadata::parseImage(path, info, true, {"blue_hair"});
    check(info.parserVersion == GalleryMetadata::ParserVersion && info.cleanTags == QStringList{"portrait"},
          "gallery parser uses shared tag cleaning and filtering");
    payload.prompt = "changed";
    check(PreviewImageStore::ensurePreviewImageMetadata(path, payload), "rewrite existing preview metadata");
    check(parseImageMetadataFromFile(path).positivePrompt == "changed", "rewritten metadata is current");
    check(!PreviewImageStore::savePreviewImageWithMetadata("invalid", dir.filePath("raw.png"), payload)
          && readFile(dir.filePath("raw.png")) == "invalid", "undecodable image retains raw fallback");
}

void testArchive()
{
    const QJsonObject version = versionFixture();
    QJsonObject modelRoot, hint;
    const QString url = "https://civarchive.com/sha256/aaaaaaaa";
    check(CivArchiveParser::parseCivArchivePayload(QJsonDocument(version).toJson(), url, modelRoot, hint),
          "parse archive version JSON");
    check(hint == version && modelRoot.value("metadataSource") == "civarchive",
          "archive version fields retained");
    const QByteArray embedded = "<script id=\"__NEXT_DATA__\">"
        + QJsonDocument(QJsonObject{{"props", QJsonObject{{"version", version}}}}).toJson()
        + "</script>";
    check(CivArchiveParser::parseCivArchivePayload(embedded, url, modelRoot, hint)
          && hint.value("images") == version.value("images"), "embedded archive JSON retains image metadata");
    check(!CivArchiveParser::parseCivArchivePayload("<html>not found</html>", url, modelRoot, hint)
          && modelRoot.isEmpty() && hint.isEmpty(), "unrecognized archive payload resets outputs");
    MetadataSyncJob job;
    job.snapshot.currentSha256 = "ABCDEF123456";
    job.snapshot.modelId = 7;
    job.snapshot.currentVersionId = 42;
    check(CivArchiveParser::civArchiveLookupUrl(job).path() == "/sha256/abcdef123456", "archive hash lookup preferred");
    job.snapshot.currentSha256.clear();
    check(CivArchiveParser::civArchiveLookupUrl(job).query().contains("modelVersionId=42"), "archive ID fallback");
}

void testPathsAndFilters(const QTemporaryDir &dir)
{
    const QString first = dir.filePath("model.safetensors");
    writeFile(first, "model");
    writeFile(dir.filePath("model_1.safetensors"), "model");
    check(FileUtils::uniqueFilePath(dir.path(), "model.safetensors") == dir.filePath("model_2.safetensors"),
          "shared unique file naming");
    QStringList paths;
    QSet<QString> disabled;
    PathUtils::applyPathEntries({{" ", true}, {dir.path(), false}, {dir.path(), true}}, paths, disabled);
    check(paths == QStringList{dir.path()} && disabled.contains(dir.path()), "blank paths ignored and first duplicate wins");
    check(PathUtils::collectEnabledPaths(paths, disabled).isEmpty(), "disabled path excluded");
    check(PathUtils::buildPathEntries(paths, disabled).first().enabled == false, "path state round-trip");
    ModelFilter::Record record{{"local filename", "Civitai name", "custom trigger", "private note"}, "SDXL", "lycoris"};
    check(ModelFilter::matches(record, "CUSTOM TRIGGER", "All", "All"), "shared search includes custom triggers");
    check(ModelFilter::matches(record, "Civitai name", "SDXL", "LoRA"), "shared search and normalized type");
    check(!ModelFilter::matches(record, "", "SD1.5", "All"), "base filter");
    check(!ModelFilter::matches(record, "", "All", "Checkpoint"), "type filter");
    check(TagUtils::normalizedGalleryTagKey(" A_B  ") == "a b"
          && TagUtils::normalizedGalleryTagKey("a-b") == "a-b", "gallery key normalization preserves hyphens");
    check(TagUtils::parsePromptTags("{\"prompt\":{}}", true, {}).isEmpty(), "workflow JSON never becomes tags");
}

void testDownloadCache(const QTemporaryDir &dir)
{
    using namespace DownloadStatus;
    const QString path = dir.filePath("downloads.json");
    check(DownloadCache::load(path).error.isEmpty(), "missing cache is an empty successful load");
    ModelUpdateInfo info;
    info.filePath = dir.filePath("model.safetensors");
    info.displayName = "Model 10";
    info.hasUpdate = true;
    info.modelId = 7;
    info.currentVersionId = 42;
    info.latestVersionId = 43;
    info.sizeMB = 123.5;
    info.previewState = ModelPreviewState::KnownNoPreview;
    info.metadataSource = "civarchive";
    info.sourceUrl = "https://civarchive.com/models/7";
    info.latestVersionJson = versionFixture();
    writeFile(info.filePath, "model");
    QString error;
    check(DownloadCache::save(path, {{info, "已忽略更新", false}}, &error), "atomic cache save");
    auto result = DownloadCache::load(path);
    check(result.error.isEmpty() && result.entries.size() == 1, "cache load");
    if (result.entries.isEmpty()) return;
    const auto entry = result.entries.first();
    check(entry.sourceAvailable && entry.status == "已忽略更新", "source existence and ignored state");
    check(entry.info.latestVersionJson == info.latestVersionJson, "cache preserves complete API JSON and image metadata");
    check(entry.info.metadataSource == info.metadataSource && entry.info.sourceUrl == info.sourceUrl
          && entry.info.previewState == info.previewState && entry.info.latestVersionId == 43
          && entry.info.sizeMB == 123.5, "identity, source, size and preview state round-trip");
    auto duplicate = info;
    duplicate.displayName = "Latest name";
    check(DownloadCache::save(path, {{info, "已是最新", false}, {duplicate, "发现新版本", false}}), "duplicate fixture");
    result = DownloadCache::load(path);
    check(result.entries.size() == 1 && result.entries.first().info.displayName == "Latest name", "duplicate paths resolve once");
    writeFile(path, "{broken");
    check(!DownloadCache::load(path).error.isEmpty(), "corrupt cache reported");
    writeFile(path, "{}");
    check(!DownloadCache::load(path).error.isEmpty(), "missing items is not an empty valid cache");

    check(category("旧版共存：本地已存在新版本") == "coexisting", "coexistence before local category");
    check(category("已忽略更新") == "ignored" && category("检查失败: HTTP 429") == "errors", "shared categories");
    check(cardAction("已是最新", false) == CardAction::Check
          && cardAction("本地/已编辑模型，已跳过", false) == CardAction::Check
          && cardAction("检查失败: HTTP 404", true) == CardAction::Check, "non-download cards check again");
    check(cardAction("发现新版本", true) == CardAction::Download
          && cardAction("下载中", true) == CardAction::Disabled
          && cardAction("校验中...", true) == CardAction::Disabled, "active downloads cannot restart");
    check(isCheckFailure("无法计算 Hash，无法判断") && !isChecking("无法计算 Hash，无法判断"), "hash failure is not active work");
    check(!isDownloadFailure("检查失败: HTTP 429") && isDownloadFailure("下载失败: disk full"), "separate check and download retries");
    check(persistedStatus("检查中...", false).isEmpty()
          && persistedStatus("计算 Hash 中...", false).isEmpty(), "incomplete checks not persisted");
    check(persistedStatus("无法计算 Hash，无法判断", false) == "无法计算 Hash，无法判断", "hash failures survive restart");
    check(persistedStatus("校验中...", true) == "发现新版本", "interrupted download restored as idle");
    check(preserveIgnored("已忽略更新", "发现新版本") == "已忽略更新", "update refresh retains explicit ignore");
}

void testInspection(const QTemporaryDir &dir)
{
    const QString model = dir.filePath("model.safetensors");
    const QString json = dir.filePath("model.json");
    writeFile(model, "model");
    MetadataScanItem item;
    item.filePath = model;
    item.jsonPath = json;
    item.displayName = "Model";
    check(MetadataInspection::scan({item}).first().category == "missing", "metadata scan missing");
    writeFile(json, "{broken");
    check(MetadataInspection::scan({item}).first().category == "invalid", "metadata scan invalid");
    writeFile(json, R"({"modelId":7,"id":42,"syncedAt":"2026-01-02T03:04:05Z"})");
    const auto scanned = MetadataInspection::scan({item}).first();
    check(scanned.category == "existing" && scanned.modelIdText == "7"
          && scanned.versionIdText == "42" && !scanned.lastSyncedAt.isEmpty(), "metadata scan identity");
    item.syncFailure = "failed";
    check(MetadataInspection::scan({item}).first().category == "failed", "failure cache classification");
    check(!MetadataInspection::healthCheck({item}).isEmpty(), "health check reuses metadata scan");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!dir.isValid()) return 1;
    const QString group = app.arguments().value(1);
    if (group == "metadata") testMetadata(dir);
    else if (group == "matching") testMatcher(dir);
    else if (group == "scanner") testScanner(dir);
    else if (group == "preview") testPreview(dir);
    else if (group == "archive") testArchive();
    else if (group == "paths_filters") testPathsAndFilters(dir);
    else if (group == "inspection") testInspection(dir);
    else if (group == "downloads") testDownloadCache(dir);
    else return 2;
    qInfo() << group << (failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
