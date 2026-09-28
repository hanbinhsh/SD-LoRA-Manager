#pragma once
#include "core/modeltypes.h"
#include <QSet>
#include <QMap>

namespace ModelImageMatcher {
struct CachedImageUsageInfo {
    QSet<QString> usedLoraNames;
    QSet<QString> loraHashes;
    QSet<QString> usedCheckpointNames;
    QSet<QString> checkpointHashes;
    bool isComfy = false;
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
struct MatchOptions {
    int mode = 0; // 0: name, 1: hash if available, 2: strict hash
    bool comfyNameFallback = true;
};
QString safetensorsInternalName(const QString &path);
// File access is confined to candidate construction; matches() is pure.
ModelUsageCandidate buildCandidate(const ModelUsageInput &model);
CachedImageUsageInfo imageUsage(const UserImageInfo &info);
bool matches(const ModelUsageCandidate &model, const CachedImageUsageInfo &image, const MatchOptions &options);
QList<ModelUsageStatResult> calculateUsage(const QList<ModelUsageInput> &models,
                                          const QMap<QString, UserImageInfo> &cache,
                                          int matchMode, bool comfyNameFallback);
}
