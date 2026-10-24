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
#include "utils/modelbrowserurl.h"
#include "utils/modelscanner.h"
#include "utils/pathutils.h"
#include "utils/previewimagestore.h"
#include "utils/tagutils.h"
#include "utils/tagusage.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstdio>

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

void testTags(const QTemporaryDir &dir)
{
    check(TagUtils::cleanPromptTag("(blue_hair:1.2)") == "blue_hair", "weight inside brackets is stripped");
    check(TagUtils::cleanPromptTag("((blue_hair:1.2):0.8)") == "blue_hair", "nested weights are stripped");
    check(TagUtils::cleanPromptTag("[blue_hair]") == "blue_hair", "unweighted wrappers still work");
    check(TagUtils::cleanPromptTag("(blue_hair:-.5)") == "blue_hair", "signed decimal weight");
    check(TagUtils::cleanPromptTag(":)") == ":)", "emoticons remain intact");
    check(TagUsage::promptTags(R"({"prompt":"not a tag, neither is this"})").isEmpty(), "JSON is not counted as tags");
    check(TagUsage::promptTags("(empty)").isEmpty(), "negative empty marker is not counted");
    check(TagUsage::promptTags("(Blue_Hair:1.2), blue hair, BLUE-HAIR, BREAK, flower")
          == QStringList{"Blue_Hair", "flower"}, "prompt stats deduplicate case, separators and weights");

    const QString cachePath = dir.filePath("tag-cache.json");
    writeFile(cachePath, R"json({
        "a.png":{"p":"(Blue_Hair:1.2), blue hair, FLOWER, BREAK", "np":"blue_hair, blur, blur"},
        "b.png":{"p":"blue-hair, flower", "np":"(empty)"},
        "c.png":{"p":"{\"prompt\":\"not a tag, neither is this\"}"},
        "__metadata__":{"p":"ghost"},
        "invalid":"ghost"
    })json");
    const TagUsage::Statistics stats = TagUsage::readCache(cachePath);
    check(stats.positive.values.size() == 2 && stats.positive.values.value("blue hair") == 2
          && stats.positive.values.value("flower") == 2, "each positive tag counts once per image");
    check(stats.negative.values.size() == 2 && stats.negative.values.value("blue hair") == 1,
          "negative counts remain separate");
    const auto combined = TagUsage::readCache(cachePath, TagUsage::Combined).combined;
    check(combined.values.size() == 3 && combined.values.value("blue hair") == 2,
          "WD14 combined counts deduplicate across positive and negative prompts in one image");
    check(TagUsage::readCache(cachePath, TagUsage::Positive).negative.values.isEmpty(),
          "single-scope callers do not build unused statistics");
    const auto rows = stats.rows();
    check(rows.size() == 4 && rows.first().tag == "Blue_Hair" && rows.first().count == 2,
          "rows retain first display text and numeric descending order");
    check(stats.rows(0).size() == 2 && stats.rows(1).size() == 2, "both pickers can filter one statistics snapshot");
    TagUsage::Counts usageAnalysis;
    usageAnalysis.addImage({"Blue_Hair", "blue hair", "BLUE_HAIR"});
    usageAnalysis.addImage({"blue hair"});
    check(usageAnalysis.values.value("blue hair") == 2, "usage analysis shares per-image counting");
    writeFile(cachePath, "{broken");
    check(TagUsage::readCache(cachePath).rows().isEmpty(), "bad cache safely returns no rows");
    check(TagUsage::readCache(dir.filePath("missing.json")).rows().isEmpty(), "missing cache safely returns no rows");

    const QString imagePath = dir.filePath("metadata-removed.png");
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(Qt::blue);
    check(image.save(imagePath, "PNG"), "write plain image");
    UserImageInfo stale;
    stale.path = imagePath;
    stale.lastModified = 123;
    stale.prompt = "old prompt";
    stale.negativePrompt = "old negative";
    stale.parameters = "Model: old-model";
    stale.cleanTags = {"old tag"};
    stale.negativeCleanTags = {"old negative"};
    stale.parserVersion = GalleryMetadata::ParserVersion;
    GalleryMetadata::parseImage(imagePath, stale, true, {});
    check(stale.prompt.isEmpty() && stale.negativePrompt.isEmpty() && stale.parameters.isEmpty()
          && stale.cleanTags.isEmpty() && stale.negativeCleanTags.isEmpty(), "rescan clears removed metadata and tag lists");
    check(stale.parserVersion == GalleryMetadata::ParserVersion && stale.path == imagePath && stale.lastModified == 123,
          "metadata-free image completes parsing without changing its identity");
    writeFile(imagePath, readFile(imagePath).left(33));
    GalleryMetadata::parseImage(imagePath, stale, true, {});
    check(stale.parserVersion == 0, "truncated PNG remains retryable even when its header is readable");
    stale.parserVersion = GalleryMetadata::ParserVersion;
    GalleryMetadata::parseImage(dir.filePath("missing.png"), stale, true, {});
    check(stale.parserVersion == 0, "missing file cannot retain a successful parser version");
}

void testComfyModels(const QTemporaryDir &dir, const QString &attachmentPath)
{
    const QString path = dir.filePath("comfy.png");
    auto saveImage = [&](const QJsonObject &prompt, const QJsonObject &workflow = {}) {
        QImage image(16, 16, QImage::Format_RGB32);
        image.fill(Qt::blue);
        if (!prompt.isEmpty()) image.setText("prompt", QString::fromUtf8(QJsonDocument(prompt).toJson()));
        if (!workflow.isEmpty()) image.setText("workflow", QString::fromUtf8(QJsonDocument(workflow).toJson()));
        check(image.save(path, "PNG"), "save ComfyUI image fixture");
        return parseImageMetadataFromFile(path);
    };
    auto setInput = [](QJsonObject &nodes, const QString &id, const QString &key, const QJsonValue &value) {
        QJsonObject node = nodes.value(id).toObject();
        QJsonObject inputs = node.value("inputs").toObject();
        inputs.insert(key, value);
        node.insert("inputs", inputs);
        nodes.insert(id, node);
    };
    const QJsonObject original = QJsonDocument::fromJson(R"({
        "1": {"class_type":"CheckpointLoaderSimple", "inputs":{"ckpt_name":"Illustrious.safetensors"}},
        "2": {"class_type":"UNETLoader", "inputs":{"unet_name":"anima.safetensors","weight_dtype":"default"}},
        "4": {"class_type":"CLIPTextEncode", "inputs":{"text":"portrait, blue_hair","clip":["6",0]}},
        "5": {"class_type":"CLIPTextEncode", "inputs":{"text":"blur","clip":["6",0]}},
        "6": {"class_type":"CLIPLoader", "inputs":{"clip_name":"encoder.safetensors"}},
        "16": {"class_type":"LoraLoader", "inputs":{"model":["2",0],"clip":["6",0],
                 "lora_name":"style.safetensors","strength_model":0.8}},
        "30": {"class_type":"KSampler", "inputs":{"model":["16",0],"positive":["4",0],"negative":["5",0],
                 "seed":123,"steps":20,"cfg":5,"sampler_name":"euler","scheduler":"normal"}},
        "99": {"class_type":"KSamplerSelect", "inputs":{"sampler_name":"euler"}}
    })").object();
    auto parsed = saveImage(original);
    check(parsed.checkpoint == "anima.safetensors" && parsed.sourceType == "ComfyUI",
          "connected UNet wins over an unconnected CheckpointLoader");
    check(parsed.positivePrompt == "portrait, blue_hair" && parsed.negativePrompt == "blur"
          && parsed.loraDescriptions == QStringList{"style.safetensors: 0.8"},
          "sampler selection preserves prompts and active LoRA chain, ignoring KSamplerSelect");
    check(parsed.parametersText.contains("Checkpoint: anima.safetensors")
          && !parsed.parametersText.contains("Illustrious"), "gallery parameters contain only the sampled base model");

    const QByteArray originalBytes = readFile(path);
    UserImageInfo info;
    info.parserVersion = 7;
    info.parameters = "Source: ComfyUI\nCheckpoint: Illustrious.safetensors";
    GalleryMetadata::parseImage(path, info, true, {});
    check(info.parserVersion > 7 && info.parserVersion == GalleryMetadata::ParserVersion
          && info.parameters.contains("Checkpoint: anima.safetensors")
          && !info.parameters.contains("Illustrious"), "reparsing replaces stale cached base model");
    using namespace ModelImageMatcher;
    const auto anima = buildCandidate({dir.filePath("anima.safetensors"), "anima", "Checkpoint", "", ""});
    const auto ill = buildCandidate({dir.filePath("Illustrious.safetensors"), "Illustrious", "Checkpoint", "", ""});
    check(matches(anima, imageUsage(info), {2, true}) && !matches(ill, imageUsage(info), {2, true}),
          "gallery name fallback matches Anima, not the idle Illustrious model");
    check(!matches(anima, imageUsage(info), {2, false}), "strict hash option is unchanged");
    check(readFile(path) == originalBytes, "metadata parsing never changes the source image");

    auto nodes = original;
    setInput(nodes, "16", "clip", QJsonArray{"1", 1});
    check(saveImage(nodes).checkpoint == "anima.safetensors", "CLIP from another checkpoint is not the base model");
    nodes.insert("2", QJsonObject{{"class_type", "UnknownLoader"}, {"inputs", QJsonObject{}}});
    check(saveImage(nodes).checkpoint.isEmpty(), "unknown model chain does not guess from CLIP or global loaders");
    setInput(nodes, "30", "model", QJsonArray{"missing", 0});
    check(saveImage(nodes).checkpoint.isEmpty(), "broken model link does not select an idle checkpoint");
    setInput(nodes, "30", "model", QJsonArray{"30", 0});
    check(saveImage(nodes).checkpoint.isEmpty(), "cyclic model input terminates without guessing");

    for (const QString &loader : {QString("UNETLoader"), QString("UnetLoaderGGUF"), QString("DiffusionModelLoader")}) {
        nodes = original;
        QJsonObject node = nodes.value("2").toObject();
        node.insert("class_type", loader);
        nodes.insert("2", node);
        check(saveImage(nodes).checkpoint == "anima.safetensors", "supported diffusion loaders follow the same model chain");
    }
    nodes = original;
    setInput(nodes, "30", "model", QJsonArray{"1", 0});
    check(saveImage(nodes).checkpoint == "Illustrious.safetensors", "a connected checkpoint still wins over an idle UNet");

    const QJsonObject workflow = QJsonDocument::fromJson(R"({
        "nodes":[
            {"id":1,"type":"CheckpointLoaderSimple","widgets_values":["Illustrious.safetensors"]},
            {"id":2,"type":"UNETLoader","widgets_values":["anima.safetensors","default"]},
            {"id":4,"type":"CLIPTextEncode","widgets_values":["portrait, blue_hair"]},
            {"id":5,"type":"CLIPTextEncode","widgets_values":["blur"]},
            {"id":16,"type":"LoraLoaderModelOnly","widgets_values":["style.safetensors",0.8],
             "inputs":[{"name":"model","link":1}]},
            {"id":30,"type":"KSampler","inputs":[{"name":"model","link":2},
                {"name":"positive","link":3},{"name":"negative","link":4}]}
        ],
        "links":[[1,2,0,16,0,"MODEL"],[2,16,0,30,0,"MODEL"],
                 [3,4,0,30,1,"CONDITIONING"],[4,5,0,30,2,"CONDITIONING"]]
    })").object();
    parsed = saveImage({}, workflow);
    check(parsed.checkpoint == "anima.safetensors" && parsed.positivePrompt == "portrait, blue_hair"
          && parsed.loraDescriptions == QStringList{"style.safetensors: 0.8"},
          "workflow-only metadata traces top-level links and UNet widgets");
    QJsonObject managerGraph = workflow;
    QJsonArray managerNodes = managerGraph.value("nodes").toArray();
    QJsonObject manager = managerNodes.at(4).toObject();
    manager.insert("type", "Lora Loader (LoraManager)");
    manager.insert("widgets_values", QJsonArray{"<lora:inactive:1>, <lora:active:0.5>", QJsonArray{
        QJsonObject{{"name", "inactive"}, {"active", false}, {"strength", 1}},
        QJsonObject{{"name", "active"}, {"active", true}, {"strength", 0.5}}}});
    managerNodes.replace(4, manager);
    managerGraph.insert("nodes", managerNodes);
    parsed = saveImage({}, managerGraph);
    check(parsed.checkpoint == "anima.safetensors" && parsed.loraDescriptions == QStringList{"active: 0.5"},
          "LoraManager still respects structured active flags while tracing UNet");
    for (const QString &loader : {QString("UnetLoaderGGUF"), QString("DiffusionModelLoader"),
                                  QString("CheckpointLoaderSimple"), QString("CheckpointLoader")}) {
        QJsonObject graph = workflow;
        QJsonArray graphNodes = graph.value("nodes").toArray();
        auto loaderNode = graphNodes.at(1).toObject();
        loaderNode.insert("type", loader);
        loaderNode.insert("widgets_values", loader == "CheckpointLoader"
                                                ? QJsonArray{"config.yaml", "anima.safetensors"}
                                                : QJsonArray{"anima.safetensors"});
        graphNodes.replace(1, loaderNode);
        graph.insert("nodes", graphNodes);
        check(saveImage({}, graph).checkpoint == "anima.safetensors", "workflow loader names are read from the right widget");
    }

    // Prefer the executed prompt's sampler ID when supplementing from a canvas
    // that also contains a different (unused) sampling branch.
    QJsonObject unrelatedGraph = workflow;
    QJsonArray graphNodes = unrelatedGraph.value("nodes").toArray();
    auto differentSampler = graphNodes.last().toObject();
    differentSampler.insert("id", 90);
    differentSampler.insert("inputs", QJsonArray{QJsonObject{{"name", "model"}, {"link", 90}}});
    graphNodes.removeLast();
    graphNodes.append(differentSampler);
    unrelatedGraph.insert("nodes", graphNodes);
    unrelatedGraph.insert("links", QJsonArray{QJsonArray{90, 1, 0, 90, 0, "MODEL"}});
    nodes = original;
    setInput(nodes, "30", "model", QJsonArray{"missing", 0});
    check(saveImage(nodes, unrelatedGraph).checkpoint.isEmpty(),
          "workflow supplement cannot substitute a different sampler's checkpoint");

    nodes = original;
    nodes.insert("25", QJsonObject{{"class_type", "CFGGuider"}, {"inputs", QJsonObject{
        {"model", QJsonArray{"16", 0}}, {"positive", QJsonArray{"4", 0}},
        {"negative", QJsonArray{"5", 0}}, {"cfg", 4}}}});
    nodes.insert("30", QJsonObject{{"class_type", "SamplerCustomAdvanced"},
                                  {"inputs", QJsonObject{{"guider", QJsonArray{"25", 0}}}}});
    parsed = saveImage(nodes);
    check(parsed.checkpoint == "anima.safetensors" && parsed.positivePrompt == "portrait, blue_hair"
          && parsed.cfg == "4", "custom sampler resolves its model through CFGGuider");

    // Reduced from the reported workflow: node 33 is present but its outputs
    // are disconnected. The saved image comes from KSampler 3, not node 33.
    const QJsonObject twoBranches = QJsonDocument::fromJson(R"json({
        "3":{"class_type":"KSampler","inputs":{"seed":554710696109726,"steps":32,"cfg":5.5,
            "sampler_name":"euler_ancestral","scheduler":"beta","model":["16",0],
            "positive":["6",0],"negative":["7",0]}},
        "4":{"class_type":"CheckpointLoaderSimple","inputs":{"ckpt_name":"waiIllustriousSDXL_v170.safetensors"}},
        "6":{"class_type":"CLIPTextEncode","inputs":{"text":"portrait","clip":["16",1]}},
        "7":{"class_type":"CLIPTextEncode","inputs":{"text":"blur","clip":["16",1]}},
        "8":{"class_type":"VAEDecode","inputs":{"samples":["3",0]}},
        "9":{"class_type":"SaveImage","inputs":{"images":["8",0]}},
        "16":{"class_type":"Lora Loader (LoraManager)","inputs":{"model":["104",0],"loras":{"__value__":[
            {"name":"inactive","active":false,"strength":1},
            {"name":"yachiyo_v1","active":true,"strength":"0.60"},
            {"name":"mary-harker_anima","active":true,"strength":"0.65"},
            {"name":"kanzarin_v1_anima","active":true,"strength":"0.90"}]}}},
        "33":{"class_type":"SamplerCustomAdvanced","inputs":{"noise":["35",0],"guider":["36",0],
            "sampler":["34",0],"sigmas":["37",0]}},
        "34":{"class_type":"Z2SamplerExperimental","inputs":{"mode":"surrogate"}},
        "35":{"class_type":"RandomNoise","inputs":{"noise_seed":500818567943651}},
        "36":{"class_type":"CFGGuider","inputs":{"cfg":6.5,"model":["104",0],
            "positive":["6",0],"negative":["7",0]}},
        "37":{"class_type":"BasicScheduler","inputs":{"steps":50,"scheduler":"karras","model":["104",0]}},
        "87":{"class_type":"PreviewImage","inputs":{"images":["86",0]}},
        "86":{"class_type":"MaskToImage","inputs":{}},
        "104":{"class_type":"UNETLoader","inputs":{"unet_name":"anima_baseV10.safetensors"}}
    })json").object();
    auto verifySavedParameters = [](const ParsedImageMetadata &meta) {
        check(meta.seed == "554710696109726", "reported seed is a complete decimal integer");
        check(meta.steps == "32" && meta.cfg == "5.5" && meta.sampler == "euler_ancestral"
              && meta.scheduler == "beta", "all generation settings come from the saved-image sampler");
        check(meta.checkpoint == "anima_baseV10.safetensors", "saved-image model chain still resolves UNet");
        check(meta.loraDescriptions == QStringList{"yachiyo_v1: 0.60", "mary-harker_anima: 0.65", "kanzarin_v1_anima: 0.90"},
              "saved-image chain includes exactly the three active LoRAs");
    };
    verifySavedParameters(saveImage(twoBranches));

    // Workflow-only representation also includes the unnamed MODEL/CONDITIONING
    // reroutes and the UI-only randomize widget found in the attachment.
    const QJsonObject twoBranchWorkflow = QJsonDocument::fromJson(R"json({"nodes":[
        {"id":3,"type":"KSampler","widgets_values":[554710696109726,"randomize",32,5.5,"euler_ancestral","beta",1],
            "inputs":[{"name":"model","link":22},{"name":"positive","link":61},{"name":"negative","link":63}]},
        {"id":6,"type":"CLIPTextEncode","widgets_values":["portrait"]},
        {"id":7,"type":"CLIPTextEncode","widgets_values":["blur"]},
        {"id":8,"type":"VAEDecode","inputs":[{"name":"samples","link":147}]},
        {"id":9,"type":"SaveImage","inputs":[{"name":"images","link":9}]},
        {"id":16,"type":"Lora Loader (LoraManager)","inputs":[{"name":"model","link":157}],"widgets_values":[
            "unused text",[{"name":"inactive","active":false,"strength":1},
                {"name":"yachiyo_v1","active":true,"strength":"0.60"},
                {"name":"mary-harker_anima","active":true,"strength":"0.65"},
                {"name":"kanzarin_v1_anima","active":true,"strength":"0.90"}]]},
        {"id":33,"type":"SamplerCustomAdvanced","inputs":[{"name":"guider","link":27},
            {"name":"noise","link":26},{"name":"sampler","link":148},{"name":"sigmas","link":31}]},
        {"id":34,"type":"Z2SamplerExperimental","widgets_values":["surrogate"]},
        {"id":35,"type":"RandomNoise","widgets_values":[500818567943651,"randomize"]},
        {"id":36,"type":"CFGGuider","widgets_values":[6.5],"inputs":[{"name":"model","link":158}]},
        {"id":37,"type":"BasicScheduler","widgets_values":["karras",50,1]},
        {"id":43,"type":"Reroute","inputs":[{"name":"","link":153}]},
        {"id":44,"type":"Reroute","inputs":[{"name":"","link":76}]},
        {"id":103,"type":"Reroute","inputs":[{"name":"","link":166}]},
        {"id":104,"type":"UNETLoader","widgets_values":["anima_baseV10.safetensors","default"]}
    ],"links":[[22,16,0,3,0,"MODEL"],[61,43,0,3,1,"CONDITIONING"],[63,44,0,3,2,"CONDITIONING"],
        [153,6,0,43,0,"CONDITIONING"],[76,7,0,44,0,"CONDITIONING"],[157,103,0,16,0,"MODEL"],
        [158,103,0,36,0,"MODEL"],[166,104,0,103,0,"MODEL"],[147,3,0,8,0,"LATENT"],[9,8,0,9,0,"IMAGE"],
        [27,36,0,33,1,"GUIDER"],[26,35,0,33,0,"NOISE"],[148,34,0,33,2,"SAMPLER"],[31,37,0,33,3,"SIGMAS"]]})json").object();
    parsed = saveImage({}, twoBranchWorkflow);
    verifySavedParameters(parsed);
    check(parsed.positivePrompt == "portrait" && parsed.negativePrompt == "blur", "workflow reroutes preserve prompt text");
    verifySavedParameters(saveImage(twoBranches, twoBranchWorkflow));

    nodes = twoBranches;
    setInput(nodes, "8", "samples", QJsonArray{"33", 0});
    parsed = saveImage(nodes);
    check(parsed.seed == "500818567943651" && parsed.cfg == "6.5" && parsed.steps == "50"
          && parsed.scheduler == "karras" && parsed.sampler == "Z2SamplerExperimental",
          "connected custom sampler reads its own noise, sampler and sigmas, never KSampler's settings");
    QJsonObject customWorkflow = twoBranchWorkflow;
    QJsonArray customLinks = customWorkflow.value("links").toArray();
    for (int i = 0; i < customLinks.size(); ++i) {
        QJsonArray link = customLinks.at(i).toArray();
        if (link.first().toInt() == 147) { link.replace(1, 33); customLinks.replace(i, link); }
    }
    customWorkflow.insert("links", customLinks);
    parsed = saveImage({}, customWorkflow);
    check(parsed.seed == "500818567943651" && parsed.steps == "50" && parsed.scheduler == "karras",
          "workflow-only custom sampler parameters follow connected helper nodes");

    nodes = twoBranches;
    for (qint64 seed : {Q_INT64_C(554710696109726), Q_INT64_C(9007199254740993), Q_INT64_C(9223372036854775807)}) {
        setInput(nodes, "3", "seed", QJsonValue(seed));
        check(saveImage(nodes).seed == QString::number(seed), "integer seeds preserve every digit, including values beyond double precision");
    }
    QJsonObject namedWorkflow = twoBranchWorkflow;
    QJsonArray namedNodes = namedWorkflow.value("nodes").toArray();
    QJsonObject namedSampler = namedNodes.first().toObject();
    namedSampler.remove("widgets_values");
    namedSampler.insert("widgets_values_named", twoBranches.value("3").toObject().value("inputs"));
    namedNodes.replace(0, namedSampler);
    namedWorkflow.insert("nodes", namedNodes);
    verifySavedParameters(saveImage({}, namedWorkflow));

    if (!attachmentPath.isEmpty()) {
        const QString raw = QString::fromUtf8(readFile(attachmentPath));
        const QString promptMarker = "===== prompt =====";
        const QString workflowMarker = "===== workflow =====";
        const int promptStart = raw.indexOf(promptMarker) + promptMarker.size();
        const int workflowStart = raw.indexOf(workflowMarker);
        check(promptStart >= promptMarker.size() && workflowStart > promptStart, "attachment contains both metadata sections");
        // Keep the original text and numeric tokens, including the actual seed.
        QImage image(16, 16, QImage::Format_RGB32);
        image.fill(Qt::blue);
        image.setText("prompt", raw.mid(promptStart, workflowStart - promptStart).trimmed());
        image.setText("workflow", raw.mid(workflowStart + workflowMarker.size()).trimmed());
        check(image.save(path, "PNG"), "save attachment metadata in temporary test image");
        const QByteArray before = readFile(path);
        verifySavedParameters(parseImageMetadataFromFile(path));
        check(readFile(path) == before, "attachment metadata stays unmodified");
        image.setText("prompt", "");
        check(image.save(path, "PNG"), "save workflow-only attachment fixture");
        verifySavedParameters(parseImageMetadataFromFile(path));
    }
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

void testModelBrowserUrls()
{
    using namespace ModelBrowser;
    const QUrl original("https://civitai.com/models/1317146/from-haison-shoujo?modelVersionId=987654#images");
    const QUrl red = preferredUrl(original, "civitai.red");
    check(red.host() == "civitai.red" && red.path() == original.path()
          && red.query() == original.query() && red.fragment() == original.fragment(),
          "Civitai browser preference preserves slug, version query and fragment");
    check(original.host() == "civitai.com", "browser resolution does not mutate its input");
    check(preferredUrl(red, "civitai.com") == original, "preference can switch back to civitai.com");
    check(preferredUrl(QUrl("http://WWW.CIVITAI.COM/models/1"), "civitai.red") == QUrl("https://civitai.red/models/1"),
          "legacy HTTP and www model links use the selected HTTPS site");
    check(normalizedCivitaiHost(" CIVITAI.COM ") == "civitai.com"
          && normalizedCivitaiHost("invalid.example") == "civitai.red", "unknown settings fall back to red");
    for (const QString &text : {QString("https://civarchive.com/sha256/abcdef"),
                               QString("https://civitai.com/api/v1/models/1317146"),
                               QString("https://civitai.com/api/download/models/987654"),
                               QString("https://civitai.com.evil.example/models/1"),
                               QString("https://example.com/models/1?redirect=civitai.com"),
                               QString("https://image.civitai.com/preview.png"),
                               QString("file:///models/local.html")}) {
        const QUrl url(text);
        check(!isCivitaiModelPage(url) && preferredUrl(url, "civitai.red") == url,
              "archive, custom, API, download and image URLs are never redirected");
    }
    const QJsonObject metadata = versionFixture();
    const QByteArray before = QJsonDocument(metadata).toJson();
    const QString canonical = ModelMetadata::metadataBrowserUrlFromRoot(metadata);
    check(preferredUrl(QUrl(canonical), "civitai.red").host() == "civitai.red"
          && ModelMetadata::metadataBrowserUrlFromRoot(metadata) == canonical
          && QJsonDocument(metadata).toJson() == before, "metadata keeps its original Civitai URL and fields");
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
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        std::fprintf(stderr, "%s\n", message.toUtf8().constData());
    });
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!dir.isValid()) return 1;
    const QString group = app.arguments().value(1);
    if (group == "metadata") testMetadata(dir);
    else if (group == "matching") testMatcher(dir);
    else if (group == "scanner") testScanner(dir);
    else if (group == "preview") testPreview(dir);
    else if (group == "comfy") testComfyModels(dir, app.arguments().value(2));
    else if (group == "tags") testTags(dir);
    else if (group == "archive") testArchive();
    else if (group == "paths_filters") testPathsAndFilters(dir);
    else if (group == "browser_urls") testModelBrowserUrls();
    else if (group == "inspection") testInspection(dir);
    else if (group == "downloads") testDownloadCache(dir);
    else return 2;
    qInfo() << group << (failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
