#include "imagemetadataparser.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>

#include <utility>

namespace {

bool looksLikeComfyPromptObject(const QJsonObject &obj);

QMap<QString, QString> readPngTextChunks(const QString &filePath)
{
    constexpr qint64 kMaxMetadataChunkBytes = 64LL * 1024LL * 1024LL;
    QMap<QString, QString> chunks;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return chunks;

    const QByteArray signature = file.read(8);
    const char pngSignature[] = {-119, 'P', 'N', 'G', 13, 10, 26, 10};
    if (signature != QByteArray::fromRawData(pngSignature, 8)) {
        return chunks;
    }

    while (!file.atEnd()) {
        const QByteArray lenData = file.read(4);
        if (lenData.size() < 4) break;
        const quint32 length = qFromBigEndian<quint32>(lenData.constData());
        const QByteArray type = file.read(4);
        if (type.size() < 4) break;

        const qint64 chunkLength = static_cast<qint64>(length);
        const qint64 remaining = file.size() - file.pos();
        if (remaining < chunkLength + 4) break; // payload + CRC must both exist

        const bool isMetadataChunk = type == "tEXt" || type == "iTXt" || type == "zTXt";
        if (!isMetadataChunk || chunkLength > kMaxMetadataChunkBytes) {
            if (!file.seek(file.pos() + chunkLength + 4)) break;
            if (type == "IEND") break;
            continue;
        }

        const QByteArray data = file.read(chunkLength);
        if (data.size() != chunkLength) break;

        if (type == "tEXt") {
            const int nullPos = data.indexOf('\0');
            if (nullPos != -1) {
                chunks.insert(QString::fromLatin1(data.left(nullPos)),
                              QString::fromUtf8(data.mid(nullPos + 1)));
            }
        } else if (type == "iTXt") {
            int pos = data.indexOf('\0');
            if (pos > 0 && pos + 5 < data.size()) {
                const QString keyword = QString::fromLatin1(data.left(pos));
                const uchar compressionFlag = static_cast<uchar>(data.at(pos + 1));
                pos += 1; // keyword terminator
                pos += 1; // compression flag
                pos += 1; // compression method
                const int languageEnd = data.indexOf('\0', pos);
                if (languageEnd != -1) {
                    const int translatedEnd = data.indexOf('\0', languageEnd + 1);
                    // Compressed iTXt is not interpreted as UTF-8. QImageReader
                    // remains available as a fallback for supported plugins.
                    if (translatedEnd != -1 && compressionFlag == 0) {
                        chunks.insert(keyword, QString::fromUtf8(data.mid(translatedEnd + 1)));
                    }
                }
            }
        }

        if (file.read(4).size() != 4) break; // CRC
    }

    return chunks;
}

QString jsonValueToString(const QJsonValue &value)
{
    if (value.isString()) return value.toString().trimmed();
    if (value.isDouble()) return QString::number(value.toDouble(), 'g', 12);
    if (value.isBool()) return value.toBool() ? "true" : "false";
    return QString();
}

QString linkedNodeId(const QJsonValue &value)
{
    if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        if (!arr.isEmpty()) return jsonValueToString(arr.first());
    }
    if (value.isString() || value.isDouble()) return jsonValueToString(value);
    return QString();
}

bool isMeaningfulPromptText(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return false;
    if (trimmed.compare("unknown", Qt::CaseInsensitive) == 0) return false;
    if (trimmed.compare("undefined", Qt::CaseInsensitive) == 0) return false;
    if (trimmed.compare("null", Qt::CaseInsensitive) == 0) return false;
    return true;
}

QString normalizeClassName(QString text)
{
    QString normalized;
    normalized.reserve(text.size());
    for (const QChar ch : std::as_const(text)) {
        if (ch.isLetterOrNumber()) normalized.append(ch.toCaseFolded());
    }
    return normalized;
}

bool classNameContains(const QString &className, const QString &needle)
{
    const QString normalizedClass = normalizeClassName(className);
    const QString normalizedNeedle = normalizeClassName(needle);
    return !normalizedNeedle.isEmpty() && normalizedClass.contains(normalizedNeedle);
}

bool classContains(const QJsonObject &node, const QString &needle)
{
    return classNameContains(node.value("class_type").toString(), needle);
}

QString checkpointNameFromNode(const QJsonObject &node)
{
    const QJsonObject inputs = node.value("inputs").toObject();
    const QStringList keys = {"ckpt_name", "checkpoint", "checkpoint_name", "model_name"};
    for (const QString &key : keys) {
        const QString name = jsonValueToString(inputs.value(key)).trimmed();
        if (!name.isEmpty()) return name;
    }
    return QString();
}

QStringList collectCheckpointNamesFromNodes(const QJsonObject &nodes)
{
    QStringList checkpoints;
    QSet<QString> seen;
    for (auto it = nodes.begin(); it != nodes.end(); ++it) {
        const QJsonObject node = it.value().toObject();
        if (!classContains(node, "CheckpointLoader")) continue;

        const QString checkpoint = checkpointNameFromNode(node);
        if (checkpoint.isEmpty()) continue;

        const QString key = checkpoint.toCaseFolded();
        if (seen.contains(key)) continue;
        seen.insert(key);
        checkpoints.append(checkpoint);
    }
    return checkpoints;
}

QString nodeTextInput(const QJsonObject &node)
{
    const QJsonObject inputs = node.value("inputs").toObject();
    const QStringList keys = {"text", "prompt", "positive_prompt", "negative_prompt"};
    for (const QString &key : keys) {
        const QJsonValue value = inputs.value(key);
        if (value.isString() && isMeaningfulPromptText(value.toString())) return value.toString().trimmed();
    }
    return QString();
}

QString traceTextFromNode(const QString &nodeId, const QJsonObject &nodes, QSet<QString> &visited)
{
    if (nodeId.isEmpty() || visited.contains(nodeId)) return QString();
    visited.insert(nodeId);

    const QJsonObject node = nodes.value(nodeId).toObject();
    if (node.isEmpty()) return QString();

    const QString directText = nodeTextInput(node);
    if (!directText.isEmpty()
        && (classContains(node, "CLIPTextEncode")
            || classContains(node, "Text")
            || classContains(node, "Prompt"))) {
        return directText;
    }

    const QJsonObject inputs = node.value("inputs").toObject();
    const QStringList linkKeys = {"text", "prompt", "conditioning", "positive", "negative"};
    for (const QString &key : linkKeys) {
        const QString linked = linkedNodeId(inputs.value(key));
        const QString traced = traceTextFromNode(linked, nodes, visited);
        if (!traced.isEmpty()) return traced;
    }

    if (!directText.isEmpty()) return directText;
    return QString();
}

QString formatLoraDescription(const QString &name, const QString &strength)
{
    const QString cleanName = name.trimmed();
    if (cleanName.isEmpty()) return QString();

    const QString cleanStrength = strength.trimmed();
    return cleanStrength.isEmpty() ? cleanName : QString("%1: %2").arg(cleanName, cleanStrength);
}

bool appendLorasFromStructuredValue(const QJsonValue &value, QStringList &loras)
{
    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        if (obj.contains("__value__")) {
            return appendLorasFromStructuredValue(obj.value("__value__"), loras);
        }
    }

    if (!value.isArray()) return false;

    bool sawStructuredEntry = false;
    const QJsonArray arr = value.toArray();
    for (const QJsonValue &entryValue : arr) {
        const QJsonObject entry = entryValue.toObject();
        if (entry.isEmpty()) continue;

        sawStructuredEntry = true;
        if (entry.contains("active") && !entry.value("active").toBool(false)) continue;

        const QString name = entry.value("name").toString().trimmed().isEmpty()
                                 ? entry.value("lora_name").toString().trimmed()
                                 : entry.value("name").toString().trimmed();
        QString strength = jsonValueToString(entry.value("strength"));
        if (strength.isEmpty()) strength = jsonValueToString(entry.value("modelStrength"));
        if (strength.isEmpty()) strength = jsonValueToString(entry.value("strength_model"));

        const QString description = formatLoraDescription(name, strength);
        if (!description.isEmpty()) loras.append(description);
    }

    return sawStructuredEntry;
}

QJsonValue structuredLoraWidgetValue(const QJsonArray &widgets)
{
    for (const QJsonValue &widget : widgets) {
        QStringList unused;
        if (appendLorasFromStructuredValue(widget, unused)) return widget;
    }
    return QJsonValue();
}

void collectLorasFromNode(const QString &nodeId,
                          const QJsonObject &nodes,
                          QSet<QString> &visited,
                          QStringList &loras,
                          QString &checkpoint,
                          QMap<QString, QString> &hashes)
{
    if (nodeId.isEmpty() || visited.contains(nodeId)) return;
    visited.insert(nodeId);

    const QJsonObject node = nodes.value(nodeId).toObject();
    if (node.isEmpty()) return;

    const QJsonObject inputs = node.value("inputs").toObject();
    if (classContains(node, "LoraLoader")) {
        QString loraNameForHash;
        bool handledStructured = appendLorasFromStructuredValue(inputs.value("loras"), loras);
        if (!handledStructured) handledStructured = appendLorasFromStructuredValue(inputs.value("lora_stack"), loras);

        if (!handledStructured) {
            const QString loraName = inputs.value("lora_name").toString().trimmed();
            loraNameForHash = loraName;
            QString strength = jsonValueToString(inputs.value("strength_model"));
            if (strength.isEmpty()) strength = jsonValueToString(inputs.value("strength"));

            const QString description = formatLoraDescription(loraName, strength);
            if (!description.isEmpty()) loras.append(description);
        }

        const QString hash = inputs.value("hash").toString().trimmed();
        if (!loraNameForHash.isEmpty() && !hash.isEmpty()) hashes.insert(QFileInfo(loraNameForHash).completeBaseName(), hash);
    }

    if (classContains(node, "CheckpointLoader") && checkpoint.isEmpty()) {
        checkpoint = checkpointNameFromNode(node);
    }

    const QStringList linkKeys = {"model", "clip", "base_model", "lora_stack"};
    for (const QString &key : linkKeys) {
        collectLorasFromNode(linkedNodeId(inputs.value(key)), nodes, visited, loras, checkpoint, hashes);
    }
}

QString joinKeyValues(const QStringList &parts)
{
    QStringList cleaned;
    for (const QString &part : parts) {
        if (!part.trimmed().isEmpty()) cleaned.append(part.trimmed());
    }
    return cleaned.join(", ");
}

QString formatComfyParameters(const ParsedImageMetadata &meta)
{
    QStringList lines;
    lines << "Source: ComfyUI";
    if (!meta.seed.isEmpty()) lines << "Seed: " + meta.seed;
    if (!meta.steps.isEmpty()) lines << "Steps: " + meta.steps;
    if (!meta.cfg.isEmpty()) lines << "CFG scale: " + meta.cfg;
    if (!meta.sampler.isEmpty()) lines << "Sampler: " + meta.sampler;
    if (!meta.scheduler.isEmpty()) lines << "Scheduler: " + meta.scheduler;
    if (!meta.checkpoint.isEmpty()) lines << "Checkpoint: " + meta.checkpoint;
    if (!meta.loraDescriptions.isEmpty()) lines << "ComfyUI LoRAs: " + meta.loraDescriptions.join(", ");
    if (!meta.loraHashes.isEmpty()) {
        QStringList hashParts;
        for (auto it = meta.loraHashes.begin(); it != meta.loraHashes.end(); ++it) {
            hashParts << QString("%1: %2").arg(it.key(), it.value());
        }
        lines << "ComfyUI Lora hashes: " + hashParts.join(", ");
    }
    return lines.join('\n');
}

ParsedImageMetadata parseA1111Text(const QString &text)
{
    ParsedImageMetadata meta;
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return meta;

    meta.sourceType = "Parameters";
    const int stepsIndex = trimmed.lastIndexOf("Steps: ");
    if (stepsIndex == -1) {
        meta.positivePrompt = trimmed;
        meta.parametersText.clear();
        return meta;
    }

    meta.parametersText = trimmed.mid(stepsIndex).trimmed();
    const QString beforeParams = trimmed.left(stepsIndex).trimmed();
    const int negIndex = beforeParams.indexOf("Negative prompt:");
    if (negIndex != -1) {
        meta.positivePrompt = beforeParams.left(negIndex).trimmed();
        meta.negativePrompt = beforeParams.mid(negIndex + 16).trimmed();
    } else {
        meta.positivePrompt = beforeParams.trimmed();
        meta.negativePrompt = "(empty)";
    }
    return meta;
}

QJsonObject parseJsonObject(const QString &text)
{
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return QJsonObject();
    return doc.object();
}

bool isOrLooksLikeJsonContainer(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return false;

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &error);
    if (error.error == QJsonParseError::NoError && !doc.isNull()) return true;

    // Broken/incomplete workflow JSON must not fall through to prompt tags.
    return trimmed.startsWith('{') || trimmed.startsWith('[');
}

bool looksLikeA1111Parameters(const QString &text)
{
    static const QRegularExpression stepsLine(
        QStringLiteral("(?:^|\\n)Steps\\s*:\\s*[^\\n,]+"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression generationField(
        QStringLiteral("(?:Sampler|CFG scale|Seed|Size|Model)\\s*:"),
        QRegularExpression::CaseInsensitiveOption);
    return stepsLine.match(text).hasMatch() && generationField.match(text).hasMatch();
}

QJsonObject comfyPromptNodesFromRoot(const QJsonObject &root)
{
    if (looksLikeComfyPromptObject(root)) return root;

    const QJsonObject promptObj = root.value("prompt").toObject();
    if (looksLikeComfyPromptObject(promptObj)) return promptObj;

    const QJsonObject workflowPromptObj = root.value("workflow").toObject().value("prompt").toObject();
    if (looksLikeComfyPromptObject(workflowPromptObj)) return workflowPromptObj;

    return QJsonObject();
}

bool looksLikeComfyPromptObject(const QJsonObject &obj)
{
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const QJsonObject node = it.value().toObject();
        if (node.contains("class_type") && node.contains("inputs")) return true;
    }
    return false;
}

ParsedImageMetadata parseComfyPromptObject(const QJsonObject &nodes)
{
    ParsedImageMetadata meta;
    if (!looksLikeComfyPromptObject(nodes)) return meta;

    QString samplerId;
    for (auto it = nodes.begin(); it != nodes.end(); ++it) {
        const QJsonObject node = it.value().toObject();
        if (classContains(node, "KSampler")) samplerId = it.key();
    }
    if (samplerId.isEmpty()) return meta;

    const QJsonObject sampler = nodes.value(samplerId).toObject();
    const QJsonObject inputs = sampler.value("inputs").toObject();

    QSet<QString> visitedPositive;
    QSet<QString> visitedNegative;
    meta.positivePrompt = traceTextFromNode(linkedNodeId(inputs.value("positive")), nodes, visitedPositive);
    meta.negativePrompt = traceTextFromNode(linkedNodeId(inputs.value("negative")), nodes, visitedNegative);
    meta.seed = jsonValueToString(inputs.value(inputs.contains("noise_seed") ? "noise_seed" : "seed"));
    meta.steps = jsonValueToString(inputs.value("steps"));
    meta.cfg = jsonValueToString(inputs.value("cfg"));
    meta.sampler = jsonValueToString(inputs.value("sampler_name"));
    meta.scheduler = jsonValueToString(inputs.value("scheduler"));

    QSet<QString> visitedModel;
    collectLorasFromNode(linkedNodeId(inputs.value("model")), nodes, visitedModel,
                         meta.loraDescriptions, meta.checkpoint, meta.loraHashes);

    if (meta.checkpoint.isEmpty()) {
        const QStringList checkpoints = collectCheckpointNamesFromNodes(nodes);
        if (!checkpoints.isEmpty()) {
            // Some ComfyUI exports omit or obscure the model link chain. Keep the
            // KSampler-traced value when available, otherwise expose a stable
            // Checkpoint: line so gallery checkpoint matching can still work.
            meta.checkpoint = checkpoints.first();
        }
    }

    if (meta.hasContent() || !meta.loraDescriptions.isEmpty() || !meta.checkpoint.isEmpty()) {
        meta.sourceType = "ComfyUI";
        meta.parametersText = formatComfyParameters(meta);
    }
    return meta;
}

void mergeComfyMetadata(ParsedImageMetadata &target, const ParsedImageMetadata &supplement)
{
    auto fillMissing = [](QString &value, const QString &fallback) {
        if (value.trimmed().isEmpty() && !fallback.trimmed().isEmpty()) value = fallback;
    };

    fillMissing(target.positivePrompt, supplement.positivePrompt);
    fillMissing(target.negativePrompt, supplement.negativePrompt);
    fillMissing(target.seed, supplement.seed);
    fillMissing(target.steps, supplement.steps);
    fillMissing(target.cfg, supplement.cfg);
    fillMissing(target.sampler, supplement.sampler);
    fillMissing(target.scheduler, supplement.scheduler);
    fillMissing(target.checkpoint, supplement.checkpoint);

    QSet<QString> seenLoras;
    for (const QString &description : std::as_const(target.loraDescriptions)) {
        seenLoras.insert(description.trimmed().toCaseFolded());
    }
    for (const QString &description : supplement.loraDescriptions) {
        const QString key = description.trimmed().toCaseFolded();
        if (key.isEmpty() || seenLoras.contains(key)) continue;
        seenLoras.insert(key);
        target.loraDescriptions.append(description);
    }
    for (auto it = supplement.loraHashes.constBegin(); it != supplement.loraHashes.constEnd(); ++it) {
        if (!target.loraHashes.contains(it.key())) target.loraHashes.insert(it.key(), it.value());
    }

    if (target.hasContent() || !target.loraDescriptions.isEmpty() || !target.checkpoint.isEmpty()) {
        target.sourceType = "ComfyUI";
        target.parametersText = formatComfyParameters(target);
    }
}

QJsonObject promptObjectFromWorkflowGraph(const QJsonObject &workflow)
{
    const QJsonArray workflowNodes = workflow.value("nodes").toArray();
    if (workflowNodes.isEmpty()) return QJsonObject();

    QHash<int, QString> outputLinkToNodeId;
    for (const QJsonValue &value : workflowNodes) {
        const QJsonObject node = value.toObject();
        const QString nodeId = jsonValueToString(node.value("id"));
        if (nodeId.isEmpty()) continue;

        const QJsonArray outputs = node.value("outputs").toArray();
        for (const QJsonValue &outputValue : outputs) {
            const QJsonArray links = outputValue.toObject().value("links").toArray();
            for (const QJsonValue &linkValue : links) {
                outputLinkToNodeId.insert(linkValue.toInt(-1), nodeId);
            }
        }
    }

    QJsonObject promptLike;
    for (const QJsonValue &value : workflowNodes) {
        const QJsonObject node = value.toObject();
        const QString nodeId = jsonValueToString(node.value("id"));
        if (nodeId.isEmpty()) continue;

        QJsonObject converted;
        const QString classType = node.value("type").toString();
        converted.insert("class_type", classType);

        QJsonObject inputs;
        const QJsonArray widgets = node.value("widgets_values").toArray();
        if (classNameContains(classType, "CLIPTextEncode")
            || classNameContains(classType, "Text")
            || classNameContains(classType, "Prompt")) {
            for (const QJsonValue &widget : widgets) {
                const QString text = jsonValueToString(widget);
                if (isMeaningfulPromptText(text)) {
                    inputs.insert("text", text);
                    break;
                }
            }
        } else if (classNameContains(classType, "KSampler")) {
            const QStringList samplerKeys = {"seed", "steps", "cfg", "sampler_name", "scheduler"};
            for (int i = 0; i < samplerKeys.size() && i < widgets.size(); ++i) {
                inputs.insert(samplerKeys.at(i), widgets.at(i));
            }
        } else if (classNameContains(classType, "CheckpointLoader")) {
            for (const QJsonValue &widget : widgets) {
                const QString checkpoint = jsonValueToString(widget);
                if (!checkpoint.isEmpty()) {
                    inputs.insert("ckpt_name", checkpoint);
                    break;
                }
            }
        } else if (classNameContains(classType, "LoraLoader")) {
            const QJsonValue structured = structuredLoraWidgetValue(widgets);
            if (!structured.isUndefined()) {
                inputs.insert("loras", structured);
            } else {
                if (!widgets.isEmpty()) inputs.insert("lora_name", widgets.at(0));
                if (widgets.size() > 1) inputs.insert("strength_model", widgets.at(1));
            }
        }

        const QJsonArray graphInputs = node.value("inputs").toArray();
        for (const QJsonValue &inputValue : graphInputs) {
            const QJsonObject input = inputValue.toObject();
            const int linkId = input.value("link").toInt(-1);
            if (linkId < 0 || !outputLinkToNodeId.contains(linkId)) continue;
            const QString name = input.value("name").toString();
            if (name.isEmpty()) continue;
            QJsonArray link;
            link.append(outputLinkToNodeId.value(linkId));
            link.append(0);
            inputs.insert(name, link);
        }

        converted.insert("inputs", inputs);
        promptLike.insert(nodeId, converted);
    }

    return promptLike;
}

ParsedImageMetadata parseComfyMetadata(const QString &promptJson, const QString &workflowJson)
{
    ParsedImageMetadata meta = parseComfyPromptObject(comfyPromptNodesFromRoot(parseJsonObject(promptJson)));

    const QJsonObject workflow = parseJsonObject(workflowJson);
    const ParsedImageMetadata workflowPrompt = parseComfyPromptObject(comfyPromptNodesFromRoot(workflow));
    mergeComfyMetadata(meta, workflowPrompt);

    const ParsedImageMetadata workflowGraph = parseComfyPromptObject(promptObjectFromWorkflowGraph(workflow));
    mergeComfyMetadata(meta, workflowGraph);
    return meta;
}

} // namespace

QString extractPngParametersText(const QString &filePath)
{
    const QMap<QString, QString> chunks = extractImageMetadataTextChunks(filePath);
    if (chunks.contains("parameters")) return chunks.value("parameters");

    QImageReader reader(filePath);
    if (!reader.canRead()) return QString();
    return reader.text("parameters");
}

QMap<QString, QString> extractImageMetadataTextChunks(const QString &filePath)
{
    QMap<QString, QString> chunks = readPngTextChunks(filePath);

    QImageReader reader(filePath);
    if (!reader.canRead()) return chunks;

    const QStringList keys = reader.textKeys();
    for (const QString &key : keys) {
        if (!chunks.contains(key)) chunks.insert(key, reader.text(key));
    }

    return chunks;
}

ParsedImageMetadata parseImageMetadataFromFile(const QString &filePath)
{
    const QMap<QString, QString> chunks = readPngTextChunks(filePath);

    QString parameters = chunks.value("parameters");
    QString promptJson = chunks.value("prompt");
    QString workflowJson = chunks.value("workflow");

    QImageReader reader(filePath);
    if (reader.canRead()) {
        if (parameters.isEmpty()) parameters = reader.text("parameters");
        if (promptJson.isEmpty()) promptJson = reader.text("prompt");
        if (workflowJson.isEmpty()) workflowJson = reader.text("workflow");
    }

    if (!parameters.trimmed().isEmpty()) {
        const QJsonObject parametersJson = parseJsonObject(parameters);
        if (!parametersJson.isEmpty()) {
            ParsedImageMetadata comfy = parseComfyMetadata(promptJson, workflowJson);
            const ParsedImageMetadata parametersMeta =
                parseComfyPromptObject(comfyPromptNodesFromRoot(parametersJson));
            mergeComfyMetadata(comfy, parametersMeta);
            if (comfy.hasContent()) return comfy;
            // Some exporters put an unrelated JSON object in `parameters` but
            // keep the real ComfyUI graph in `prompt`/`workflow`.
            // Continue probing those chunks instead of treating JSON as tags.
        } else if (!isOrLooksLikeJsonContainer(parameters) || looksLikeA1111Parameters(parameters)) {
            return parseA1111Text(parameters);
        }
    }

    ParsedImageMetadata comfy = parseComfyMetadata(promptJson, workflowJson);
    if (comfy.hasContent()) return comfy;

    ParsedImageMetadata fallback;
    const QString plainPrompt = promptJson.trimmed();
    if (!plainPrompt.isEmpty()
        && !isOrLooksLikeJsonContainer(plainPrompt)
        && isMeaningfulPromptText(plainPrompt)) {
        fallback.sourceType = "Prompt";
        fallback.positivePrompt = plainPrompt;
    }
    return fallback;
}
