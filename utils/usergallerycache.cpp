#include "usergallerycache.h"
#include "gallerymetadata.h"
#include "tagutils.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>

namespace UserGalleryCache {

QMap<QString, UserImageInfo> load(const QString &cachePath,
                                  bool splitOnNewline,
                                  const QStringList &filterTags)
{
    QMap<QString, UserImageInfo> result;
    QFile file(cachePath);
    if (!file.open(QIODevice::ReadOnly)) return result;

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "Invalid user gallery cache JSON:" << error.errorString();
        return result;
    }

    const QJsonObject root = doc.object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!it.value().isObject()) continue;
        const QJsonObject obj = it.value().toObject();
        UserImageInfo info;
        info.path = it.key();
        info.prompt = obj.value(QStringLiteral("p")).toString();
        info.negativePrompt = obj.value(QStringLiteral("np")).toString();
        info.parameters = obj.value(QStringLiteral("param")).toString();
        info.lastModified = obj.value(QStringLiteral("t")).toVariant().toLongLong();
        info.parserVersion = obj.value(QStringLiteral("pv")).toInt(0);
        info.cleanTags = TagUtils::parsePromptTags(info.prompt, splitOnNewline, filterTags);
        info.negativeCleanTags = TagUtils::parsePromptTags(info.negativePrompt, splitOnNewline, filterTags);
        info.checkpointName = obj.value(QStringLiteral("ckpt")).toString();
        info.checkpointHash = obj.value(QStringLiteral("ckpt_h")).toString();
        info.isComfy = obj.value(QStringLiteral("comfy")).toBool(false);

        if (obj.contains(QStringLiteral("loras")) && obj.value(QStringLiteral("loras")).isArray()) {
            const QJsonArray lorasArr = obj.value(QStringLiteral("loras")).toArray();
            info.parsedLoras.reserve(lorasArr.size());
            for (const QJsonValue &v : lorasArr) {
                const QJsonObject lo = v.toObject();
                ParsedLoraEntry entry;
                entry.name = lo.value(QStringLiteral("n")).toString();
                entry.hash = lo.value(QStringLiteral("h")).toString();
                entry.weight = lo.value(QStringLiteral("w")).toString();
                if (!entry.name.isEmpty()) info.parsedLoras.append(entry);
            }
        }
        if (obj.contains(QStringLiteral("extra_lh")) && obj.value(QStringLiteral("extra_lh")).isArray()) {
            const QJsonArray extraArr = obj.value(QStringLiteral("extra_lh")).toArray();
            info.extraLoraHashes.reserve(extraArr.size());
            for (const QJsonValue &v : extraArr) {
                const QString h = v.toString().trimmed();
                if (!h.isEmpty()) info.extraLoraHashes.append(h);
            }
        }
        if (!obj.value(QStringLiteral("pm")).toBool(false)) {
            continue;
        }
        info.parsedModelsReady = true;
        result.insert(info.path, info);
    }
    return result;
}

bool save(const QString &cachePath, const QMap<QString, UserImageInfo> &imageCache)
{
    const QFileInfo fi(cachePath);
    if (!fi.absolutePath().isEmpty()) {
        QDir().mkpath(fi.absolutePath());
    }

    QJsonObject root;
    for (auto it = imageCache.cbegin(); it != imageCache.cend(); ++it) {
        UserImageInfo info = it.value();
        if (!info.parsedModelsReady) {
            GalleryMetadata::populateParsedModelFields(info);
        }
        QJsonObject obj;
        obj[QStringLiteral("p")] = info.prompt;
        obj[QStringLiteral("np")] = info.negativePrompt;
        obj[QStringLiteral("param")] = info.parameters;
        obj[QStringLiteral("t")] = QString::number(info.lastModified); // 存为字符串避免精度问题
        obj[QStringLiteral("pv")] = info.parserVersion;
        if (!info.checkpointName.isEmpty()) obj[QStringLiteral("ckpt")] = info.checkpointName;
        if (!info.checkpointHash.isEmpty()) obj[QStringLiteral("ckpt_h")] = info.checkpointHash;
        if (info.isComfy) obj[QStringLiteral("comfy")] = true;
        if (!info.parsedLoras.isEmpty()) {
            QJsonArray lorasArr;
            for (const ParsedLoraEntry &entry : std::as_const(info.parsedLoras)) {
                QJsonObject lo;
                lo[QStringLiteral("n")] = entry.name;
                if (!entry.hash.isEmpty()) lo[QStringLiteral("h")] = entry.hash;
                if (!entry.weight.isEmpty()) lo[QStringLiteral("w")] = entry.weight;
                lorasArr.append(lo);
            }
            obj[QStringLiteral("loras")] = lorasArr;
        }
        if (!info.extraLoraHashes.isEmpty()) {
            QJsonArray extraArr;
            for (const QString &h : std::as_const(info.extraLoraHashes)) extraArr.append(h);
            obj[QStringLiteral("extra_lh")] = extraArr;
        }
        if (info.parsedModelsReady) obj[QStringLiteral("pm")] = true;

        root.insert(info.path, obj);
    }

    QSaveFile file(cachePath);
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(payload) != payload.size()
        || !file.commit()) {
        qWarning() << "Unable to save user gallery cache atomically:" << file.errorString();
        return false;
    }
    return true;
}

} // namespace UserGalleryCache
