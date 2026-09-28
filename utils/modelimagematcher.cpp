#include "modelimagematcher.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace ModelImageMatcher {
namespace {

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

static bool parametersAreFromComfyWorker(const QString &parameters)
{
    if (parameters.isEmpty()) return false;

    static QRegularExpression sourceRegex("(?:^|[\\n\\r])\\s*Source\\s*:\\s*ComfyUI\\b",
                                          QRegularExpression::CaseInsensitiveOption);
    return sourceRegex.match(parameters).hasMatch();
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

static bool nameSetsIntersectWorker(const QSet<QString> &imageNames, const QSet<QString> &targetNames)
{
    if (imageNames.isEmpty() || targetNames.isEmpty()) return false;
    for (const QString &name : targetNames) {
        if (imageNames.contains(name)) return true;
    }
    return false;
}

static void addLoraNameVariantsWorker(const QString &name, QSet<QString> &out)
{
    QString coreName = name.trimmed();
    if (coreName.isEmpty()) return;
    if (coreName.contains("[")) coreName = coreName.split("[").first().trimmed();
    if (coreName.endsWith(".safetensors", Qt::CaseInsensitive)
        || coreName.endsWith(".ckpt", Qt::CaseInsensitive)
        || coreName.endsWith(".pt", Qt::CaseInsensitive)) {
        coreName = QFileInfo(coreName).completeBaseName();
    }
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

}
QString safetensorsInternalName(const QString &path)
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

ModelUsageCandidate buildCandidate(const ModelUsageInput &model)
{
    ModelUsageCandidate candidate;
    candidate.filePath = model.filePath;
    candidate.baseName = model.baseName;
    candidate.isCheckpoint = isCheckpointModelType(model.type, model.filePath);

    const QString internalName = safetensorsInternalName(model.filePath);
    if (!internalName.isEmpty()) {
        addLoraNameVariantsWorker(internalName, candidate.normalizedLoraNames);
    }
    // ComfyUI commonly stores the disk filename rather than ss_output_name.
    addLoraNameVariantsWorker(model.baseName, candidate.normalizedLoraNames);

    addLoraNameVariantsWorker(model.baseName, candidate.normalizedCheckpointNames);
    for (const QString &name : splitCivitaiFullNameForMatch(model.civitaiName)) {
        addLoraNameVariantsWorker(name, candidate.normalizedCheckpointNames);
    }
    if (!internalName.isEmpty()) {
        addLoraNameVariantsWorker(internalName, candidate.normalizedCheckpointNames);
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
        if (candidate.isCheckpoint) {
            candidate.checkpointHashes.unite(collectCheckpointHashesFromJsonFileWorker(path));
        } else {
            candidate.summaryHashes.unite(collectLoraSummaryHashesFromJsonFileWorker(path));
        }
    }
    if (!model.sha256.trimmed().isEmpty()) {
        const QString normalized = normalizeSummaryHashForMatch(model.sha256);
        if (!normalized.isEmpty()) {
            if (candidate.isCheckpoint) candidate.checkpointHashes.insert(normalized);
            else candidate.summaryHashes.insert(normalized);
        }
    }

    return candidate;
}

CachedImageUsageInfo imageUsage(const UserImageInfo &info)
{
    CachedImageUsageInfo cached;
    QStringList usedNames = extractLoraNamesFromPromptWorker(info.prompt);
    usedNames.append(extractLoraNamesFromMetadataWorker(info.parameters));
    for (const QString &usedName : usedNames) {
        QSet<QString> usedVariants;
        addLoraNameVariantsWorker(usedName, usedVariants);
        if (usedVariants.isEmpty()) {
            const QString normalized = normalizeLoraNameForMatch(usedName);
            if (!normalized.isEmpty()) usedVariants.insert(normalized);
        }
        for (const QString &variant : usedVariants) cached.usedLoraNames.insert(variant);
    }
    cached.loraHashes = extractLoraHashValuesFromParametersWorker(info.parameters);
    cached.isComfy = parametersAreFromComfyWorker(info.parameters);
    const QStringList checkpointNames = extractCheckpointNamesFromParametersWorker(info.parameters);
    for (const QString &checkpointName : checkpointNames) {
        const QString normalized = normalizeLoraNameForMatch(checkpointName);
        if (!normalized.isEmpty()) cached.usedCheckpointNames.insert(normalized);
    }
    cached.checkpointHashes = extractCheckpointHashValuesFromParametersWorker(info.parameters);
    cached.lastModified = info.lastModified;

    return cached;
}

bool matches(const ModelUsageCandidate &model, const CachedImageUsageInfo &image,
             const MatchOptions &options)
{
    const QSet<QString> &targetHashes = model.isCheckpoint ? model.checkpointHashes : model.summaryHashes;
    const QSet<QString> &imageHashes = model.isCheckpoint ? image.checkpointHashes : image.loraHashes;
    const QSet<QString> &targetNames = model.isCheckpoint ? model.normalizedCheckpointNames : model.normalizedLoraNames;
    const QSet<QString> &imageNames = model.isCheckpoint ? image.usedCheckpointNames : image.usedLoraNames;
    if (options.mode == 0 || (options.mode == 1 && targetHashes.isEmpty()))
        return nameSetsIntersectWorker(imageNames, targetNames);
    if (hashSetsMatchByPrefixWorker(imageHashes, targetHashes)) return true;
    return options.comfyNameFallback && image.isComfy && imageHashes.isEmpty()
           && nameSetsIntersectWorker(imageNames, targetNames);
}

QList<ModelUsageStatResult> calculateUsage(const QList<ModelUsageInput> &models,
                                          const QMap<QString, UserImageInfo> &cache,
                                          int matchMode, bool comfyNameFallback)
{
    QList<CachedImageUsageInfo> images;
    images.reserve(cache.size());
    for (auto it = cache.cbegin(); it != cache.cend(); ++it) images.append(imageUsage(it.value()));
    QList<ModelUsageStatResult> result;
    result.reserve(models.size());
    const MatchOptions options{matchMode, comfyNameFallback};
    for (const ModelUsageInput &input : models) {
        const ModelUsageCandidate model = buildCandidate(input);
        ModelUsageStatResult stat;
        stat.filePath = input.filePath;
        for (const CachedImageUsageInfo &image : images) {
            if (matches(model, image, options)) {
                ++stat.usageCount;
                stat.lastUsed = qMax(stat.lastUsed, image.lastModified);
            }
        }
        result.append(stat);
    }
    return result;
}
}
