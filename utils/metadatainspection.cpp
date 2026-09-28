#include "metadatainspection.h"
#include "modelmetadatacodec.h"
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <utility>

namespace MetadataInspection {
namespace {

QString metadataIsoTimeForDisplay(const QString &iso)
{
    QDateTime dt = QDateTime::fromString(iso, Qt::ISODate);
    if (!dt.isValid()) return iso;
    return dt.toLocalTime().toString("yyyy-MM-dd HH:mm");
}

}

QVector<MetadataScanItem> scan(QVector<MetadataScanItem> items)
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
        const QString sha = ModelMetadata::metadataShaFromRoot(root);
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

QVector<MetadataHealthIssue> healthCheck(QVector<MetadataScanItem> items)
{
    QVector<MetadataHealthIssue> issues;
    const QVector<MetadataScanItem> scanned = scan(std::move(items));
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

}
